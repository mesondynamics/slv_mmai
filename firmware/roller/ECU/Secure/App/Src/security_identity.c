#include "security_identity.h"

#include <stddef.h>
#include <string.h>

#include "ecu_flash_layout.h"
#include "main.h"
#include "security_crypto.h"
#include "security_mcu_identity.h"

#define SECURITY_IDENTITY_MAGIC       0x49435545UL /* "ECUI" */
#define SECURITY_IDENTITY_COMMIT      0x444E4249UL /* "IBND" */
#define SECURITY_IDENTITY_SECTOR_SIZE ECU_FLASH_SECTOR_SIZE
#define SECURITY_IDENTITY_ADDRESS_A \
  (ECU_FLASH_BASE_S + ECU_SECURITY_STORE_OFFSET)
#define SECURITY_IDENTITY_ADDRESS_B \
  (SECURITY_IDENTITY_ADDRESS_A + SECURITY_IDENTITY_SECTOR_SIZE)

typedef struct __attribute__((aligned(16)))
{
  uint32_t magic;
  uint32_t schema;
  uint32_t generation;
  uint32_t layout_version;
  uint32_t mcu_uid[3];
  uint32_t atecc_config_crc32c;
  uint8_t atecc_serial[ATECC608_SERIAL_SIZE];
  uint8_t atecc_revision[ATECC608_REVISION_SIZE];
  uint8_t atecc_i2c_address;
  uint8_t private_key_slot;
  uint8_t identity_padding;
  uint8_t public_key[SECURITY_IDENTITY_PUBLIC_KEY_SIZE];
  uint32_t record_size;
  uint32_t crc32c;
  uint8_t reserved[8];
  uint32_t commit;
  uint8_t commit_padding[12];
} SecurityIdentity_Record;

_Static_assert(sizeof(SecurityIdentity_Record) == 144U,
               "Pairing record layout is part of the factory ABI");
_Static_assert((sizeof(SecurityIdentity_Record) % ECU_FLASH_PROGRAM_UNIT) == 0U,
               "Pairing records must use complete flash quad-words");
_Static_assert(offsetof(SecurityIdentity_Record, commit) ==
               (sizeof(SecurityIdentity_Record) - ECU_FLASH_PROGRAM_UNIT),
               "Commit marker must be programmed in the final quad-word");
_Static_assert((ECU_SECURITY_STORE_SIZE >=
                (2U * SECURITY_IDENTITY_SECTOR_SIZE)),
               "Security store must contain redundant identity sectors");

static uint32_t SecurityIdentity_Crc32c(const void *data, size_t length)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFUL;
  size_t index;
  uint32_t bit;

  for (index = 0U; index < length; ++index)
  {
    crc ^= bytes[index];
    for (bit = 0U; bit < 8U; ++bit)
    {
      crc = (crc >> 1U) ^ ((crc & 1U) ? 0x82F63B78UL : 0U);
    }
  }
  return crc ^ 0xFFFFFFFFUL;
}

static bool SecurityIdentity_GenerationNewer(uint32_t candidate,
                                              uint32_t reference)
{
  uint32_t difference = candidate - reference;
  return (difference != 0U) && (difference < 0x80000000UL);
}

static bool SecurityIdentity_McuUidMatches(const uint32_t expected[3])
{
  uint32_t current[3];

  return SecurityMcuIdentity_Get(current) &&
         (memcmp(expected, current, sizeof(current)) == 0);
}

static bool SecurityIdentity_RecordValid(
    const SecurityIdentity_Record *record)
{
  if ((record->magic != SECURITY_IDENTITY_MAGIC) ||
      (record->schema != SECURITY_IDENTITY_SCHEMA) ||
      (record->layout_version != ECU_LAYOUT_VERSION) ||
      (record->record_size != sizeof(*record)) ||
      (record->private_key_slot > 15U) ||
      (record->commit != SECURITY_IDENTITY_COMMIT))
  {
    return false;
  }
  return record->crc32c == SecurityIdentity_Crc32c(
      record, offsetof(SecurityIdentity_Record, crc32c));
}

