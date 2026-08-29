#include "security_ota.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "ecu_flash_layout.h"
#include "ecu_ota_transport_public_key.h"
#include "iwdg.h"
#include "main.h"
#include "security_crypto.h"
#include "security_sha256.h"
#include "stm32h5xx_hal.h"

#define OTA_RECORD_MAGIC             0x5241544FUL /* "OTAR" */
#define OTA_RECORD_COMMIT            0x54494D43UL /* "CMIT" */
#define OTA_RECORD_ADDRESS_A \
  (ECU_FLASH_BASE_S + ECU_OTA_JOURNAL_OFFSET)
#define OTA_RECORD_ADDRESS_B \
  (OTA_RECORD_ADDRESS_A + ECU_FLASH_SECTOR_SIZE)
#define OTA_RECORD_SECTOR_A \
  (ECU_OTA_JOURNAL_OFFSET / ECU_FLASH_SECTOR_SIZE)
#define OTA_RECORD_SECTOR_B          (OTA_RECORD_SECTOR_A + 1U)
#define OTA_IMAGE_MAGIC              0x96F3B83DUL
#define OTA_IMAGE_ENCRYPTED_FLAG     0x00000004UL
#define OTA_REQUIRED_FLAGS \
  (SAFETY_OTA_FLAG_ENCRYPTED_IMAGES | SAFETY_OTA_FLAG_TEST_SWAP)
#define OTA_IMAGE_HEADER_SIZE        32U
#define OTA_IMAGE_HEADER_FLAGS       16U
#define OTA_BOOT_MAGIC_SIZE          16U
#define OTA_BOOT_MAX_ALIGN           16U
#define OTA_IMAGE_OK_OFFSET_FROM_END \
  (OTA_BOOT_MAGIC_SIZE + OTA_BOOT_MAX_ALIGN)

static const uint8_t ota_transport_public_key[64] =
    ECU_OTA_TRANSPORT_PUBLIC_KEY_BYTES;
static const uint8_t ota_boot_magic[16] = {
  0x77,0xc2,0x95,0xf3,0x60,0xd2,0xef,0x7f,
  0x35,0x52,0x50,0x0f,0x2c,0xb6,0x79,0x80
};

typedef struct __attribute__((aligned(16)))
{
  uint32_t magic;
  uint32_t schema;
  uint32_t generation;
  uint32_t accepted_sequence;
  uint8_t manifest_sha256[32];
  uint32_t record_size;
  uint32_t crc32c;
  uint8_t reserved[8];
  uint32_t commit;
  uint8_t commit_padding[12];
} SecurityOta_Record;

_Static_assert(sizeof(SAFETY_OtaManifest) == 128U,
               "OTA manifest ABI must be canonical");
_Static_assert(sizeof(SAFETY_OtaBeginRequest) == 192U,
               "OTA begin ABI changed");
_Static_assert(sizeof(SAFETY_OtaChunk) == 532U,
               "OTA chunk ABI changed");
_Static_assert(sizeof(SAFETY_OtaStatus) == 36U,
               "OTA status ABI changed");
_Static_assert(sizeof(SecurityOta_Record) == 80U,
               "OTA journal ABI changed");
_Static_assert(OTA_BOOT_MAX_ALIGN == ECU_FLASH_PROGRAM_UNIT,
               "MCUboot alignment must match the H563 Flash program unit");
_Static_assert(OTA_IMAGE_OK_OFFSET_FROM_END == 32U,
               "MCUboot image_ok flag offset changed");
_Static_assert(offsetof(SecurityOta_Record, commit) ==
               (sizeof(SecurityOta_Record) - ECU_FLASH_PROGRAM_UNIT),
               "OTA journal commit must be the final quad-word");

static SAFETY_OtaStatus ota_status;
static SAFETY_OtaManifest ota_manifest;
static SecuritySha256_Context ota_hash[2];
static uint8_t ota_manifest_hash[32];
static uint32_t ota_record_generation;

