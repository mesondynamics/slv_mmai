#ifndef ATECC608_H
#define ATECC608_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATECC608_SERIAL_SIZE       9U
#define ATECC608_REVISION_SIZE     4U
#define ATECC608_CONFIG_SIZE     128U
#define ATECC608_P256_DIGEST_SIZE 32U
#define ATECC608_P256_SIGNATURE_SIZE 64U
/* Longest execution budget used by this driver (slowest supported Sign mode).
   Startup recovery is bounded by this value and services the MCU watchdog in
   shorter intervals. */
#define ATECC608_MAX_EXECUTION_TIME_MS 700U

typedef enum
{
  ATECC608_RESULT_OK = 0,
  ATECC608_RESULT_BAD_ARGUMENT = -1,
  ATECC608_RESULT_BUS = -2,
  ATECC608_RESULT_WAKE = -3,
  ATECC608_RESULT_TRANSMIT = -4,
  ATECC608_RESULT_RECEIVE = -5,
  ATECC608_RESULT_CRC = -6,
  ATECC608_RESULT_DEVICE_STATUS = -7,
  ATECC608_RESULT_INVALID_RESPONSE = -8,
  ATECC608_RESULT_TIMEOUT = -9,
  ATECC608_RESULT_RECOVERY_ABORTED = -10
} ATECC608_Result;

typedef bool (*ATECC608_RecoveryService)(void *context);

typedef struct
{
  int32_t result;
  uint32_t config_crc32c;
  uint8_t serial[ATECC608_SERIAL_SIZE];
  uint8_t revision[ATECC608_REVISION_SIZE];
  uint8_t i2c_address;
  uint8_t config_locked;
  uint8_t data_locked;
  uint8_t device_status;
  uint16_t slot_locked_mask;
  uint8_t config[ATECC608_CONFIG_SIZE];
} ATECC608_ProbeResult;

/* Read-only discovery. This API intentionally exposes no write or lock
   primitive; irreversible provisioning is a separate, explicitly gated tool. */
int32_t ATECC608_Probe(ATECC608_ProbeResult *probe);

/* Startup-only, read-only recovery for an MCU reset that did not reset the
   ATECC.  Probe commands may be repeated within one bounded longest-command
   budget; no state-changing crypto operation is replayed. */
int32_t ATECC608_ProbeWithRecovery(
    ATECC608_ProbeResult *probe,
    ATECC608_RecoveryService recovery_service,
    void *recovery_context);

/* Reset only volatile I/O framing after an independent host reset.  This
   performs the Microchip bus synchronization/address-counter reset and, when
   the device is awake and not busy, places it in Sleep.  It cannot modify any
   ATECC EEPROM/configuration/slot data. */
int32_t ATECC608_Synchronize(void);

/* Proves possession of an already-provisioned private key without exposing
   it. The digest is supplied by the Secure application and the returned
   signature is P-256 R || S in big-endian form. This is not a provisioning
   API and cannot write or lock any ATECC zone. */
int32_t ATECC608_SignDigest(
    uint16_t private_key_slot,
    const uint8_t digest[ATECC608_P256_DIGEST_SIZE],
    uint8_t signature[ATECC608_P256_SIGNATURE_SIZE],
    uint8_t *device_status);

#if defined(ECU_FACTORY_PROVISIONING)
/* These irreversible primitives exist only in the separately built factory
   image. Production and development applications have no linkable mutation
   entry points. */
int32_t ATECC608_FactoryWriteConfig(
    const uint8_t target_config[ATECC608_CONFIG_SIZE],
    uint8_t *device_status);
int32_t ATECC608_FactoryLockConfig(uint16_t summary_crc,
                                   uint8_t *device_status);
int32_t ATECC608_FactoryGeneratePrivateKey(
    uint16_t slot, uint8_t public_key[ATECC608_P256_SIGNATURE_SIZE],
    uint8_t *device_status);
int32_t ATECC608_FactoryGetPublicKey(
    uint16_t slot, uint8_t public_key[ATECC608_P256_SIGNATURE_SIZE],
    uint8_t *device_status);
int32_t ATECC608_FactoryLockData(uint8_t *device_status);
int32_t ATECC608_FactoryLockSlot(uint16_t slot, uint8_t *device_status);
uint16_t ATECC608_FactoryConfigCrc16(
    const uint8_t config[ATECC608_CONFIG_SIZE]);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ATECC608_H */