static const SecurityIdentity_Record *SecurityIdentity_ScanSector(
    uint32_t address, bool *nonblank_record_seen)
{
  const SecurityIdentity_Record *newest = NULL;
  uint32_t offset;

  for (offset = 0U;
       (offset + sizeof(SecurityIdentity_Record)) <=
       SECURITY_IDENTITY_SECTOR_SIZE;
       offset += sizeof(SecurityIdentity_Record))
  {
    const SecurityIdentity_Record *candidate =
        (const SecurityIdentity_Record *)(uintptr_t)(address + offset);
    if (candidate->magic != 0xFFFFFFFFUL)
    {
      *nonblank_record_seen = true;
    }
    if (SecurityIdentity_RecordValid(candidate) &&
        ((newest == NULL) ||
         SecurityIdentity_GenerationNewer(candidate->generation,
                                           newest->generation)))
    {
      newest = candidate;
    }
  }
  return newest;
}

int32_t SecurityIdentity_Load(SecurityIdentity_Manifest *manifest)
{
  const SecurityIdentity_Record *record_a;
  const SecurityIdentity_Record *record_b;
  const SecurityIdentity_Record *newest;
  bool nonblank_record_seen = false;

  if (manifest == NULL) { return SECURITY_IDENTITY_BAD_ARGUMENT; }
  memset(manifest, 0, sizeof(*manifest));
  record_a = SecurityIdentity_ScanSector(SECURITY_IDENTITY_ADDRESS_A,
                                         &nonblank_record_seen);
  record_b = SecurityIdentity_ScanSector(SECURITY_IDENTITY_ADDRESS_B,
                                         &nonblank_record_seen);
  if (record_a == NULL) { newest = record_b; }
  else if (record_b == NULL) { newest = record_a; }
  else
  {
    newest = SecurityIdentity_GenerationNewer(record_b->generation,
                                               record_a->generation) ?
             record_b : record_a;
  }
  if (newest == NULL)
  {
    return nonblank_record_seen ? SECURITY_IDENTITY_RECORD_INVALID :
           SECURITY_IDENTITY_NOT_PROVISIONED;
  }

  manifest->generation = newest->generation;
  memcpy(manifest->mcu_uid, newest->mcu_uid, sizeof(manifest->mcu_uid));
  manifest->atecc_config_crc32c = newest->atecc_config_crc32c;
  memcpy(manifest->atecc_serial, newest->atecc_serial,
         sizeof(manifest->atecc_serial));
  memcpy(manifest->atecc_revision, newest->atecc_revision,
         sizeof(manifest->atecc_revision));
  manifest->atecc_i2c_address = newest->atecc_i2c_address;
  manifest->private_key_slot = newest->private_key_slot;
  memcpy(manifest->public_key, newest->public_key,
         sizeof(manifest->public_key));
  return SECURITY_IDENTITY_OK;
}

static void SecurityIdentity_ClearVolatile(uint8_t *buffer, size_t length)
{
  volatile uint8_t *value = buffer;
  while (length-- != 0U) { *value++ = 0U; }
}