static uint32_t SecurityOta_Crc32c(const void *data, size_t size)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFUL;
  size_t index;
  uint32_t bit;

  for (index = 0U; index < size; ++index)
  {
    crc ^= bytes[index];
    for (bit = 0U; bit < 8U; ++bit)
    {
      crc = (crc >> 1U) ^ ((crc & 1U) ? 0x82F63B78UL : 0U);
    }
  }
  return crc ^ 0xFFFFFFFFUL;
}

static bool SecurityOta_SequenceNewer(uint32_t candidate,
                                      uint32_t reference)
{
  uint32_t difference = candidate - reference;
  return (difference != 0U) && (difference < 0x80000000UL);
}

static bool SecurityOta_RecordValid(const SecurityOta_Record *record)
{
  return (record->magic == OTA_RECORD_MAGIC) &&
         (record->schema == SAFETY_OTA_MANIFEST_SCHEMA) &&
         (record->record_size == sizeof(*record)) &&
         (record->commit == OTA_RECORD_COMMIT) &&
         (record->crc32c == SecurityOta_Crc32c(
             record, offsetof(SecurityOta_Record, crc32c)));
}

static const SecurityOta_Record *SecurityOta_NewestRecord(void)
{
  const SecurityOta_Record *a =
      (const SecurityOta_Record *)(uintptr_t)OTA_RECORD_ADDRESS_A;
  const SecurityOta_Record *b =
      (const SecurityOta_Record *)(uintptr_t)OTA_RECORD_ADDRESS_B;
  bool a_valid = SecurityOta_RecordValid(a);
  bool b_valid = SecurityOta_RecordValid(b);

  if (!a_valid) { return b_valid ? b : NULL; }
  if (!b_valid) { return a; }
  return SecurityOta_SequenceNewer(b->generation, a->generation) ? b : a;
}

static bool SecurityOta_EraseSector(uint32_t sector)
{
  FLASH_EraseInitTypeDef erase = {
    .TypeErase = FLASH_TYPEERASE_SECTORS,
    .Banks = FLASH_BANK_1,
    .Sector = sector,
    .NbSectors = 1U
  };
  uint32_t sector_error = UINT32_MAX;
  (void)HAL_IWDG_Refresh(&hiwdg);
  return HAL_FLASHEx_Erase(&erase, &sector_error) == HAL_OK;
}

static bool SecurityOta_Program(uint32_t address, const void *data,
                                size_t size)
{
  size_t offset;

  if (((address | (uint32_t)size) &
       (ECU_FLASH_PROGRAM_UNIT - 1U)) != 0U)
  {
    return false;
  }
  for (offset = 0U; offset < size; offset += ECU_FLASH_PROGRAM_UNIT)
  {
    if (HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_QUADWORD, address + (uint32_t)offset,
            (uint32_t)(uintptr_t)((const uint8_t *)data + offset)) != HAL_OK)
    {
      return false;
    }
  }
  return memcmp((const void *)(uintptr_t)address, data, size) == 0;
}

