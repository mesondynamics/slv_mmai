#include "security_ota_policy.h"

#include <stddef.h>

bool SecurityOta_EvaluateConfirmationFlags(uint8_t secure_value,
                                           uint8_t nonsecure_value,
                                           uint32_t *confirmed)
{
  if (confirmed == NULL)
  {
    return false;
  }
  if (((secure_value != 0x01U) && (secure_value != 0xFFU)) ||
      ((nonsecure_value != 0x01U) && (nonsecure_value != 0xFFU)))
  {
    *confirmed = 0U;
    return false;
  }
  *confirmed = ((secure_value == 0x01U) &&
                (nonsecure_value == 0x01U)) ? 1U : 0U;
  return true;
}
