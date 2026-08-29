#include "security_mcu_identity.h"

#include <stddef.h>

#include "ecu_boot_handoff.h"
#include "stm32h5xx_hal.h"

enum
{
  SECURITY_MCU_IDENTITY_UNINITIALIZED = 0U,
  SECURITY_MCU_IDENTITY_VALID = 1U,
  SECURITY_MCU_IDENTITY_INVALID = 2U
};

static uint32_t security_mcu_uid[3];
static uint8_t security_mcu_identity_state;

#if defined(ECU_OEMIROT_LAYOUT)
static uint32_t SecurityMcuIdentity_Crc32c(const volatile void *data,
                                           size_t length)
{
  const volatile uint8_t *bytes = (const volatile uint8_t *)data;
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

static bool SecurityMcuIdentity_LoadHandoff(void)
{
  const volatile ECU_BootHandoff *handoff =
      (const volatile ECU_BootHandoff *)(uintptr_t)ECU_BOOT_HANDOFF_ADDRESS;
  uint32_t crc;
  uint32_t index;

  if ((handoff->magic != ECU_BOOT_HANDOFF_MAGIC) ||
      ((handoff->magic ^ handoff->magic_inv) != UINT32_MAX) ||
      (handoff->schema != ECU_BOOT_HANDOFF_SCHEMA) ||
      ((handoff->schema ^ handoff->schema_inv) != UINT32_MAX) ||
      (handoff->record_size != sizeof(ECU_BootHandoff)) ||
      ((handoff->record_size ^ handoff->record_size_inv) != UINT32_MAX) ||
      ((handoff->idcode ^ handoff->idcode_inv) != UINT32_MAX) ||
      ((handoff->idcode & 0x0FFFUL) != 0x0484UL) ||
      ((handoff->crc32c ^ handoff->crc32c_inv) != UINT32_MAX))
  {
    return false;
  }
  for (index = 0U; index < 3U; ++index)
  {
    if ((handoff->mcu_uid[index] ^ handoff->mcu_uid_inv[index]) !=
        UINT32_MAX)
    {
      return false;
    }
  }
  if (((handoff->mcu_uid[0] | handoff->mcu_uid[1] |
        handoff->mcu_uid[2]) == 0U) ||
      ((handoff->mcu_uid[0] & handoff->mcu_uid[1] &
        handoff->mcu_uid[2]) == UINT32_MAX))
  {
    return false;
  }
  crc = SecurityMcuIdentity_Crc32c(
      &handoff->schema,
      offsetof(ECU_BootHandoff, crc32c) -
          offsetof(ECU_BootHandoff, schema));
  if (crc != handoff->crc32c)
  {
    return false;
  }
  for (index = 0U; index < 3U; ++index)
  {
    security_mcu_uid[index] = handoff->mcu_uid[index];
  }
  return true;
}
#endif

bool SecurityMcuIdentity_Get(uint32_t mcu_uid[3])
{
  uint32_t index;

  if (mcu_uid == NULL)
  {
    return false;
  }
  if (security_mcu_identity_state == SECURITY_MCU_IDENTITY_UNINITIALIZED)
  {
#if defined(ECU_OEMIROT_LAYOUT)
    security_mcu_identity_state = SecurityMcuIdentity_LoadHandoff() ?
        SECURITY_MCU_IDENTITY_VALID : SECURITY_MCU_IDENTITY_INVALID;
#else
    security_mcu_uid[0] = HAL_GetUIDw0();
    security_mcu_uid[1] = HAL_GetUIDw1();
    security_mcu_uid[2] = HAL_GetUIDw2();
    security_mcu_identity_state =
        ((security_mcu_uid[0] | security_mcu_uid[1] |
          security_mcu_uid[2]) != 0U) ?
        SECURITY_MCU_IDENTITY_VALID : SECURITY_MCU_IDENTITY_INVALID;
#endif
  }
  if (security_mcu_identity_state != SECURITY_MCU_IDENTITY_VALID)
  {
    mcu_uid[0] = 0U;
    mcu_uid[1] = 0U;
    mcu_uid[2] = 0U;
    return false;
  }
  for (index = 0U; index < 3U; ++index)
  {
    mcu_uid[index] = security_mcu_uid[index];
  }
  return true;
}
