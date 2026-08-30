#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "atecc608.h"
#include "software_i2c.h"

typedef enum
{
  MODEL_ASLEEP = 0,
  MODEL_AWAKE,
  MODEL_BUSY,
  MODEL_ABSENT
} ModelState;

static ModelState model_state;
static uint32_t model_tick;
static uint32_t model_busy_until;
static uint32_t model_sync_calls;
static uint32_t model_sync_failures;
static uint32_t model_reset_words;
static uint32_t model_sleep_words;
static uint32_t model_service_calls;
static uint32_t model_abort_service_call;
static bool model_wake_response;
static bool model_command_response;
static uint8_t model_opcode;
static uint16_t model_param2;
static uint8_t model_config[ATECC608_CONFIG_SIZE];

static void ModelUpdateBusy(void)
{
  if ((model_state == MODEL_BUSY) && (model_tick >= model_busy_until))
  {
    model_state = MODEL_AWAKE;
  }
}

uint32_t HAL_GetTick(void)
{
  return model_tick;
}

void HAL_Delay(uint32_t milliseconds)
{
  model_tick += milliseconds;
  ModelUpdateBusy();
}

bool SoftwareI2C_Init(void) { return true; }
bool SoftwareI2C_RecoverBus(void) { return true; }

bool SoftwareI2C_Synchronize(void)
{
  ++model_sync_calls;
  ModelUpdateBusy();
  if (model_sync_failures != 0U)
  {
    --model_sync_failures;
    return false;
  }
  return true;
}

bool SoftwareI2C_WakeToken(void)
{
  ModelUpdateBusy();
  if (model_state == MODEL_ASLEEP)
  {
    model_state = MODEL_AWAKE;
    model_wake_response = true;
  }
  /* The real device ignores a Wake token while awake or busy. */
  return true;
}

static uint16_t ModelCrc16Update(uint16_t crc, const uint8_t *data,
                                 size_t length)
{
  size_t index;
  uint8_t bit;

  for (index = 0U; index < length; ++index)
  {
    for (bit = 0x01U; bit != 0U; bit <<= 1U)
    {
      uint8_t data_bit = ((data[index] & bit) != 0U) ? 1U : 0U;
      uint8_t crc_bit = (uint8_t)(crc >> 15U);
      crc <<= 1U;
      if (data_bit != crc_bit) { crc ^= 0x8005U; }
    }
  }
  return crc;
}

static void ModelFinishResponse(uint8_t *data, size_t length)
{
  uint16_t crc = ModelCrc16Update(0U, data, length - 2U);
  data[length - 2U] = (uint8_t)crc;
  data[length - 1U] = (uint8_t)(crc >> 8U);
}

bool SoftwareI2C_Write(uint8_t address_7bit, const uint8_t *data,
                       size_t length)
{
  ModelUpdateBusy();
  if ((address_7bit != 0x60U) || (data == NULL) ||
      (model_state == MODEL_ASLEEP) || (model_state == MODEL_BUSY) ||
      (model_state == MODEL_ABSENT))
  {
    return false;
  }
  if (length == 1U)
  {
    if (data[0] == 0x00U)
    {
      ++model_reset_words;
      model_command_response = false;
      return true;
    }
    if (data[0] == 0x01U)
    {
      ++model_sleep_words;
      model_state = MODEL_ASLEEP;
      model_wake_response = false;
      model_command_response = false;
      return true;
    }
    return false;
  }
  if ((length < 8U) || (data[0] != 0x03U))
  {
    return false;
  }
  model_opcode = data[2];
  model_param2 = (uint16_t)data[4] | ((uint16_t)data[5] << 8U);
  model_command_response = true;
  return true;
}

bool SoftwareI2C_Read(uint8_t address_7bit, uint8_t *data, size_t length)
{
  ModelUpdateBusy();
  if ((address_7bit != 0x60U) || (data == NULL) ||
      (model_state == MODEL_ASLEEP) || (model_state == MODEL_BUSY) ||
      (model_state == MODEL_ABSENT))
  {
    return false;
  }
  if (model_wake_response)
  {
    static const uint8_t wake[4] = {0x04U, 0x11U, 0x33U, 0x43U};
    if (length != sizeof(wake)) { return false; }
    memcpy(data, wake, sizeof(wake));
    model_wake_response = false;
    return true;
  }
  if (!model_command_response)
  {
    return false;
  }
  model_command_response = false;
  if ((model_opcode == 0x02U) && (length == 35U))
  {
    size_t block = (size_t)(model_param2 >> 3U);
    if (block >= 4U) { return false; }
    data[0] = 35U;
    memcpy(&data[1], &model_config[block * 32U], 32U);
    ModelFinishResponse(data, 35U);
    return true;
  }
  if ((model_opcode == 0x30U) && (length == 7U))
  {
    data[0] = 7U;
    data[1] = 0x00U;
    data[2] = 0x00U;
    data[3] = 0x60U;
    data[4] = 0x05U;
    ModelFinishResponse(data, 7U);
    return true;
  }
  return false;
}

static bool ModelService(void *context)
{
  (void)context;
  ++model_service_calls;
  return (model_abort_service_call == 0U) ||
         (model_service_calls < model_abort_service_call);
}