static bool SecurityOta_SaveAcceptedRecord(void)
{
  SecurityOta_Record record;
  const SecurityOta_Record *newest = SecurityOta_NewestRecord();
  uint32_t address;
  uint32_t sector;
  bool result;

  memset(&record, 0xFF, sizeof(record));
  record.magic = OTA_RECORD_MAGIC;
  record.schema = SAFETY_OTA_MANIFEST_SCHEMA;
  record.generation = ota_record_generation + 1U;
  record.accepted_sequence = ota_manifest.update_sequence;
  memcpy(record.manifest_sha256, ota_manifest_hash,
         sizeof(record.manifest_sha256));
  record.record_size = sizeof(record);
  record.crc32c = SecurityOta_Crc32c(
      &record, offsetof(SecurityOta_Record, crc32c));
  record.commit = OTA_RECORD_COMMIT;

  if ((newest != NULL) &&
      ((uintptr_t)newest < (uintptr_t)OTA_RECORD_ADDRESS_B))
  {
    address = OTA_RECORD_ADDRESS_B;
    sector = OTA_RECORD_SECTOR_B;
  }
  else
  {
    address = OTA_RECORD_ADDRESS_A;
    sector = OTA_RECORD_SECTOR_A;
  }
  if (HAL_FLASH_Unlock() != HAL_OK) { return false; }
  result = SecurityOta_EraseSector(sector) &&
           SecurityOta_Program(
               address, &record, offsetof(SecurityOta_Record, commit)) &&
           SecurityOta_Program(
               address + offsetof(SecurityOta_Record, commit),
               (const uint8_t *)&record +
                   offsetof(SecurityOta_Record, commit),
               ECU_FLASH_PROGRAM_UNIT);
  (void)HAL_FLASH_Lock();
  if (result && SecurityOta_RecordValid(
                    (const SecurityOta_Record *)(uintptr_t)address))
  {
    ota_record_generation = record.generation;
    ota_status.accepted_sequence = record.accepted_sequence;
    return true;
  }
  return false;
}

static bool SecurityOta_ManifestValid(const SAFETY_OtaManifest *manifest)
{
  static const uint32_t zero_reserved[4] = {0U, 0U, 0U, 0U};

  return (manifest->magic == SAFETY_OTA_MANIFEST_MAGIC) &&
         (manifest->schema == SAFETY_OTA_MANIFEST_SCHEMA) &&
         (manifest->layout_version == ECU_LAYOUT_VERSION) &&
         (manifest->update_sequence != 0U) &&
         (manifest->security_counter != 0U) &&
         (manifest->flags == OTA_REQUIRED_FLAGS) &&
         (manifest->secure_image_size == ECU_SECURE_SECONDARY_SIZE) &&
         (manifest->nonsecure_image_size == ECU_NONSECURE_SECONDARY_SIZE) &&
         (memcmp(manifest->reserved, zero_reserved,
                 sizeof(zero_reserved)) == 0);
}

static uint32_t SecurityOta_ImageAddress(uint8_t image_index)
{
  if (image_index == 0U)
  {
    return ECU_FLASH_BASE_S + ECU_SECURE_SECONDARY_OFFSET;
  }
  return ECU_FLASH_BASE_S + ECU_NONSECURE_SECONDARY_OFFSET;
}

static uint32_t SecurityOta_Received(uint8_t image_index)
{
  return (image_index == 0U) ? ota_status.secure_received :
                               ota_status.nonsecure_received;
}

static void SecurityOta_SetReceived(uint8_t image_index, uint32_t value)
{
  if (image_index == 0U) { ota_status.secure_received = value; }
  else { ota_status.nonsecure_received = value; }
}

void SecurityOta_Init(void)
{
  const SecurityOta_Record *record = SecurityOta_NewestRecord();

  memset(&ota_status, 0, sizeof(ota_status));
  memset(&ota_manifest, 0, sizeof(ota_manifest));
  memset(ota_manifest_hash, 0, sizeof(ota_manifest_hash));
  ota_status.api_version = SAFETY_OTA_API_VERSION;
  ota_status.state = SAFETY_OTA_STATE_IDLE;
  ota_status.result = SAFETY_RESULT_OK;
  ota_record_generation = 0U;
  if (record != NULL)
  {
    ota_record_generation = record->generation;
    ota_status.accepted_sequence = record->accepted_sequence;
  }
}

int32_t SecurityOta_GetStatus(SAFETY_OtaStatus *status)
{
  if (status == NULL) { return SAFETY_RESULT_BAD_ARGUMENT; }
  *status = ota_status;
  return SAFETY_RESULT_OK;
}

