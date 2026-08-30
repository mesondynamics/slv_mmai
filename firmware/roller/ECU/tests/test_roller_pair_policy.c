#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "roller_pair_policy.h"

enum
{
  TEST_SWAP_NONE = 1,
  TEST_SWAP_TEST = 2,
  TEST_SWAP_PERM = 3,
  TEST_SWAP_REVERT = 4,
  TEST_SWAP_FAIL = 5,
  TEST_SWAP_PANIC = 0xFF
};

int main(void)
{
  const uint8_t swap_values[] = {
    TEST_SWAP_NONE, TEST_SWAP_TEST, TEST_SWAP_PERM,
    TEST_SWAP_REVERT, TEST_SWAP_FAIL, TEST_SWAP_PANIC
  };
  uint8_t pair[2];
  uint32_t left;
  uint32_t right;
  RollerPair_ReleaseIdentity identity = {
    .major = 1U,
    .minor = 2U,
    .revision = 3U,
    .build = 4U,
    .security_counter = 5U
  };
  RollerPair_ReleaseIdentity changed;
  RollerPair_ReleaseIdentity identities[2];
  bool primary_verified[2] = { true, true };

  for (left = 0U; left < sizeof(swap_values); ++left)
  {
    for (right = 0U; right < sizeof(swap_values); ++right)
    {
      const int expected =
          ((swap_values[left] == TEST_SWAP_NONE) ||
           (swap_values[left] == TEST_SWAP_REVERT)) &&
          ((swap_values[right] == TEST_SWAP_NONE) ||
           (swap_values[right] == TEST_SWAP_REVERT)) &&
          ((swap_values[left] == TEST_SWAP_REVERT) ||
           (swap_values[right] == TEST_SWAP_REVERT));
      pair[0] = swap_values[left];
      pair[1] = swap_values[right];
      assert(RollerPair_SwapTypesRequireRevert(
                 pair, 2U, TEST_SWAP_NONE, TEST_SWAP_REVERT) ==
             (expected != 0));
      assert(RollerPair_SwapTypesFormOperation(
                 pair, 2U, TEST_SWAP_NONE, TEST_SWAP_REVERT) ==
             (expected != 0));
      assert(RollerPair_SwapTypesFormOperation(
                 pair, 2U, TEST_SWAP_NONE, TEST_SWAP_TEST) ==
             ((((swap_values[left] == TEST_SWAP_NONE) ||
                (swap_values[left] == TEST_SWAP_TEST)) &&
               ((swap_values[right] == TEST_SWAP_NONE) ||
                (swap_values[right] == TEST_SWAP_TEST)) &&
               ((swap_values[left] == TEST_SWAP_TEST) ||
                (swap_values[right] == TEST_SWAP_TEST))) != 0));
      assert(RollerPair_SwapTypesConflict(
                 pair, 2U, TEST_SWAP_TEST, TEST_SWAP_REVERT) ==
             (((swap_values[left] == TEST_SWAP_TEST) ||
               (swap_values[right] == TEST_SWAP_TEST)) &&
              ((swap_values[left] == TEST_SWAP_REVERT) ||
               (swap_values[right] == TEST_SWAP_REVERT))));
    }
  }

  for (left = 0U; left <= UINT8_MAX; ++left)
  {
    assert(RollerPair_PartialSwapIsForbidden(
               (uint8_t)left, TEST_SWAP_PERM) ==
           (left == TEST_SWAP_PERM));
    for (right = 0U; right <= UINT8_MAX; ++right)
    {
      pair[0] = (uint8_t)left;
      pair[1] = (uint8_t)right;
      assert(RollerPair_AllImagesConfirmed(pair, 2U, 0x01U) ==
             ((left == 0x01U) && (right == 0x01U)));
    }
  }

  assert(!RollerPair_SwapTypesRequireRevert(
      NULL, 2U, TEST_SWAP_NONE, TEST_SWAP_REVERT));
  assert(!RollerPair_SwapTypesFormOperation(
      NULL, 2U, TEST_SWAP_NONE, TEST_SWAP_TEST));
  assert(RollerPair_SwapTypesConflict(
      NULL, 2U, TEST_SWAP_TEST, TEST_SWAP_REVERT));
  assert(!RollerPair_AllImagesConfirmed(NULL, 2U, 0x01U));
  assert(!RollerPair_AllImagesConfirmed(pair, 1U, 0x01U));

  assert(RollerPair_ReleaseIdentitiesMatch(&identity, &identity));
  assert(!RollerPair_ReleaseIdentitiesMatch(NULL, &identity));
  assert(!RollerPair_ReleaseIdentitiesMatch(&identity, NULL));
  changed = identity;
  changed.major++;
  assert(!RollerPair_ReleaseIdentitiesMatch(&identity, &changed));
  changed = identity;
  changed.minor++;
  assert(!RollerPair_ReleaseIdentitiesMatch(&identity, &changed));
  changed = identity;
  changed.revision++;
  assert(!RollerPair_ReleaseIdentitiesMatch(&identity, &changed));
  changed = identity;
  changed.build++;
  assert(!RollerPair_ReleaseIdentitiesMatch(&identity, &changed));
  changed = identity;
  changed.security_counter++;
  assert(!RollerPair_ReleaseIdentitiesMatch(&identity, &changed));

  pair[0] = 0x01U;
  pair[1] = 0x01U;
  identities[0] = identity;
  identities[1] = identity;
  assert(RollerPair_ReleaseReadyToCommit(
      pair, primary_verified, identities, 2U, 0x01U));
  primary_verified[0] = false;
  assert(!RollerPair_ReleaseReadyToCommit(
      pair, primary_verified, identities, 2U, 0x01U));
  primary_verified[0] = true;
  primary_verified[1] = false;
  assert(!RollerPair_ReleaseReadyToCommit(
      pair, primary_verified, identities, 2U, 0x01U));
  primary_verified[1] = true;
  identities[1].security_counter++;
  assert(!RollerPair_ReleaseReadyToCommit(
      pair, primary_verified, identities, 2U, 0x01U));
  identities[1] = identity;
  pair[1] = 0xFFU;
  assert(!RollerPair_ReleaseReadyToCommit(
      pair, primary_verified, identities, 2U, 0x01U));
  pair[1] = 0x01U;
  assert(!RollerPair_ReleaseReadyToCommit(
      NULL, primary_verified, identities, 2U, 0x01U));
  assert(!RollerPair_ReleaseReadyToCommit(
      pair, NULL, identities, 2U, 0x01U));
  assert(!RollerPair_ReleaseReadyToCommit(
      pair, primary_verified, NULL, 2U, 0x01U));
  assert(!RollerPair_ReleaseReadyToCommit(
      pair, primary_verified, identities, 1U, 0x01U));

  /* BOOT_SWAP_TYPE_FAIL may clear invalid staging only when this exact gate
     was true before the swap loop; it must never manufacture TEST health. */
  identities[0] = identity;
  identities[1] = identity;
  primary_verified[0] = true;
  primary_verified[1] = true;
  pair[0] = 0xFFU;
  pair[1] = 0xFFU;
  assert(!RollerPair_ReleaseReadyToCommit(
      pair, primary_verified, identities, 2U, 0x01U));
  pair[0] = 0x01U;
  assert(!RollerPair_ReleaseReadyToCommit(
      pair, primary_verified, identities, 2U, 0x01U));
  pair[1] = 0x01U;
  identities[1].build++;
  assert(!RollerPair_ReleaseReadyToCommit(
      pair, primary_verified, identities, 2U, 0x01U));
  identities[1] = identity;
  primary_verified[1] = false;
  assert(!RollerPair_ReleaseReadyToCommit(
      pair, primary_verified, identities, 2U, 0x01U));
  primary_verified[1] = true;
  assert(RollerPair_ReleaseReadyToCommit(
      pair, primary_verified, identities, 2U, 0x01U));

  puts("roller_pair_policy: PASS");
  return 0;
}