int32_t SecurityIdentity_Authenticate(
    const ATECC608_ProbeResult *probe, uint8_t *device_status,
    uint32_t *pairing_generation)
{
  SecurityIdentity_Manifest manifest;
  uint8_t challenge[SECURITY_CRYPTO_P256_SIZE];
  uint8_t signature[2U * SECURITY_CRYPTO_P256_SIZE];
  int32_t result;

  if ((probe == NULL) || (pairing_generation == NULL))
  {
    return SECURITY_IDENTITY_BAD_ARGUMENT;
  }
  *pairing_generation = 0U;
  result = SecurityIdentity_Load(&manifest);
  if (result != SECURITY_IDENTITY_OK) { return result; }
  *pairing_generation = manifest.generation;
  if (!SecurityIdentity_McuUidMatches(manifest.mcu_uid))
  {
    return SECURITY_IDENTITY_MCU_MISMATCH;
  }
  if ((manifest.atecc_config_crc32c != probe->config_crc32c) ||
      (manifest.atecc_i2c_address != probe->i2c_address) ||
      (memcmp(manifest.atecc_serial, probe->serial,
              sizeof(manifest.atecc_serial)) != 0) ||
      (memcmp(manifest.atecc_revision, probe->revision,
              sizeof(manifest.atecc_revision)) != 0))
  {
    return SECURITY_IDENTITY_ATECC_MISMATCH;
  }
  if ((probe->config_locked == 0U) || (probe->data_locked == 0U) ||
      ((probe->slot_locked_mask &
        (uint16_t)(1UL << manifest.private_key_slot)) != 0U))
  {
    return SECURITY_IDENTITY_ATECC_UNLOCKED;
  }
  if (!SecurityCrypto_RandomDigest(challenge))
  {
    return SECURITY_IDENTITY_RANDOM_FAILED;
  }
  result = ATECC608_SignDigest(manifest.private_key_slot, challenge,
                               signature, device_status);
  if (result != ATECC608_RESULT_OK)
  {
    SecurityIdentity_ClearVolatile(challenge, sizeof(challenge));
    return SECURITY_IDENTITY_SIGN_FAILED;
  }
  result = SecurityCrypto_VerifyP256(manifest.public_key, challenge,
                                     signature) ?
           SECURITY_IDENTITY_OK : SECURITY_IDENTITY_VERIFY_FAILED;
  SecurityIdentity_ClearVolatile(challenge, sizeof(challenge));
  SecurityIdentity_ClearVolatile(signature, sizeof(signature));
  return result;
}

#if defined(ECU_FACTORY_PROVISIONING)
#define SECURITY_FACTORY_JOURNAL_MAGIC  0x4A504645UL /* "EFPJ" */
#define SECURITY_FACTORY_JOURNAL_COMMIT 0x4B434F4CUL /* "LOCK" */
#define SECURITY_FACTORY_JOURNAL_SCHEMA 2UL
#define SECURITY_FACTORY_JOURNAL_ADDRESS \
  (SECURITY_IDENTITY_ADDRESS_A + (2U * SECURITY_IDENTITY_SECTOR_SIZE))
#define SECURITY_FACTORY_JOURNAL_SECTOR \
  ((ECU_SECURITY_STORE_OFFSET / ECU_FLASH_SECTOR_SIZE) + 2U)

typedef struct __attribute__((aligned(16)))
{
  uint32_t magic;
  uint32_t schema;
  uint32_t initial_config_crc32c;
  uint32_t layout_version;
  uint32_t mcu_uid[3];
  uint16_t data_lock_policy;
  uint8_t private_key_slot;
  uint8_t reserved0;
  uint8_t atecc_serial[ATECC608_SERIAL_SIZE];
  uint8_t reserved1[3];
  uint8_t initial_config[ATECC608_CONFIG_SIZE];
  uint8_t target_config[ATECC608_CONFIG_SIZE];
  uint32_t crc32c;
  uint8_t reserved2[16];
  uint32_t commit;
  uint8_t commit_padding[12];
} SecurityFactory_Journal;

_Static_assert((sizeof(SecurityFactory_Journal) % ECU_FLASH_PROGRAM_UNIT) == 0U,
               "Factory journal must use complete flash quad-words");
_Static_assert(offsetof(SecurityFactory_Journal, commit) ==
               (sizeof(SecurityFactory_Journal) - ECU_FLASH_PROGRAM_UNIT),
               "Factory journal commit must be the final quad-word");

static bool SecurityIdentity_FactoryEraseSector(uint32_t sector)
{
  FLASH_EraseInitTypeDef erase = {
    .TypeErase = FLASH_TYPEERASE_SECTORS,
    .Banks = FLASH_BANK_1,
    .Sector = sector,
    .NbSectors = 1U
  };
  uint32_t sector_error = UINT32_MAX;
  return HAL_FLASHEx_Erase(&erase, &sector_error) == HAL_OK;
}

