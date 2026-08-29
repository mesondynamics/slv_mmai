#include "atecc608.h"

#include <stddef.h>
#include <string.h>

#include "main.h"
#include "software_i2c.h"

#define ATECC608_ADDRESS_7BIT          0x60U
#define ATECC608_WORD_COMMAND          0x03U
#define ATECC608_WORD_SLEEP            0x01U
#define ATECC608_OPCODE_READ           0x02U
#define ATECC608_OPCODE_NONCE          0x16U
#define ATECC608_OPCODE_RANDOM         0x1BU
#define ATECC608_OPCODE_INFO           0x30U
#define ATECC608_OPCODE_GENKEY         0x40U
#define ATECC608_OPCODE_SIGN           0x41U
#define ATECC608_OPCODE_WRITE          0x12U
#define ATECC608_OPCODE_LOCK           0x17U
#define ATECC608_ZONE_CONFIG_32        0x80U
#define ATECC608_LOCK_NO_CRC           0x80U
#define ATECC608_COMMAND_OVERHEAD_SIZE    7U
#define ATECC608_MAX_COMMAND_DATA_SIZE   64U
#define ATECC608_RESPONSE_4_SIZE          7U
#define ATECC608_RESPONSE_32_SIZE        35U
#define ATECC608_RESPONSE_64_SIZE        67U
#define ATECC608_WAKE_RESPONSE_SIZE       4U
#define ATECC608_READ_TIMEOUT_MS          8U
#define ATECC608_INFO_TIMEOUT_MS          8U
#define ATECC608_RANDOM_TIMEOUT_MS       30U
#define ATECC608_NONCE_TIMEOUT_MS        30U
/* Covers all ATECC608C execution-speed modes, including the slowest M2
   silicon setting listed by Microchip CryptoAuthLib. */
#define ATECC608_SIGN_TIMEOUT_MS        700U
#define ATECC608_GENKEY_TIMEOUT_MS      700U
#define ATECC608_WRITE_TIMEOUT_MS        60U
#define ATECC608_LOCK_TIMEOUT_MS         50U
#define ATECC608_POLL_INTERVAL_MS         1U
#define ATECC608_NONCE_PASSTHROUGH_MESSAGE_DIGEST 0x43U
#define ATECC608_SIGN_EXTERNAL_MESSAGE_DIGEST     0xA0U
#define ATECC608_RANDOM_SEED_UPDATE       0x00U
#define ATECC608_MAX_SLOT                 15U
#define ATECC608_LOCK_VALUE_OFFSET       86U
#define ATECC608_LOCK_CONFIG_OFFSET      87U
#define ATECC608_I2C_ADDRESS_OFFSET      16U
#define ATECC608_LOCKED_VALUE          0x00U
#define ATECC608_SLOT_LOCKED_OFFSET      88U

static uint16_t ATECC608_Crc16Update(uint16_t crc, const uint8_t *data,
                                     size_t length)
{
  size_t byte_index;
  uint8_t bit_mask;

  for (byte_index = 0U; byte_index < length; ++byte_index)
  {
    for (bit_mask = 0x01U; bit_mask != 0U; bit_mask <<= 1U)
    {
      uint8_t data_bit = ((data[byte_index] & bit_mask) != 0U) ? 1U : 0U;
      uint8_t crc_bit = (uint8_t)(crc >> 15U);
      crc <<= 1U;
      if (data_bit != crc_bit)
      {
        crc ^= 0x8005U;
      }
    }
  }
  return crc;
}

static void ATECC608_Crc16(const uint8_t *data, size_t length,
                           uint8_t crc_le[2])
{
  uint16_t crc = ATECC608_Crc16Update(0U, data, length);
  crc_le[0] = (uint8_t)crc;
  crc_le[1] = (uint8_t)(crc >> 8U);
}

static uint32_t ATECC608_Crc32c(const uint8_t *data, size_t length)
{
  uint32_t crc = 0xFFFFFFFFUL;
  size_t index;
  uint32_t bit;

  for (index = 0U; index < length; ++index)
  {
    crc ^= data[index];
    for (bit = 0U; bit < 8U; ++bit)
    {
      uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0x82F63B78UL & mask);
    }
  }
  return ~crc;
}

