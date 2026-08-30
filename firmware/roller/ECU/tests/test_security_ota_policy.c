#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "security_ota_policy.h"

int main(void)
{
  uint32_t confirmed = UINT32_MAX;
  uint32_t secure_value;
  uint32_t nonsecure_value;

  assert(SecurityOta_EvaluateConfirmationFlags(0x01U, 0x01U, &confirmed));
  assert(confirmed == 1U);

  assert(SecurityOta_EvaluateConfirmationFlags(0xFFU, 0xFFU, &confirmed));
  assert(confirmed == 0U);
  assert(SecurityOta_EvaluateConfirmationFlags(0x01U, 0xFFU, &confirmed));
  assert(confirmed == 0U);
  assert(SecurityOta_EvaluateConfirmationFlags(0xFFU, 0x01U, &confirmed));
  assert(confirmed == 0U);

  /* Exhaustively prove that only erased/programmed MCUboot flag bytes are
     accepted and that only the fully confirmed pair evaluates true. */
  for (secure_value = 0U; secure_value <= UINT8_MAX; ++secure_value)
  {
    for (nonsecure_value = 0U; nonsecure_value <= UINT8_MAX;
         ++nonsecure_value)
    {
      const int valid = ((secure_value == 0x01U) ||
                         (secure_value == 0xFFU)) &&
                        ((nonsecure_value == 0x01U) ||
                         (nonsecure_value == 0xFFU));
      confirmed = UINT32_MAX;
      assert(SecurityOta_EvaluateConfirmationFlags(
                 (uint8_t)secure_value, (uint8_t)nonsecure_value,
                 &confirmed) == (valid != 0));
      assert(confirmed == (((secure_value == 0x01U) &&
                            (nonsecure_value == 0x01U)) ? 1U : 0U));
    }
  }
  assert(!SecurityOta_EvaluateConfirmationFlags(0x01U, 0x01U, NULL));

  puts("security_ota_policy: PASS");
  return 0;
}