int32_t SecurityOta_Begin(const SAFETY_OtaBeginRequest *request,
                          SAFETY_OtaStatus *status)
{
  uint8_t digest[32];
  uint32_t first_sector = ECU_SECURE_SECONDARY_OFFSET /
                          ECU_FLASH_SECTOR_SIZE;
  uint32_t sector_count = (ECU_SECURE_SECONDARY_SIZE +
                           ECU_NONSECURE_SECONDARY_SIZE) /
                          ECU_FLASH_SECTOR_SIZE;
  uint32_t index;
  bool erase_ok = true;

  if ((request == NULL) || (status == NULL) ||
      !SecurityOta_ManifestValid(&request->manifest))
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  SecuritySha256_Compute(&request->manifest, sizeof(request->manifest),
                         digest);
  if (!SecurityCrypto_VerifyP256(ota_transport_public_key, digest,
                                 request->signature))
  {
    memset(digest, 0, sizeof(digest));
    ota_status.result = SAFETY_RESULT_AUTHENTICATION;
    *status = ota_status;
    return SAFETY_RESULT_AUTHENTICATION;
  }
  if (!SecurityOta_SequenceNewer(request->manifest.update_sequence,
                                 ota_status.accepted_sequence))
  {
    memset(digest, 0, sizeof(digest));
    ota_status.result = SAFETY_RESULT_ROLLBACK;
    *status = ota_status;
    return SAFETY_RESULT_ROLLBACK;
  }
  if (ota_status.state == SAFETY_OTA_STATE_RECEIVING)
  {
    if (memcmp(digest, ota_manifest_hash, sizeof(digest)) == 0)
    {
      ota_status.result = SAFETY_RESULT_OK;
      *status = ota_status;
      memset(digest, 0, sizeof(digest));
      return SAFETY_RESULT_OK;
    }
    memset(digest, 0, sizeof(digest));
    return SAFETY_RESULT_BUSY;
  }

  if (HAL_FLASH_Unlock() != HAL_OK) { erase_ok = false; }
  for (index = 0U; erase_ok && (index < sector_count); ++index)
  {
    erase_ok = SecurityOta_EraseSector(first_sector + index);
  }
  (void)HAL_FLASH_Lock();
  if (!erase_ok)
  {
    ota_status.state = SAFETY_OTA_STATE_ERROR;
    ota_status.result = SAFETY_RESULT_STORAGE;
    *status = ota_status;
    memset(digest, 0, sizeof(digest));
    return SAFETY_RESULT_STORAGE;
  }
  ota_manifest = request->manifest;
  memcpy(ota_manifest_hash, digest, sizeof(ota_manifest_hash));
  memset(digest, 0, sizeof(digest));
  SecuritySha256_Init(&ota_hash[0]);
  SecuritySha256_Init(&ota_hash[1]);
  ota_status.state = SAFETY_OTA_STATE_RECEIVING;
  ota_status.result = SAFETY_RESULT_OK;
  ota_status.update_sequence = ota_manifest.update_sequence;
  ota_status.secure_received = 0U;
  ota_status.nonsecure_received = 0U;
  ota_status.secure_image_size = ota_manifest.secure_image_size;
  ota_status.nonsecure_image_size = ota_manifest.nonsecure_image_size;
  *status = ota_status;
  return SAFETY_RESULT_OK;
}

