#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#include "control_authority_policy.h"

typedef struct
{
  uint32_t session_generation;
  uint8_t sender_id;
} TestControlSlot;

static void test_timeout_boundaries(void)
{
  const uint32_t last = 1000U;

  assert(!ControlAuthorityPolicy_TimeExpired(1249U, last, 250U));
  assert(!ControlAuthorityPolicy_TimeExpired(1250U, last, 250U));
  assert(ControlAuthorityPolicy_TimeExpired(1251U, last, 250U));
  assert(ControlAuthorityPolicy_TimeExpired(1299U, last, 250U));
  assert(ControlAuthorityPolicy_TimeExpired(1300U, last, 250U));

  assert(!ControlAuthorityPolicy_TimeExpired(100U, UINT32_MAX - 149U,
                                             250U));
  assert(ControlAuthorityPolicy_TimeExpired(101U, UINT32_MAX - 149U,
                                            250U));
}

static void test_expired_sender_requires_neutral_and_a_full_safe_round(void)
{
  ControlAuthorityPolicy policy;

  ControlAuthorityPolicy_Init(&policy);
  ControlAuthorityPolicy_MarkAuthorityExpired(&policy, 3U);
  assert(ControlAuthorityPolicy_RearmRequired(&policy, 3U));
  assert(!ControlAuthorityPolicy_CanEstablish(&policy, 3U, false));
  assert(ControlAuthorityPolicy_CanEstablish(&policy, 3U, true));
  assert(!ControlAuthorityPolicy_CompleteNeutralRearm(&policy, 3U, false));
  assert(ControlAuthorityPolicy_RearmRequired(&policy, 3U));

  assert(ControlAuthorityPolicy_BeginSafetyRound(&policy, 1251U, 20U));
  assert(ControlAuthorityPolicy_SafetyHoldActive(&policy, 1251U));
  assert(ControlAuthorityPolicy_SafetyHoldActive(&policy, 1270U));
  assert(!ControlAuthorityPolicy_SafetyHoldActive(&policy, 1271U));
  assert(!ControlAuthorityPolicy_BeginSafetyRound(&policy, 1271U, 20U));

  assert(ControlAuthorityPolicy_CompleteNeutralRearm(&policy, 3U, true));
  assert(!ControlAuthorityPolicy_RearmRequired(&policy, 3U));
  assert(ControlAuthorityPolicy_CanEstablish(&policy, 3U, false));
}

static void test_same_slot_reuse_changes_session_identity(void)
{
  ControlAuthorityPolicy policy;
  TestControlSlot slot;
  uint32_t active_generation;

  ControlAuthorityPolicy_Init(&policy);
  slot.sender_id = 2U;
  slot.session_generation = ControlAuthorityPolicy_BeginSession(&policy);
  active_generation = slot.session_generation;
  assert(active_generation != 0U);

  ControlAuthorityPolicy_MarkAuthorityExpired(&policy, slot.sender_id);
  /* Reuse the exact same storage address for the restarted session. */
  slot.session_generation = ControlAuthorityPolicy_BeginSession(&policy);
  assert(slot.session_generation != active_generation);
  assert(!ControlAuthorityPolicy_CanEstablish(&policy, slot.sender_id,
                                              false));

  assert(ControlAuthorityPolicy_BeginSafetyRound(&policy, 1299U, 20U));
  assert(ControlAuthorityPolicy_SafetyHoldActive(&policy, 1318U));
  assert(!ControlAuthorityPolicy_SafetyHoldActive(&policy, 1319U));
  assert(ControlAuthorityPolicy_CompleteNeutralRearm(&policy, slot.sender_id,
                                                     true));

  policy.next_session_generation = UINT32_MAX;
  assert(ControlAuthorityPolicy_BeginSession(&policy) == 1U);
}

static void test_explicit_estop_reset_forces_a_safe_round(void)
{
  ControlAuthorityPolicy policy;

  ControlAuthorityPolicy_Init(&policy);
  ControlAuthorityPolicy_RequireRearm(&policy, 1U);
  ControlAuthorityPolicy_RequireRearm(&policy, 3U);
  ControlAuthorityPolicy_RequireSafeRound(&policy);
  assert(ControlAuthorityPolicy_RearmRequired(&policy, 1U));
  assert(ControlAuthorityPolicy_RearmRequired(&policy, 3U));
  assert(ControlAuthorityPolicy_BeginSafetyRound(&policy, 2000U, 20U));
  assert(ControlAuthorityPolicy_SafetyHoldActive(&policy, 2019U));
  assert(!ControlAuthorityPolicy_SafetyHoldActive(&policy, 2020U));
  assert(!ControlAuthorityPolicy_CanEstablish(&policy, 1U, false));
  assert(ControlAuthorityPolicy_CanEstablish(&policy, 1U, true));
}

int main(void)
{
  test_timeout_boundaries();
  test_expired_sender_requires_neutral_and_a_full_safe_round();
  test_same_slot_reuse_changes_session_identity();
  test_explicit_estop_reset_forces_a_safe_round();
  return 0;
}
