#include "valve_config_store.h"

#include <stddef.h>
#include <string.h>

#include "secure_flash_guard.h"
#include "stm32h5xx_hal.h"

#define VALVE_STORE_MAGIC          0x43465056UL /* "VPFC" */
#define VALVE_STORE_COMMIT         0x54494D43UL /* "CMIT" */
#define VALVE_STORE_SECTOR_SIZE    8192UL
#define VALVE_STORE_SECTOR_A       125U
#define VALVE_STORE_SECTOR_B       126U
#define VALVE_STORE_ADDRESS_A      0x0C0FA000UL
#define VALVE_STORE_ADDRESS_B      0x0C0FC000UL

typedef struct __attribute__((aligned(16)))
{
  uint32_t magic;
  uint32_t schema;
  uint32_t generation;
  uint32_t payload_size;
  SAFETY_ValveConfig config;
  uint32_t crc32c;
  uint8_t reserved[12];
  uint32_t commit;
  uint8_t commit_padding[12];
} ValveConfigRecord;

_Static_assert((sizeof(ValveConfigRecord) % 16U) == 0U,
               "Flash record must use complete quad-words");
_Static_assert(offsetof(ValveConfigRecord, commit) ==
               (sizeof(ValveConfigRecord) - 16U),
               "Commit marker must occupy the final quad-word");

typedef struct
{
  const ValveConfigRecord *record;
  uint32_t address;
  uint32_t next_free;
} ValveSectorScan;

uint32_t ValveConfigStore_Crc32c(const void *data, uint32_t length)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t index;
  uint32_t bit;

  for (index = 0U; index < length; ++index)
  {
    crc ^= bytes[index];
    for (bit = 0U; bit < 8U; ++bit)
    {
      crc = (crc >> 1U) ^
            ((crc & 1U) ? 0x82F63B78UL : 0U);
    }
  }
  return crc ^ 0xFFFFFFFFUL;
}

static bool ValveConfigStore_GenerationNewer(uint32_t value,
                                              uint32_t previous)
{
  uint32_t difference = value - previous;
  return (difference != 0U) && (difference < 0x80000000UL);
}

static bool ValveConfigStore_RecordValid(const ValveConfigRecord *record)
{
  uint32_t calculated;

  if ((record->magic != VALVE_STORE_MAGIC) ||
      (record->schema != SAFETY_VALVE_CONFIG_VERSION) ||
      (record->payload_size != sizeof(record->config)) ||
      (record->commit != VALVE_STORE_COMMIT))
  {
    return false;
  }
  calculated = ValveConfigStore_Crc32c(
      record, offsetof(ValveConfigRecord, crc32c));
  return calculated == record->crc32c;
}

static void ValveConfigStore_ScanSector(uint32_t address,
                                        ValveSectorScan *scan)
{
  uint32_t offset;

  memset(scan, 0, sizeof(*scan));
  scan->address = address;
  scan->next_free = UINT32_MAX;
  for (offset = 0U;
       (offset + sizeof(ValveConfigRecord)) <= VALVE_STORE_SECTOR_SIZE;
       offset += sizeof(ValveConfigRecord))
  {
    const ValveConfigRecord *candidate =
        (const ValveConfigRecord *)(uintptr_t)(address + offset);
    if (candidate->magic == 0xFFFFFFFFUL)
    {
      if (scan->next_free == UINT32_MAX) { scan->next_free = address + offset; }
      continue;
    }
    if (ValveConfigStore_RecordValid(candidate) &&
        ((scan->record == NULL) ||
         ValveConfigStore_GenerationNewer(candidate->generation,
                                           scan->record->generation)))
    {
      scan->record = candidate;
    }
  }
}

static const ValveConfigRecord *ValveConfigStore_Newest(
    const ValveSectorScan *a, const ValveSectorScan *b)
{
  if (a->record == NULL) { return b->record; }
  if (b->record == NULL) { return a->record; }
  return ValveConfigStore_GenerationNewer(b->record->generation,
                                          a->record->generation) ?
         b->record : a->record;
}