int32_t SecurityOta_Write(const SAFETY_OtaChunk *chunk,
                          SAFETY_OtaStatus *status)
{
  uint32_t received;
  uint32_t image_size;
  uint32_t address;
  bool programmed;

  if ((chunk == NULL) || (status == NULL) || (chunk->image_index > 1U) ||
      (chunk->data_size == 0U) ||
      (chunk->data_size > SAFETY_OTA_CHUNK_SIZE) ||
      ((chunk->offset | chunk->data_size) &
       (ECU_FLASH_PROGRAM_UNIT - 1U)) != 0U ||
      (chunk->reserved0[0] != 0U) || (chunk->reserved0[1] != 0U) ||
      (chunk->reserved0[2] != 0U) || (chunk->reserved1 != 0U))
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  if ((ota_status.state != SAFETY_OTA_STATE_RECEIVING) ||
      (chunk->update_sequence != ota_status.update_sequence))
  {
    *status = ota_status;
    return SAFETY_RESULT_CONFLICT;
  }
  if (SecurityOta_Crc32c(chunk->data, chunk->data_size) !=
      chunk->data_crc32c)
  {
    *status = ota_status;
    return SAFETY_RESULT_INTEGRITY;
  }
  received = SecurityOta_Received(chunk->image_index);
  image_size = (chunk->image_index == 0U) ?
               ota_manifest.secure_image_size :
               ota_manifest.nonsecure_image_size;
  address = SecurityOta_ImageAddress(chunk->image_index) + chunk->offset;
  if ((chunk->offset + chunk->data_size) > image_size)
  {
    return SAFETY_RESULT_RANGE;
  }
  if (chunk->offset < received)
  {
    if (((chunk->offset + chunk->data_size) <= received) &&
        (memcmp((const void *)(uintptr_t)address, chunk->data,
                chunk->data_size) == 0))
    {
      ota_status.result = SAFETY_RESULT_OK;
      *status = ota_status;
      return SAFETY_RESULT_OK;
    }
    return SAFETY_RESULT_CONFLICT;
  }
  if (chunk->offset != received) { return SAFETY_RESULT_CONFLICT; }

  if (HAL_FLASH_Unlock() != HAL_OK) { programmed = false; }
  else
  {
    programmed = SecurityOta_Program(address, chunk->data,
                                     chunk->data_size);
    (void)HAL_FLASH_Lock();
  }
  if (!programmed)
  {
    ota_status.state = SAFETY_OTA_STATE_ERROR;
    ota_status.result = SAFETY_RESULT_STORAGE;
    *status = ota_status;
    return SAFETY_RESULT_STORAGE;
  }
  SecuritySha256_Update(&ota_hash[chunk->image_index], chunk->data,
                        chunk->data_size);
  SecurityOta_SetReceived(chunk->image_index,
                          chunk->offset + chunk->data_size);
  ota_status.result = SAFETY_RESULT_OK;
  (void)HAL_IWDG_Refresh(&hiwdg);
  *status = ota_status;
  return SAFETY_RESULT_OK;
}

static bool SecurityOta_ImageEnvelopeValid(uint32_t address,
                                           uint32_t image_size)
{
  uint32_t magic;
  uint32_t flags;
  memcpy(&magic, (const void *)(uintptr_t)address, sizeof(magic));
  memcpy(&flags, (const void *)(uintptr_t)(address + OTA_IMAGE_HEADER_FLAGS),
         sizeof(flags));
  return (magic == OTA_IMAGE_MAGIC) &&
         ((flags & OTA_IMAGE_ENCRYPTED_FLAG) != 0U) &&
         (memcmp((const void *)(uintptr_t)(address + image_size -
                                           sizeof(ota_boot_magic)),
                 ota_boot_magic, sizeof(ota_boot_magic)) == 0);
}

