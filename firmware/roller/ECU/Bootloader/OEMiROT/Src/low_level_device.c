/*
 * Restricted flash write policy for OEMiROT.
 *
 * The upstream example exposes one continuous write range. This product has a
 * persistent secure region between the NS secondary and NS primary slots, so
 * enumerate the exact MCUboot areas and make accidental parameter erasure fail.
 */
#include "flash_layout.h"
#include "low_level_flash.h"

static struct flash_range erase_vect[] =
{
  { FLASH_AREA_SCRATCH_OFFSET,
    FLASH_AREA_SCRATCH_OFFSET + FLASH_AREA_SCRATCH_SIZE - 1 },
  { FLASH_AREA_0_OFFSET, FLASH_AREA_0_OFFSET + FLASH_AREA_0_SIZE - 1 },
  { FLASH_AREA_1_OFFSET, FLASH_AREA_1_OFFSET + FLASH_AREA_1_SIZE - 1 },
  { FLASH_AREA_2_OFFSET, FLASH_AREA_2_OFFSET + FLASH_AREA_2_SIZE - 1 },
  { FLASH_AREA_3_OFFSET, FLASH_AREA_3_OFFSET + FLASH_AREA_3_SIZE - 1 }
};

static struct flash_range write_vect[] =
{
  { FLASH_AREA_SCRATCH_OFFSET,
    FLASH_AREA_SCRATCH_OFFSET + FLASH_AREA_SCRATCH_SIZE - 1 },
  { FLASH_AREA_0_OFFSET, FLASH_AREA_0_OFFSET + FLASH_AREA_0_SIZE - 1 },
  { FLASH_AREA_1_OFFSET, FLASH_AREA_1_OFFSET + FLASH_AREA_1_SIZE - 1 },
  { FLASH_AREA_2_OFFSET, FLASH_AREA_2_OFFSET + FLASH_AREA_2_SIZE - 1 },
  { FLASH_AREA_3_OFFSET, FLASH_AREA_3_OFFSET + FLASH_AREA_3_SIZE - 1 }
};

#if defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3U)
/* Bank 1 is Secure; the NS primary executes from NonSecure Bank 2. */
static struct flash_range secure_vect[] =
{
  { FLASH_AREA_SCRATCH_OFFSET,
    FLASH_AREA_SCRATCH_OFFSET + FLASH_AREA_SCRATCH_SIZE - 1 },
  { FLASH_AREA_0_OFFSET, FLASH_AREA_0_OFFSET + FLASH_AREA_0_SIZE - 1 },
  { FLASH_AREA_2_OFFSET, FLASH_AREA_2_OFFSET + FLASH_AREA_2_SIZE - 1 },
  { FLASH_AREA_3_OFFSET, FLASH_AREA_3_OFFSET + FLASH_AREA_3_SIZE - 1 }
};
#endif

struct low_level_device FLASH0_DEV =
{
  .erase = { .nb = sizeof(erase_vect) / sizeof(erase_vect[0]),
             .range = erase_vect },
  .write = { .nb = sizeof(write_vect) / sizeof(write_vect[0]),
             .range = write_vect },
#if defined(__ARM_FEATURE_CMSE) && (__ARM_FEATURE_CMSE == 3U)
  .secure = { .nb = sizeof(secure_vect) / sizeof(secure_vect[0]),
              .range = secure_vect },
#endif
  .read_error = 1
};