bool ValveConfigStore_Load(SAFETY_ValveConfig *config, uint32_t *generation,
                           uint32_t *crc32c)
{
  ValveSectorScan a;
  ValveSectorScan b;
  const ValveConfigRecord *newest;

  if ((config == NULL) || (generation == NULL) || (crc32c == NULL))
  {
    return false;
  }
  ValveConfigStore_ScanSector(VALVE_STORE_ADDRESS_A, &a);
  ValveConfigStore_ScanSector(VALVE_STORE_ADDRESS_B, &b);
  newest = ValveConfigStore_Newest(&a, &b);
  if (newest == NULL) { return false; }
  *config = newest->config;
  *generation = newest->generation;
  *crc32c = newest->crc32c;
  return true;
}

static bool ValveConfigStore_Erase(uint32_t sector)
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

static bool ValveConfigStore_InvalidateFlashCache(void)
{
  if (HAL_ICACHE_IsEnabled() == 0U) { return true; }
  __DSB();
  if (HAL_ICACHE_Invalidate() != HAL_OK) { return false; }
  __DSB();
  __ISB();
  return true;
}

static bool ValveConfigStore_Program(uint32_t address,
                                     const ValveConfigRecord *record)
{
  uint32_t offset;
  uint32_t committed_offset = offsetof(ValveConfigRecord, commit);

  for (offset = 0U; offset < committed_offset; offset += 16U)
  {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD, address + offset,
                          (uint32_t)(uintptr_t)((const uint8_t *)record +
                                                offset)) != HAL_OK)
    {
      return false;
    }
  }
  if (HAL_FLASH_Program(
          FLASH_TYPEPROGRAM_QUADWORD, address + committed_offset,
          (uint32_t)(uintptr_t)((const uint8_t *)record + committed_offset)) !=
      HAL_OK)
  {
    return false;
  }
  return ValveConfigStore_InvalidateFlashCache();
}

bool ValveConfigStore_Save(const SAFETY_ValveConfig *config,
                           uint32_t generation, uint32_t *crc32c)
{
  ValveSectorScan a;
  ValveSectorScan b;
  const ValveConfigRecord *newest;
  ValveConfigRecord record;
  uint32_t destination;
  uint32_t erase_sector = UINT32_MAX;
  bool result;

  if ((config == NULL) || (crc32c == NULL)) { return false; }
  ValveConfigStore_ScanSector(VALVE_STORE_ADDRESS_A, &a);
  ValveConfigStore_ScanSector(VALVE_STORE_ADDRESS_B, &b);
  newest = ValveConfigStore_Newest(&a, &b);

  if ((newest != NULL) &&
      ((uintptr_t)newest >= (uintptr_t)VALVE_STORE_ADDRESS_B))
  {
    destination = b.next_free;
    if (destination == UINT32_MAX)
    {
      destination = VALVE_STORE_ADDRESS_A;
      erase_sector = VALVE_STORE_SECTOR_A;
    }
  }
  else
  {
    destination = a.next_free;
    if (destination == UINT32_MAX)
    {
      destination = VALVE_STORE_ADDRESS_B;
      erase_sector = VALVE_STORE_SECTOR_B;
    }
  }
  if (destination == UINT32_MAX) { return false; }

  memset(&record, 0xFF, sizeof(record));
  record.magic = VALVE_STORE_MAGIC;
  record.schema = SAFETY_VALVE_CONFIG_VERSION;
  record.generation = generation;
  record.payload_size = sizeof(record.config);
  record.config = *config;
  record.crc32c = ValveConfigStore_Crc32c(
      &record, offsetof(ValveConfigRecord, crc32c));
  record.commit = VALVE_STORE_COMMIT;

  /* Valve calibration is Secure Bank 1 data.  Keep the NonSecure FLASH
     controller locked throughout the transaction. */
  if (HAL_FLASH_Unlock_S() != HAL_OK) { return false; }
  if ((erase_sector != UINT32_MAX) && !ValveConfigStore_Erase(erase_sector))
  {
    SecureFlash_LockSecureOrReset();
    return false;
  }
  result = ValveConfigStore_Program(destination, &record);
  SecureFlash_LockSecureOrReset();
  if (!result || !ValveConfigStore_RecordValid(
      (const ValveConfigRecord *)(uintptr_t)destination))
  {
    return false;
  }
  *crc32c = record.crc32c;
  return true;
}
