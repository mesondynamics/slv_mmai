#include "secure_flash_guard.h"

#include "stm32h5xx_hal.h"

static void SecureFlash_ResetAfterLockFailure(void)
{
  __DSB();
  NVIC_SystemReset();
  for (;;)
  {
    __NOP();
  }
}

void SecureFlash_LockSecureOrReset(void)
{
  if (HAL_FLASH_Lock_S() != HAL_OK)
  {
    SecureFlash_ResetAfterLockFailure();
  }
}

void SecureFlash_LockNonSecureOrReset(void)
{
  if (HAL_FLASH_Lock_NS() != HAL_OK)
  {
    SecureFlash_ResetAfterLockFailure();
  }
}
