#include "security_factory.h"

#if defined(ECU_FACTORY_PROVISIONING)

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "atecc608.h"
#include "main.h"
#include "security_crypto.h"
#include "security_identity.h"
#include "security_mcu_identity.h"

#define SECURITY_FACTORY_PRIVATE_KEY_SLOT       2U
#define SECURITY_FACTORY_SLOT_CONFIG_OFFSET    24U
#define SECURITY_FACTORY_KEY_CONFIG_OFFSET    100U
#define SECURITY_FACTORY_SLOT_CONFIG_LSB      0x87U
#define SECURITY_FACTORY_SLOT_CONFIG_MSB      0x20U
/* P-256 private/public-info, lockable, and ReqRandom. AuthKey/Persistent
   options are deliberately disabled for the board-pairing identity key. */
#define SECURITY_FACTORY_KEY_CONFIG_LSB       0x73U
#define SECURITY_FACTORY_KEY_CONFIG_MSB       0x00U
#define SECURITY_FACTORY_DATA_LOCK_POLICY_NO_CRC 0x0001U

enum
{
  SECURITY_FACTORY_OK = 0,
  SECURITY_FACTORY_BAD_ARGUMENT = -100,
  SECURITY_FACTORY_WRONG_DEVICE = -101,
  SECURITY_FACTORY_BASELINE_MISMATCH = -102,
  SECURITY_FACTORY_JOURNAL_FAILED = -103,
  SECURITY_FACTORY_CONFIG_FAILED = -104,
  SECURITY_FACTORY_DATA_FAILED = -105,
  SECURITY_FACTORY_KEY_FAILED = -106,
  SECURITY_FACTORY_SLOT_FAILED = -107,
  SECURITY_FACTORY_CRYPTO_FAILED = -108,
  SECURITY_FACTORY_MANIFEST_FAILED = -109,
  SECURITY_FACTORY_PROBE_FAILED = -110
};

static bool SecurityFactory_ConfigMatchesTarget(
    const uint8_t current[ATECC608_CONFIG_SIZE],
    const uint8_t target[ATECC608_CONFIG_SIZE])
{
  size_t index;
  for (index = 0U; index < ATECC608_CONFIG_SIZE; ++index)
  {
    /* LockValue, LockConfig, and SlotLocked are expected to change as the
       transaction advances. No other byte may drift from the journal. */
    if ((index >= 86U) && (index <= 89U)) { continue; }
    if (current[index] != target[index]) { return false; }
  }
  return true;
}

static bool SecurityFactory_TargetReviewed(
    const uint8_t target[ATECC608_CONFIG_SIZE])
{
  return (target[SECURITY_FACTORY_SLOT_CONFIG_OFFSET] ==
          SECURITY_FACTORY_SLOT_CONFIG_LSB) &&
         (target[SECURITY_FACTORY_SLOT_CONFIG_OFFSET + 1U] ==
          SECURITY_FACTORY_SLOT_CONFIG_MSB) &&
         (target[SECURITY_FACTORY_KEY_CONFIG_OFFSET] ==
          SECURITY_FACTORY_KEY_CONFIG_LSB) &&
         (target[SECURITY_FACTORY_KEY_CONFIG_OFFSET + 1U] ==
          SECURITY_FACTORY_KEY_CONFIG_MSB);
}

static void SecurityFactory_FillStatus(
    SAFETY_FactoryStatus *status, const ATECC608_ProbeResult *probe,
    int32_t result, uint32_t phases, const uint8_t *public_key)
{
  memset(status, 0, sizeof(*status));
  status->result = result;
  status->phase_flags = phases;
  (void)SecurityMcuIdentity_Get(status->mcu_uid);
  status->private_key_slot = SECURITY_FACTORY_PRIVATE_KEY_SLOT;
  if (probe != NULL)
  {
    status->config_crc32c = probe->config_crc32c;
    status->slot_locked_mask = probe->slot_locked_mask;
    status->config_locked = probe->config_locked;
    status->data_locked = probe->data_locked;
    status->device_status = probe->device_status;
    memcpy(status->serial, probe->serial, sizeof(status->serial));
    memcpy(status->revision, probe->revision, sizeof(status->revision));
    memcpy(status->config, probe->config, sizeof(status->config));
  }
  if (public_key != NULL)
  {
    memcpy(status->public_key, public_key, sizeof(status->public_key));
  }
}