static bool ATECC608_ResponseCrcValid(const uint8_t *response,
                                      size_t capacity)
{
  uint8_t expected[2];
  uint8_t count;

  if ((response == NULL) || (capacity < ATECC608_WAKE_RESPONSE_SIZE))
  {
    return false;
  }
  count = response[0];
  if ((count < ATECC608_WAKE_RESPONSE_SIZE) || (count > capacity))
  {
    return false;
  }
  ATECC608_Crc16(response, (size_t)count - 2U, expected);
  return (expected[0] == response[count - 2U]) &&
         (expected[1] == response[count - 1U]);
}

static void ATECC608_Sleep(void)
{
  const uint8_t sleep_word = ATECC608_WORD_SLEEP;
  (void)SoftwareI2C_Write(ATECC608_ADDRESS_7BIT, &sleep_word, 1U);
}

static int32_t ATECC608_Wake(void)
{
  static const uint8_t expected[ATECC608_WAKE_RESPONSE_SIZE] = {
    0x04U, 0x11U, 0x33U, 0x43U
  };
  uint8_t response[ATECC608_WAKE_RESPONSE_SIZE];

  if (!SoftwareI2C_WakeToken())
  {
    return ATECC608_RESULT_BUS;
  }
  if (!SoftwareI2C_Read(ATECC608_ADDRESS_7BIT, response, sizeof(response)))
  {
    return ATECC608_RESULT_WAKE;
  }
  return (memcmp(response, expected, sizeof(expected)) == 0) ?
         ATECC608_RESULT_OK : ATECC608_RESULT_WAKE;
}

static bool ATECC608_DeadlineReached(uint32_t deadline)
{
  return (int32_t)(HAL_GetTick() - deadline) >= 0;
}

static int32_t ATECC608_ExecuteAwake(
    uint8_t opcode, uint8_t param1, uint16_t param2,
    const uint8_t *command_data, size_t command_data_size,
    uint32_t timeout_ms, uint8_t *response, size_t response_capacity,
    uint8_t *device_status)
{
  uint8_t transmit[1U + ATECC608_COMMAND_OVERHEAD_SIZE +
                   ATECC608_MAX_COMMAND_DATA_SIZE];
  size_t command_size;
  uint32_t deadline;
  bool received = false;

  if ((response == NULL) ||
      (response_capacity < ATECC608_WAKE_RESPONSE_SIZE) ||
      (command_data_size > ATECC608_MAX_COMMAND_DATA_SIZE) ||
      ((command_data == NULL) && (command_data_size != 0U)))
  {
    return ATECC608_RESULT_BAD_ARGUMENT;
  }
  command_size = ATECC608_COMMAND_OVERHEAD_SIZE + command_data_size;
  transmit[0] = ATECC608_WORD_COMMAND;
  transmit[1] = (uint8_t)command_size;
  transmit[2] = opcode;
  transmit[3] = param1;
  transmit[4] = (uint8_t)param2;
  transmit[5] = (uint8_t)(param2 >> 8U);
  if (command_data_size != 0U)
  {
    memcpy(&transmit[6], command_data, command_data_size);
  }
  ATECC608_Crc16(&transmit[1], command_size - 2U,
                 &transmit[6U + command_data_size]);
  if (!SoftwareI2C_Write(ATECC608_ADDRESS_7BIT, transmit,
                         1U + command_size))
  {
    return ATECC608_RESULT_TRANSMIT;
  }
  deadline = HAL_GetTick() + timeout_ms;
  do
  {
    HAL_Delay(ATECC608_POLL_INTERVAL_MS);
    memset(response, 0, response_capacity);
    received = SoftwareI2C_Read(ATECC608_ADDRESS_7BIT, response,
                                response_capacity);
  } while (!received && !ATECC608_DeadlineReached(deadline));
  if (!received)
  {
    return ATECC608_RESULT_TIMEOUT;
  }
  if (!ATECC608_ResponseCrcValid(response, response_capacity))
  {
    return ATECC608_RESULT_CRC;
  }
  if (response[0] == ATECC608_WAKE_RESPONSE_SIZE)
  {
    if (device_status != NULL)
    {
      *device_status = response[1];
    }
    return (response[1] == 0x00U) ? ATECC608_RESULT_OK :
           ATECC608_RESULT_DEVICE_STATUS;
  }
  return ATECC608_RESULT_OK;
}

