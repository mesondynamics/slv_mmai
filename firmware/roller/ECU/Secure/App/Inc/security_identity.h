#ifndef SECURITY_IDENTITY_H
#define SECURITY_IDENTITY_H

#include <stdint.h>

#include "atecc608.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SECURITY_IDENTITY_SCHEMA           1UL
#define SECURITY_IDENTITY_PUBLIC_KEY_SIZE 64U

typedef enum
{
  SECURITY_IDENTITY_OK = 0,
  SECURITY_IDENTITY_BAD_ARGUMENT = -1,
  SECURITY_IDENTITY_NOT_PROVISIONED = -2,
  SECURITY_IDENTITY_RECORD_INVALID = -3,
  SECURITY_IDENTITY_MCU_MISMATCH = -4,
  SECURITY_IDENTITY_ATECC_MISMATCH = -5,
  SECURITY_IDENTITY_ATECC_UNLOCKED = -6,
  SECURITY_IDENTITY_RANDOM_FAILED = -7,
  SECURITY_IDENTITY_SIGN_FAILED = -8,
  SECURITY_IDENTITY_VERIFY_FAILED = -9,
  SECURITY_IDENTITY_LAYOUT_MISMATCH = -10,
  SECURITY_IDENTITY_ATECC_UNAVAILABLE = -11
} SecurityIdentity_Result;

typedef struct
{
  uint32_t generation;
  uint32_t mcu_uid[3];
  uint32_t atecc_config_crc32c;
  uint8_t atecc_serial[ATECC608_SERIAL_SIZE];
  uint8_t atecc_revision[ATECC608_REVISION_SIZE];
  uint8_t atecc_i2c_address;
  uint8_t private_key_slot;
  uint8_t public_key[SECURITY_IDENTITY_PUBLIC_KEY_SIZE];
} SecurityIdentity_Manifest;

/* Read-only runtime interface. Pairing-record programming is intentionally
   reserved for the separately gated factory image. */
int32_t SecurityIdentity_Load(SecurityIdentity_Manifest *manifest);
int32_t SecurityIdentity_Authenticate(
    const ATECC608_ProbeResult *probe, uint8_t *device_status,
    uint32_t *pairing_generation);

#if defined(ECU_FACTORY_PROVISIONING)
bool SecurityIdentity_FactorySaveManifest(
    const SecurityIdentity_Manifest *manifest);
bool SecurityIdentity_FactorySaveJournal(
    uint32_t initial_config_crc32c,
    const uint8_t initial_config[ATECC608_CONFIG_SIZE],
    const uint8_t target_config[ATECC608_CONFIG_SIZE],
    uint16_t data_lock_policy, uint8_t private_key_slot,
    const uint8_t atecc_serial[ATECC608_SERIAL_SIZE]);
bool SecurityIdentity_FactoryLoadJournal(
    uint32_t *initial_config_crc32c,
    uint8_t initial_config[ATECC608_CONFIG_SIZE],
    uint8_t target_config[ATECC608_CONFIG_SIZE],
    uint16_t *data_lock_policy, uint8_t *private_key_slot,
    uint8_t atecc_serial[ATECC608_SERIAL_SIZE]);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SECURITY_IDENTITY_H */