int32_t SecurityFactory_GetStatus(SAFETY_FactoryStatus *status)
{
  ATECC608_ProbeResult probe;
  SecurityIdentity_Manifest manifest;
  uint8_t public_key[ATECC608_P256_SIGNATURE_SIZE];
  uint8_t initial[ATECC608_CONFIG_SIZE];
  uint8_t target[ATECC608_CONFIG_SIZE];
  uint8_t journal_serial[ATECC608_SERIAL_SIZE];
  uint32_t initial_crc;
  uint16_t data_lock_policy;
  uint8_t slot;
  uint32_t phases = 0U;
  int32_t result;

  if (status == NULL) { return SECURITY_FACTORY_BAD_ARGUMENT; }
  result = ATECC608_Probe(&probe);
  if (result != ATECC608_RESULT_OK)
  {
    SecurityFactory_FillStatus(status, NULL, result, 0U, NULL);
    return SECURITY_FACTORY_PROBE_FAILED;
  }
  phases |= SAFETY_FACTORY_PHASE_PROBED;
  if (SecurityIdentity_FactoryLoadJournal(
          &initial_crc, initial, target, &data_lock_policy, &slot,
          journal_serial))
  {
    phases |= SAFETY_FACTORY_PHASE_JOURNALED;
  }
  if (probe.config_locked != 0U)
  {
    phases |= SAFETY_FACTORY_PHASE_CONFIG_LOCKED;
  }
  if (probe.data_locked != 0U)
  {
    phases |= SAFETY_FACTORY_PHASE_DATA_LOCKED;
  }
  if ((probe.slot_locked_mask &
       (uint16_t)(1UL << SECURITY_FACTORY_PRIVATE_KEY_SLOT)) == 0U)
  {
    phases |= SAFETY_FACTORY_PHASE_SLOT_LOCKED;
  }
  memset(public_key, 0, sizeof(public_key));
  if (SecurityIdentity_Load(&manifest) == SECURITY_IDENTITY_OK)
  {
    phases |= SAFETY_FACTORY_PHASE_MANIFEST_SAVED;
    memcpy(public_key, manifest.public_key, sizeof(public_key));
    if (SecurityIdentity_Authenticate(&probe, &probe.device_status,
                                      &initial_crc) ==
        SECURITY_IDENTITY_OK)
    {
      phases |= SAFETY_FACTORY_PHASE_AUTHENTICATED;
    }
  }
  SecurityFactory_FillStatus(status, &probe, SECURITY_FACTORY_OK, phases,
                             public_key);
  return SECURITY_FACTORY_OK;
}