static int32_t ATECC608_ExecuteSingle(
    uint8_t opcode, uint8_t param1, uint16_t param2,
    const uint8_t *command_data, size_t command_data_size,
    uint32_t timeout_ms, uint8_t *response, size_t response_capacity,
    uint8_t *device_status)
{
  int32_t result = ATECC608_Wake();

  if (result == ATECC608_RESULT_OK)
  {
    result = ATECC608_ExecuteAwake(
        opcode, param1, param2, command_data, command_data_size, timeout_ms,
        response, response_capacity, device_status);
    ATECC608_Sleep();
  }
  return result;
}

static int32_t ATECC608_ReadConfigBlock(uint8_t block, uint8_t data[32],
                                        uint8_t *device_status)
{
  uint8_t response[ATECC608_RESPONSE_32_SIZE];
  uint16_t address = (uint16_t)block << 3U;
  int32_t result = ATECC608_ExecuteSingle(
      ATECC608_OPCODE_READ, ATECC608_ZONE_CONFIG_32, address,
      NULL, 0U, ATECC608_READ_TIMEOUT_MS, response, sizeof(response),
      device_status);

  if (result != ATECC608_RESULT_OK)
  {
    return result;
  }
  if (response[0] != ATECC608_RESPONSE_32_SIZE)
  {
    return ATECC608_RESULT_INVALID_RESPONSE;
  }
  memcpy(data, &response[1], 32U);
  return ATECC608_RESULT_OK;
}

int32_t ATECC608_Probe(ATECC608_ProbeResult *probe)
{
  uint8_t response[ATECC608_RESPONSE_4_SIZE];
  uint8_t block;
  int32_t result;

  if (probe == NULL)
  {
    return ATECC608_RESULT_BAD_ARGUMENT;
  }
  memset(probe, 0, sizeof(*probe));
  for (block = 0U; block < 4U; ++block)
  {
    result = ATECC608_ReadConfigBlock(
        block, &probe->config[(size_t)block * 32U], &probe->device_status);
    if (result != ATECC608_RESULT_OK)
    {
      probe->result = result;
      return result;
    }
  }
  result = ATECC608_ExecuteSingle(
      ATECC608_OPCODE_INFO, 0U, 0U, NULL, 0U, ATECC608_INFO_TIMEOUT_MS,
      response, sizeof(response), &probe->device_status);
  if (result != ATECC608_RESULT_OK)
  {
    probe->result = result;
    return result;
  }
  if (response[0] != ATECC608_RESPONSE_4_SIZE)
  {
    probe->result = ATECC608_RESULT_INVALID_RESPONSE;
    return probe->result;
  }
  memcpy(probe->revision, &response[1], sizeof(probe->revision));
  memcpy(&probe->serial[0], &probe->config[0], 4U);
  memcpy(&probe->serial[4], &probe->config[8], 5U);
  probe->i2c_address = probe->config[ATECC608_I2C_ADDRESS_OFFSET];
  probe->config_locked =
      (probe->config[ATECC608_LOCK_CONFIG_OFFSET] == ATECC608_LOCKED_VALUE) ?
      1U : 0U;
  probe->data_locked =
      (probe->config[ATECC608_LOCK_VALUE_OFFSET] == ATECC608_LOCKED_VALUE) ?
      1U : 0U;
  probe->slot_locked_mask =
      (uint16_t)probe->config[ATECC608_SLOT_LOCKED_OFFSET] |
      ((uint16_t)probe->config[ATECC608_SLOT_LOCKED_OFFSET + 1U] << 8U);
  probe->config_crc32c = ATECC608_Crc32c(
      probe->config, sizeof(probe->config));
  probe->result = ATECC608_RESULT_OK;
  return ATECC608_RESULT_OK;
}