static void ModelReset(ModelState initial_state)
{
  memset(model_config, 0, sizeof(model_config));
  model_config[0] = 0x01U;
  model_config[1] = 0x23U;
  model_config[2] = 0xD4U;
  model_config[3] = 0x7EU;
  model_config[8] = 0xB2U;
  model_config[9] = 0xEEU;
  model_config[10] = 0x0EU;
  model_config[11] = 0x9BU;
  model_config[12] = 0xEEU;
  model_config[16] = 0xC0U;
  model_config[86] = 0x00U;
  model_config[87] = 0x00U;
  model_config[88] = 0xFBU;
  model_config[89] = 0xFFU;
  model_state = initial_state;
  model_tick = 0U;
  model_busy_until = 0U;
  model_sync_calls = 0U;
  model_sync_failures = 0U;
  model_reset_words = 0U;
  model_sleep_words = 0U;
  model_service_calls = 0U;
  model_abort_service_call = 0U;
  model_wake_response = false;
  model_command_response = false;
  model_opcode = 0U;
  model_param2 = 0U;
}

static void AssertExactProbe(const ATECC608_ProbeResult *probe)
{
  static const uint8_t serial[ATECC608_SERIAL_SIZE] = {
    0x01U, 0x23U, 0xD4U, 0x7EU, 0xB2U,
    0xEEU, 0x0EU, 0x9BU, 0xEEU
  };
  static const uint8_t revision[ATECC608_REVISION_SIZE] = {
    0x00U, 0x00U, 0x60U, 0x05U
  };

  assert(probe->result == ATECC608_RESULT_OK);
  assert(memcmp(probe->serial, serial, sizeof(serial)) == 0);
  assert(memcmp(probe->revision, revision, sizeof(revision)) == 0);
  assert(probe->i2c_address == 0xC0U);
  assert(probe->config_locked == 1U);
  assert(probe->data_locked == 1U);
}

static void TestAsleepFastPath(void)
{
  ATECC608_ProbeResult probe;
  ModelReset(MODEL_ASLEEP);
  assert(ATECC608_ProbeWithRecovery(&probe, ModelService, NULL) ==
         ATECC608_RESULT_OK);
  AssertExactProbe(&probe);
  assert(model_service_calls == 1U);
  assert(model_sync_calls == 1U);
}

static void TestAwakeResetNormalization(void)
{
  ATECC608_ProbeResult probe;
  ModelReset(MODEL_AWAKE);
  assert(ATECC608_ProbeWithRecovery(&probe, ModelService, NULL) ==
         ATECC608_RESULT_OK);
  AssertExactProbe(&probe);
  assert(model_reset_words == 1U);
  assert(model_sleep_words == 6U); /* normalization plus five probe commands */
}

static void TestInterruptedBusyCommandRecovery(void)
{
  ATECC608_ProbeResult probe;
  ModelReset(MODEL_BUSY);
  model_busy_until = 110U;
  assert(ATECC608_ProbeWithRecovery(&probe, ModelService, NULL) ==
         ATECC608_RESULT_OK);
  AssertExactProbe(&probe);
  assert(model_service_calls > 1U);
  assert(model_tick >= model_busy_until);
  assert(model_tick < ATECC608_MAX_EXECUTION_TIME_MS);
  assert(model_reset_words == 1U);
}

static void TestAbsentDeviceIsBounded(void)
{
  ATECC608_ProbeResult probe;
  ModelReset(MODEL_ABSENT);
  assert(ATECC608_ProbeWithRecovery(&probe, ModelService, NULL) ==
         ATECC608_RESULT_WAKE);
  assert(probe.result == ATECC608_RESULT_WAKE);
  assert(model_tick == 725U);
  assert(model_service_calls == 30U);
  assert(model_sync_calls == 30U);
}

static void TestTransientBusRecovery(void)
{
  ATECC608_ProbeResult probe;
  ModelReset(MODEL_ASLEEP);
  model_sync_failures = 2U;
  assert(ATECC608_ProbeWithRecovery(&probe, ModelService, NULL) ==
         ATECC608_RESULT_OK);
  AssertExactProbe(&probe);
  assert(model_service_calls == 3U);
  assert(model_tick >= 50U);
}

static void TestWatchdogAbortFailsClosed(void)
{
  ATECC608_ProbeResult probe;
  ModelReset(MODEL_ABSENT);
  model_abort_service_call = 3U;
  assert(ATECC608_ProbeWithRecovery(&probe, ModelService, NULL) ==
         ATECC608_RESULT_RECOVERY_ABORTED);
  assert(probe.result == ATECC608_RESULT_RECOVERY_ABORTED);
  assert(model_service_calls == 3U);
  assert(model_tick == 50U);
}

int main(void)
{
  TestAsleepFastPath();
  TestAwakeResetNormalization();
  TestInterruptedBusyCommandRecovery();
  TestAbsentDeviceIsBounded();
  TestTransientBusRecovery();
  TestWatchdogAbortFailsClosed();
  puts("atecc608 recovery tests passed");
  return 0;
}