int32_t SecurityFactory_Provision(
    const SAFETY_FactoryProvisionRequest *request,
    SAFETY_FactoryStatus *status)
{
  ATECC608_ProbeResult probe;
  SecurityIdentity_Manifest manifest;
  uint8_t initial[ATECC608_CONFIG_SIZE];
  uint8_t target[ATECC608_CONFIG_SIZE];
  uint8_t journal_serial[ATECC608_SERIAL_SIZE];
  uint8_t public_key[ATECC608_P256_SIGNATURE_SIZE];
  uint8_t challenge[SECURITY_CRYPTO_P256_SIZE];
  uint8_t signature[2U * SECURITY_CRYPTO_P256_SIZE];
  uint32_t mcu_uid[3];
  uint32_t journal_initial_crc = 0U;
  uint16_t data_lock_policy = SECURITY_FACTORY_DATA_LOCK_POLICY_NO_CRC;
  uint8_t slot = SECURITY_FACTORY_PRIVATE_KEY_SLOT;
  int32_t result;

  if ((request == NULL) || (status == NULL) ||
      (request->authorization_token != SAFETY_FACTORY_PROVISION_TOKEN))
  {
    return SECURITY_FACTORY_BAD_ARGUMENT;
  }
  result = ATECC608_Probe(&probe);
  if (result != ATECC608_RESULT_OK)
  {
    SecurityFactory_FillStatus(status, NULL, result, 0U, NULL);
    return SECURITY_FACTORY_PROBE_FAILED;
  }
  if (!SecurityMcuIdentity_Get(mcu_uid) ||
      (memcmp(request->expected_mcu_uid, mcu_uid, sizeof(mcu_uid)) != 0) ||
      (memcmp(request->expected_atecc_serial, probe.serial,
              sizeof(probe.serial)) != 0))
  {
    SecurityFactory_FillStatus(status, &probe,
                               SECURITY_FACTORY_WRONG_DEVICE, 0U, NULL);
    return SECURITY_FACTORY_WRONG_DEVICE;
  }

  if (!SecurityIdentity_FactoryLoadJournal(
          &journal_initial_crc, initial, target, &data_lock_policy, &slot,
          journal_serial))
  {
    if ((probe.config_locked != 0U) || (probe.data_locked != 0U) ||
        (probe.config_crc32c !=
         request->expected_initial_config_crc32c))
    {
      SecurityFactory_FillStatus(status, &probe,
                                 SECURITY_FACTORY_BASELINE_MISMATCH,
                                 SAFETY_FACTORY_PHASE_PROBED, NULL);
      return SECURITY_FACTORY_BASELINE_MISMATCH;
    }
    memcpy(initial, probe.config, sizeof(initial));
    memcpy(target, initial, sizeof(target));
    target[SECURITY_FACTORY_SLOT_CONFIG_OFFSET] =
        SECURITY_FACTORY_SLOT_CONFIG_LSB;
    target[SECURITY_FACTORY_SLOT_CONFIG_OFFSET + 1U] =
        SECURITY_FACTORY_SLOT_CONFIG_MSB;
    target[SECURITY_FACTORY_KEY_CONFIG_OFFSET] =
        SECURITY_FACTORY_KEY_CONFIG_LSB;
    target[SECURITY_FACTORY_KEY_CONFIG_OFFSET + 1U] =
        SECURITY_FACTORY_KEY_CONFIG_MSB;
    if (!SecurityIdentity_FactorySaveJournal(
            probe.config_crc32c, initial, target,
            SECURITY_FACTORY_DATA_LOCK_POLICY_NO_CRC,
            SECURITY_FACTORY_PRIVATE_KEY_SLOT, probe.serial))
    {
      SecurityFactory_FillStatus(status, &probe,
                                 SECURITY_FACTORY_JOURNAL_FAILED,
                                 SAFETY_FACTORY_PHASE_PROBED, NULL);
      return SECURITY_FACTORY_JOURNAL_FAILED;
    }
    journal_initial_crc = probe.config_crc32c;
    memcpy(journal_serial, probe.serial, sizeof(journal_serial));
  }
  if ((journal_initial_crc != request->expected_initial_config_crc32c) ||
      (data_lock_policy != SECURITY_FACTORY_DATA_LOCK_POLICY_NO_CRC) ||
      (slot != SECURITY_FACTORY_PRIVATE_KEY_SLOT) ||
      (memcmp(journal_serial, probe.serial, sizeof(journal_serial)) != 0) ||
      !SecurityFactory_TargetReviewed(target) ||
      ((probe.config_locked == 0U) &&
       (memcmp(probe.config, initial, sizeof(initial)) != 0)) ||
      ((probe.config_locked != 0U) &&
       !SecurityFactory_ConfigMatchesTarget(probe.config, target)))
  {
    SecurityFactory_FillStatus(status, &probe,
                               SECURITY_FACTORY_BASELINE_MISMATCH,
                               SAFETY_FACTORY_PHASE_PROBED |
                               SAFETY_FACTORY_PHASE_JOURNALED, NULL);
    return SECURITY_FACTORY_BASELINE_MISMATCH;
  }

  if (probe.config_locked == 0U)
  {
    result = ATECC608_FactoryWriteConfig(target, &probe.device_status);
    if ((result != ATECC608_RESULT_OK) ||
        (ATECC608_Probe(&probe) != ATECC608_RESULT_OK) ||
        (memcmp(probe.config, target, sizeof(target)) != 0) ||
        (ATECC608_FactoryLockConfig(
             ATECC608_FactoryConfigCrc16(probe.config),
             &probe.device_status) != ATECC608_RESULT_OK) ||
        (ATECC608_Probe(&probe) != ATECC608_RESULT_OK) ||
        (probe.config_locked == 0U))
    {
      SecurityFactory_FillStatus(status, &probe,
                                 SECURITY_FACTORY_CONFIG_FAILED,
                                 SAFETY_FACTORY_PHASE_PROBED |
                                 SAFETY_FACTORY_PHASE_JOURNALED, NULL);
      return SECURITY_FACTORY_CONFIG_FAILED;
    }
  }
  if (!SecurityFactory_ConfigMatchesTarget(probe.config, target))
  {
    SecurityFactory_FillStatus(status, &probe,
                               SECURITY_FACTORY_CONFIG_FAILED,
                               SAFETY_FACTORY_PHASE_CONFIG_LOCKED, NULL);
    return SECURITY_FACTORY_CONFIG_FAILED;
  }

  memset(public_key, 0, sizeof(public_key));
  if (probe.data_locked == 0U)
  {
    result = ATECC608_FactoryGeneratePrivateKey(
        SECURITY_FACTORY_PRIVATE_KEY_SLOT, public_key,
        &probe.device_status);
    if ((result != ATECC608_RESULT_OK) ||
        (ATECC608_FactoryLockData(&probe.device_status) !=
         ATECC608_RESULT_OK) ||
        (ATECC608_Probe(&probe) != ATECC608_RESULT_OK) ||
        (probe.data_locked == 0U))
    {
      SecurityFactory_FillStatus(status, &probe,
                                 SECURITY_FACTORY_DATA_FAILED,
                                 SAFETY_FACTORY_PHASE_CONFIG_LOCKED, NULL);
      return SECURITY_FACTORY_DATA_FAILED;
    }
  }
  result = ATECC608_FactoryGetPublicKey(
      SECURITY_FACTORY_PRIVATE_KEY_SLOT, public_key, &probe.device_status);
  if (result != ATECC608_RESULT_OK)
  {
    SecurityFactory_FillStatus(status, &probe, SECURITY_FACTORY_KEY_FAILED,
                               SAFETY_FACTORY_PHASE_CONFIG_LOCKED |
                               SAFETY_FACTORY_PHASE_DATA_LOCKED, NULL);
    return SECURITY_FACTORY_KEY_FAILED;
  }
  if ((probe.slot_locked_mask &
       (uint16_t)(1UL << SECURITY_FACTORY_PRIVATE_KEY_SLOT)) != 0U)
  {
    if ((ATECC608_FactoryLockSlot(
             SECURITY_FACTORY_PRIVATE_KEY_SLOT, &probe.device_status) !=
         ATECC608_RESULT_OK) ||
        (ATECC608_Probe(&probe) != ATECC608_RESULT_OK) ||
        ((probe.slot_locked_mask &
          (uint16_t)(1UL << SECURITY_FACTORY_PRIVATE_KEY_SLOT)) != 0U))
    {
      SecurityFactory_FillStatus(status, &probe,
                                 SECURITY_FACTORY_SLOT_FAILED,
                                 SAFETY_FACTORY_PHASE_CONFIG_LOCKED |
                                 SAFETY_FACTORY_PHASE_DATA_LOCKED,
                                 public_key);
      return SECURITY_FACTORY_SLOT_FAILED;
    }
  }

  if (!SecurityCrypto_RandomDigest(challenge) ||
      (ATECC608_SignDigest(SECURITY_FACTORY_PRIVATE_KEY_SLOT, challenge,
                           signature, &probe.device_status) !=
       ATECC608_RESULT_OK) ||
      !SecurityCrypto_VerifyP256(public_key, challenge, signature))
  {
    memset(challenge, 0, sizeof(challenge));
    memset(signature, 0, sizeof(signature));
    SecurityFactory_FillStatus(status, &probe,
                               SECURITY_FACTORY_CRYPTO_FAILED,
                               SAFETY_FACTORY_PHASE_CONFIG_LOCKED |
                               SAFETY_FACTORY_PHASE_DATA_LOCKED |
                               SAFETY_FACTORY_PHASE_SLOT_LOCKED,
                               public_key);
    return SECURITY_FACTORY_CRYPTO_FAILED;
  }
  memset(challenge, 0, sizeof(challenge));
  memset(signature, 0, sizeof(signature));

  memset(&manifest, 0, sizeof(manifest));
  manifest.generation = 1U;
  if (!SecurityMcuIdentity_Get(manifest.mcu_uid))
  {
    SecurityFactory_FillStatus(status, &probe,
                               SECURITY_FACTORY_WRONG_DEVICE, 0U, NULL);
    return SECURITY_FACTORY_WRONG_DEVICE;
  }
  manifest.atecc_config_crc32c = probe.config_crc32c;
  memcpy(manifest.atecc_serial, probe.serial, sizeof(manifest.atecc_serial));
  memcpy(manifest.atecc_revision, probe.revision,
         sizeof(manifest.atecc_revision));
  manifest.atecc_i2c_address = probe.i2c_address;
  manifest.private_key_slot = SECURITY_FACTORY_PRIVATE_KEY_SLOT;
  memcpy(manifest.public_key, public_key, sizeof(manifest.public_key));
  if (!SecurityIdentity_FactorySaveManifest(&manifest) ||
      (SecurityIdentity_Authenticate(
           &probe, &probe.device_status, &journal_initial_crc) !=
       SECURITY_IDENTITY_OK))
  {
    SecurityFactory_FillStatus(status, &probe,
                               SECURITY_FACTORY_MANIFEST_FAILED,
                               SAFETY_FACTORY_PHASE_CONFIG_LOCKED |
                               SAFETY_FACTORY_PHASE_DATA_LOCKED |
                               SAFETY_FACTORY_PHASE_SLOT_LOCKED,
                               public_key);
    return SECURITY_FACTORY_MANIFEST_FAILED;
  }
  SecurityFactory_FillStatus(
      status, &probe, SECURITY_FACTORY_OK,
      SAFETY_FACTORY_PHASE_PROBED | SAFETY_FACTORY_PHASE_JOURNALED |
      SAFETY_FACTORY_PHASE_CONFIG_LOCKED |
      SAFETY_FACTORY_PHASE_DATA_LOCKED |
      SAFETY_FACTORY_PHASE_SLOT_LOCKED |
      SAFETY_FACTORY_PHASE_MANIFEST_SAVED |
      SAFETY_FACTORY_PHASE_AUTHENTICATED,
      public_key);
  return SECURITY_FACTORY_OK;
}

#endif /* ECU_FACTORY_PROVISIONING */