int32_t ATECC608_SignDigest(
    uint16_t private_key_slot,
    const uint8_t digest[ATECC608_P256_DIGEST_SIZE],
    uint8_t signature[ATECC608_P256_SIGNATURE_SIZE],
    uint8_t *device_status)
{
  uint8_t response[ATECC608_RESPONSE_64_SIZE];
  int32_t result;

  if ((digest == NULL) || (signature == NULL) ||
      (private_key_slot > ATECC608_MAX_SLOT))
  {
    return ATECC608_RESULT_BAD_ARGUMENT;
  }
  memset(signature, 0, ATECC608_P256_SIGNATURE_SIZE);
  result = ATECC608_Wake();
  if (result != ATECC608_RESULT_OK)
  {
    return result;
  }

  /* Random refreshes the device RNG state as required before an external
     signature. Keep the device awake so its volatile message-digest buffer
     survives through Nonce and Sign. */
  result = ATECC608_ExecuteAwake(
      ATECC608_OPCODE_RANDOM, ATECC608_RANDOM_SEED_UPDATE, 0U, NULL, 0U,
      ATECC608_RANDOM_TIMEOUT_MS, response, ATECC608_RESPONSE_32_SIZE,
      device_status);
  if ((result == ATECC608_RESULT_OK) &&
      (response[0] != ATECC608_RESPONSE_32_SIZE))
  {
    result = ATECC608_RESULT_INVALID_RESPONSE;
  }
  if (result == ATECC608_RESULT_OK)
  {
    result = ATECC608_ExecuteAwake(
        ATECC608_OPCODE_NONCE,
        ATECC608_NONCE_PASSTHROUGH_MESSAGE_DIGEST, 0U, digest,
        ATECC608_P256_DIGEST_SIZE, ATECC608_NONCE_TIMEOUT_MS, response,
        ATECC608_WAKE_RESPONSE_SIZE, device_status);
    if ((result == ATECC608_RESULT_OK) &&
        (response[0] != ATECC608_WAKE_RESPONSE_SIZE))
    {
      result = ATECC608_RESULT_INVALID_RESPONSE;
    }
  }
  if (result == ATECC608_RESULT_OK)
  {
    result = ATECC608_ExecuteAwake(
        ATECC608_OPCODE_SIGN, ATECC608_SIGN_EXTERNAL_MESSAGE_DIGEST,
        private_key_slot, NULL, 0U, ATECC608_SIGN_TIMEOUT_MS, response,
        sizeof(response), device_status);
    if ((result == ATECC608_RESULT_OK) &&
        (response[0] == ATECC608_RESPONSE_64_SIZE))
    {
      memcpy(signature, &response[1], ATECC608_P256_SIGNATURE_SIZE);
    }
    else if (result == ATECC608_RESULT_OK)
    {
      result = ATECC608_RESULT_INVALID_RESPONSE;
    }
  }
  ATECC608_Sleep();
  if (result != ATECC608_RESULT_OK)
  {
    memset(signature, 0, ATECC608_P256_SIGNATURE_SIZE);
  }
  return result;
}

#if defined(ECU_FACTORY_PROVISIONING)
static int32_t ATECC608_FactoryExecuteStatus(
    uint8_t opcode, uint8_t mode, uint16_t parameter,
    const uint8_t *data, size_t data_size, uint32_t timeout_ms,
    uint8_t *device_status)
{
  uint8_t response[ATECC608_WAKE_RESPONSE_SIZE];
  int32_t result = ATECC608_ExecuteSingle(
      opcode, mode, parameter, data, data_size, timeout_ms, response,
      sizeof(response), device_status);

  if ((result == ATECC608_RESULT_OK) &&
      (response[0] != ATECC608_WAKE_RESPONSE_SIZE))
  {
    return ATECC608_RESULT_INVALID_RESPONSE;
  }
  return result;
}