int32_t SecurityOta_Finish(uint32_t update_sequence,
                           SAFETY_OtaStatus *status)
{
  uint8_t secure_digest[32];
  uint8_t nonsecure_digest[32];
  uint32_t secure_address = SecurityOta_ImageAddress(0U);
  uint32_t nonsecure_address = SecurityOta_ImageAddress(1U);
  bool valid;

  if (status == NULL) { return SAFETY_RESULT_BAD_ARGUMENT; }
  if ((ota_status.state != SAFETY_OTA_STATE_RECEIVING) ||
      (update_sequence != ota_status.update_sequence) ||
      (ota_status.secure_received != ota_manifest.secure_image_size) ||
      (ota_status.nonsecure_received != ota_manifest.nonsecure_image_size))
  {
    *status = ota_status;
    return SAFETY_RESULT_CONFLICT;
  }
  SecuritySha256_Final(&ota_hash[0], secure_digest);
  SecuritySha256_Final(&ota_hash[1], nonsecure_digest);
  valid = (memcmp(secure_digest, ota_manifest.secure_sha256,
                  sizeof(secure_digest)) == 0) &&
          (memcmp(nonsecure_digest, ota_manifest.nonsecure_sha256,
                  sizeof(nonsecure_digest)) == 0) &&
          SecurityOta_ImageEnvelopeValid(
              secure_address, ota_manifest.secure_image_size) &&
          SecurityOta_ImageEnvelopeValid(
              nonsecure_address, ota_manifest.nonsecure_image_size);
  memset(secure_digest, 0, sizeof(secure_digest));
  memset(nonsecure_digest, 0, sizeof(nonsecure_digest));
  if (!valid)
  {
    ota_status.state = SAFETY_OTA_STATE_ERROR;
    ota_status.result = SAFETY_RESULT_INTEGRITY;
    *status = ota_status;
    return SAFETY_RESULT_INTEGRITY;
  }
  if (!SecurityOta_SaveAcceptedRecord())
  {
    ota_status.state = SAFETY_OTA_STATE_ERROR;
    ota_status.result = SAFETY_RESULT_STORAGE;
    *status = ota_status;
    return SAFETY_RESULT_STORAGE;
  }
  ota_status.state = SAFETY_OTA_STATE_READY;
  ota_status.result = SAFETY_RESULT_OK;
  *status = ota_status;
  return SAFETY_RESULT_OK;
}

#if defined(ECU_OEMIROT_LAYOUT)
static bool SecurityOta_ConfirmFlag(uint32_t address, uint32_t program_type)
{
  const uint32_t program_address =
      address & ~(ECU_FLASH_PROGRAM_UNIT - 1U);
  const uint32_t flag_offset = address - program_address;
  uint8_t value[ECU_FLASH_PROGRAM_UNIT];

  if (flag_offset >= sizeof(value)) { return false; }
  memcpy(value, (const void *)(uintptr_t)program_address, sizeof(value));
  if (value[flag_offset] == 0x01U) { return true; }
  if (value[flag_offset] != 0xFFU) { return false; }
  value[flag_offset] = 0x01U;
  return (HAL_FLASH_Program(program_type, program_address,
                            (uint32_t)(uintptr_t)value) == HAL_OK) &&
         (*(const uint8_t *)(uintptr_t)address == 0x01U);
}
#endif

int32_t SecurityOta_ConfirmRunningImages(void)
{
#if defined(ECU_OEMIROT_LAYOUT)
  const uint32_t secure_flag = ECU_FLASH_BASE_S +
      ECU_SECURE_PRIMARY_OFFSET + ECU_SECURE_PRIMARY_SIZE -
      OTA_IMAGE_OK_OFFSET_FROM_END;
  const uint32_t nonsecure_flag = ECU_FLASH_BASE_NS +
      ECU_NONSECURE_PRIMARY_OFFSET + ECU_NONSECURE_PRIMARY_SIZE -
      OTA_IMAGE_OK_OFFSET_FROM_END;
  bool result;

  if (HAL_FLASH_Unlock() != HAL_OK) { return SAFETY_RESULT_STORAGE; }
  result = SecurityOta_ConfirmFlag(secure_flag,
                                   FLASH_TYPEPROGRAM_QUADWORD) &&
           SecurityOta_ConfirmFlag(nonsecure_flag,
                                   FLASH_TYPEPROGRAM_QUADWORD_NS);
  (void)HAL_FLASH_Lock();
  return result ? SAFETY_RESULT_OK : SAFETY_RESULT_STORAGE;
#else
  return SAFETY_RESULT_OK;
#endif
}