static bool SecurityIdentity_FactoryProgramRecord(
    uint32_t address, const void *record, size_t size, size_t commit_offset)
{
  size_t offset;

  for (offset = 0U; offset < commit_offset; offset += ECU_FLASH_PROGRAM_UNIT)
  {
    if (HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_QUADWORD, address + (uint32_t)offset,
            (uint32_t)(uintptr_t)((const uint8_t *)record + offset)) != HAL_OK)
    {
      return false;
    }
  }
  if ((commit_offset + ECU_FLASH_PROGRAM_UNIT) != size) { return false; }
  return HAL_FLASH_Program(
      FLASH_TYPEPROGRAM_QUADWORD, address + (uint32_t)commit_offset,
      (uint32_t)(uintptr_t)((const uint8_t *)record + commit_offset)) == HAL_OK;
}

bool SecurityIdentity_FactorySaveManifest(
    const SecurityIdentity_Manifest *manifest)
{
  SecurityIdentity_Manifest existing;
  SecurityIdentity_Record record;
  const uint32_t sector = ECU_SECURITY_STORE_OFFSET /
                          ECU_FLASH_SECTOR_SIZE;
  bool result;

  if ((manifest == NULL) || (manifest->generation == 0U) ||
      (manifest->private_key_slot > 15U))
  {
    return false;
  }
  if (SecurityIdentity_Load(&existing) == SECURITY_IDENTITY_OK)
  {
    return memcmp(&existing, manifest, sizeof(existing)) == 0;
  }
  memset(&record, 0xFF, sizeof(record));
  record.magic = SECURITY_IDENTITY_MAGIC;
  record.schema = SECURITY_IDENTITY_SCHEMA;
  record.generation = manifest->generation;
  record.layout_version = ECU_LAYOUT_VERSION;
  memcpy(record.mcu_uid, manifest->mcu_uid, sizeof(record.mcu_uid));
  record.atecc_config_crc32c = manifest->atecc_config_crc32c;
  memcpy(record.atecc_serial, manifest->atecc_serial,
         sizeof(record.atecc_serial));
  memcpy(record.atecc_revision, manifest->atecc_revision,
         sizeof(record.atecc_revision));
  record.atecc_i2c_address = manifest->atecc_i2c_address;
  record.private_key_slot = manifest->private_key_slot;
  record.identity_padding = 0U;
  memcpy(record.public_key, manifest->public_key, sizeof(record.public_key));
  record.record_size = sizeof(record);
  record.crc32c = SecurityIdentity_Crc32c(
      &record, offsetof(SecurityIdentity_Record, crc32c));
  record.commit = SECURITY_IDENTITY_COMMIT;

  if (HAL_FLASH_Unlock() != HAL_OK) { return false; }
  result = SecurityIdentity_FactoryEraseSector(sector) &&
           SecurityIdentity_FactoryProgramRecord(
               SECURITY_IDENTITY_ADDRESS_A, &record, sizeof(record),
               offsetof(SecurityIdentity_Record, commit));
  (void)HAL_FLASH_Lock();
  return result && SecurityIdentity_RecordValid(
      (const SecurityIdentity_Record *)(uintptr_t)
      SECURITY_IDENTITY_ADDRESS_A);
}

static bool SecurityIdentity_FactoryJournalValid(
    const SecurityFactory_Journal *journal)
{
  return (journal->magic == SECURITY_FACTORY_JOURNAL_MAGIC) &&
         (journal->schema == SECURITY_FACTORY_JOURNAL_SCHEMA) &&
         (journal->layout_version == ECU_LAYOUT_VERSION) &&
         (journal->private_key_slot <= 15U) &&
         (journal->commit == SECURITY_FACTORY_JOURNAL_COMMIT) &&
         (journal->crc32c == SecurityIdentity_Crc32c(
             journal, offsetof(SecurityFactory_Journal, crc32c)));
}