int32_t ATECC608_FactoryWriteConfig(
    const uint8_t target_config[ATECC608_CONFIG_SIZE], uint8_t *device_status)
{
  uint32_t offset;
  uint8_t block;
  uint8_t word;
  uint16_t address;
  int32_t result;

  if (target_config == NULL) { return ATECC608_RESULT_BAD_ARGUMENT; }
  /* Bytes 0..15 are factory identity. Bytes 84..87 are UpdateExtra/Lock
     command owned and are deliberately preserved. */
  for (offset = 16U; offset < ATECC608_CONFIG_SIZE; offset += 4U)
  {
    if (offset == 84U) { continue; }
    block = (uint8_t)(offset / 32U);
    word = (uint8_t)((offset % 32U) / 4U);
    address = ((uint16_t)block << 3U) | word;
    result = ATECC608_FactoryExecuteStatus(
        ATECC608_OPCODE_WRITE, 0U, address, &target_config[offset], 4U,
        ATECC608_WRITE_TIMEOUT_MS, device_status);
    if (result != ATECC608_RESULT_OK) { return result; }
  }
  return ATECC608_RESULT_OK;
}

uint16_t ATECC608_FactoryConfigCrc16(
    const uint8_t config[ATECC608_CONFIG_SIZE])
{
  return (config == NULL) ? 0U :
         ATECC608_Crc16Update(0U, config, ATECC608_CONFIG_SIZE);
}

int32_t ATECC608_FactoryLockConfig(uint16_t summary_crc,
                                   uint8_t *device_status)
{
  return ATECC608_FactoryExecuteStatus(
      ATECC608_OPCODE_LOCK, 0x00U, summary_crc, NULL, 0U,
      ATECC608_LOCK_TIMEOUT_MS, device_status);
}

static int32_t ATECC608_FactoryGenKey(
    uint8_t mode, uint16_t slot,
    uint8_t public_key[ATECC608_P256_SIGNATURE_SIZE],
    uint8_t *device_status)
{
  uint8_t response[ATECC608_RESPONSE_64_SIZE];
  int32_t result;

  if ((public_key == NULL) || (slot > ATECC608_MAX_SLOT))
  {
    return ATECC608_RESULT_BAD_ARGUMENT;
  }
  result = ATECC608_ExecuteSingle(
      ATECC608_OPCODE_GENKEY, mode, slot, NULL, 0U,
      ATECC608_GENKEY_TIMEOUT_MS, response, sizeof(response), device_status);
  if (result != ATECC608_RESULT_OK) { return result; }
  if (response[0] != ATECC608_RESPONSE_64_SIZE)
  {
    return ATECC608_RESULT_INVALID_RESPONSE;
  }
  memcpy(public_key, &response[1], ATECC608_P256_SIGNATURE_SIZE);
  return ATECC608_RESULT_OK;
}

int32_t ATECC608_FactoryGeneratePrivateKey(
    uint16_t slot, uint8_t public_key[ATECC608_P256_SIGNATURE_SIZE],
    uint8_t *device_status)
{
  return ATECC608_FactoryGenKey(0x04U, slot, public_key, device_status);
}

int32_t ATECC608_FactoryGetPublicKey(
    uint16_t slot, uint8_t public_key[ATECC608_P256_SIGNATURE_SIZE],
    uint8_t *device_status)
{
  return ATECC608_FactoryGenKey(0x00U, slot, public_key, device_status);
}

int32_t ATECC608_FactoryLockData(uint8_t *device_status)
{
  /* CryptoAuthLib's calib_lock_data_zone() uses the same no-CRC mode. Data
     and OTP cannot be read while this blank device is unlocked, so a CRC
     cannot be independently reconstructed. The exact device identity and
     complete configuration are journaled before this factory-only command. */
  return ATECC608_FactoryExecuteStatus(
      ATECC608_OPCODE_LOCK, ATECC608_LOCK_NO_CRC | 0x01U, 0U, NULL, 0U,
      ATECC608_LOCK_TIMEOUT_MS, device_status);
}

int32_t ATECC608_FactoryLockSlot(uint16_t slot, uint8_t *device_status)
{
  if (slot > ATECC608_MAX_SLOT) { return ATECC608_RESULT_BAD_ARGUMENT; }
  return ATECC608_FactoryExecuteStatus(
      ATECC608_OPCODE_LOCK, (uint8_t)(0x02U | (slot << 2U)), 0U, NULL, 0U,
      ATECC608_LOCK_TIMEOUT_MS, device_status);
}
#endif /* ECU_FACTORY_PROVISIONING */