bool SecurityIdentity_FactorySaveJournal(
    uint32_t initial_config_crc32c,
    const uint8_t initial_config[ATECC608_CONFIG_SIZE],
    const uint8_t target_config[ATECC608_CONFIG_SIZE],
    uint16_t data_lock_policy, uint8_t private_key_slot,
    const uint8_t atecc_serial[ATECC608_SERIAL_SIZE])
{
  SecurityFactory_Journal journal;
  bool result;

  if ((initial_config == NULL) || (target_config == NULL) ||
      (atecc_serial == NULL) ||
      (private_key_slot > 15U))
  {
    return false;
  }
  memset(&journal, 0xFF, sizeof(journal));
  journal.magic = SECURITY_FACTORY_JOURNAL_MAGIC;
  journal.schema = SECURITY_FACTORY_JOURNAL_SCHEMA;
  journal.initial_config_crc32c = initial_config_crc32c;
  journal.layout_version = ECU_LAYOUT_VERSION;
  if (!SecurityMcuIdentity_Get(journal.mcu_uid)) { return false; }
  journal.data_lock_policy = data_lock_policy;
  journal.private_key_slot = private_key_slot;
  memcpy(journal.atecc_serial, atecc_serial,
         sizeof(journal.atecc_serial));
  memcpy(journal.initial_config, initial_config,
         sizeof(journal.initial_config));
  memcpy(journal.target_config, target_config,
         sizeof(journal.target_config));
  journal.crc32c = SecurityIdentity_Crc32c(
      &journal, offsetof(SecurityFactory_Journal, crc32c));
  journal.commit = SECURITY_FACTORY_JOURNAL_COMMIT;

  if (HAL_FLASH_Unlock() != HAL_OK) { return false; }
  result = SecurityIdentity_FactoryEraseSector(
               SECURITY_FACTORY_JOURNAL_SECTOR) &&
           SecurityIdentity_FactoryProgramRecord(
               SECURITY_FACTORY_JOURNAL_ADDRESS, &journal, sizeof(journal),
               offsetof(SecurityFactory_Journal, commit));
  (void)HAL_FLASH_Lock();
  return result && SecurityIdentity_FactoryJournalValid(
      (const SecurityFactory_Journal *)(uintptr_t)
      SECURITY_FACTORY_JOURNAL_ADDRESS);
}

bool SecurityIdentity_FactoryLoadJournal(
    uint32_t *initial_config_crc32c,
    uint8_t initial_config[ATECC608_CONFIG_SIZE],
    uint8_t target_config[ATECC608_CONFIG_SIZE],
    uint16_t *data_lock_policy, uint8_t *private_key_slot,
    uint8_t atecc_serial[ATECC608_SERIAL_SIZE])
{
  const SecurityFactory_Journal *journal =
      (const SecurityFactory_Journal *)(uintptr_t)
      SECURITY_FACTORY_JOURNAL_ADDRESS;

  if ((initial_config_crc32c == NULL) || (initial_config == NULL) ||
      (target_config == NULL) ||
      (data_lock_policy == NULL) || (private_key_slot == NULL) ||
      (atecc_serial == NULL) ||
      !SecurityIdentity_FactoryJournalValid(journal) ||
      !SecurityIdentity_McuUidMatches(journal->mcu_uid))
  {
    return false;
  }
  *initial_config_crc32c = journal->initial_config_crc32c;
  memcpy(initial_config, journal->initial_config, ATECC608_CONFIG_SIZE);
  memcpy(target_config, journal->target_config, ATECC608_CONFIG_SIZE);
  *data_lock_policy = journal->data_lock_policy;
  *private_key_slot = journal->private_key_slot;
  memcpy(atecc_serial, journal->atecc_serial, ATECC608_SERIAL_SIZE);
  return true;
}
#endif /* ECU_FACTORY_PROVISIONING */
