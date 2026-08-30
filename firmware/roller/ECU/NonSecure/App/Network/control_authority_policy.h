#ifndef CONTROL_AUTHORITY_POLICY_H
#define CONTROL_AUTHORITY_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONTROL_AUTHORITY_SENDER_COUNT 256U
#define CONTROL_AUTHORITY_REARM_WORDS   (CONTROL_AUTHORITY_SENDER_COUNT / 32U)

typedef struct
{
  uint32_t sender_rearm_mask[CONTROL_AUTHORITY_REARM_WORDS];
  uint32_t next_session_generation;
  uint32_t safety_hold_deadline;
  uint8_t safety_round_pending;
  uint8_t safety_hold_active;
} ControlAuthorityPolicy;

static inline void ControlAuthorityPolicy_Init(ControlAuthorityPolicy *policy)
{
  uint32_t index;

  if (policy == NULL)
  {
    return;
  }
  for (index = 0U; index < CONTROL_AUTHORITY_REARM_WORDS; ++index)
  {
    policy->sender_rearm_mask[index] = 0U;
  }
  policy->next_session_generation = 0U;
  policy->safety_hold_deadline = 0U;
  policy->safety_round_pending = 0U;
  policy->safety_hold_active = 0U;
}

static inline bool ControlAuthorityPolicy_TimeExpired(uint32_t now,
                                                       uint32_t last,
                                                       uint32_t timeout)
{
  /* Signed modular subtraction is valid for reviewed intervals below
     INT32_MAX. A 250 ms lease is live through age 250 and expired at 251. */
  return (int32_t)(now - last) > (int32_t)timeout;
}

static inline uint32_t ControlAuthorityPolicy_BeginSession(
    ControlAuthorityPolicy *policy)
{
  if (policy == NULL)
  {
    return 0U;
  }
  ++policy->next_session_generation;
  if (policy->next_session_generation == 0U)
  {
    ++policy->next_session_generation;
  }
  return policy->next_session_generation;
}

static inline void ControlAuthorityPolicy_RequireRearm(
    ControlAuthorityPolicy *policy, uint8_t sender_id)
{
  if (policy != NULL)
  {
    policy->sender_rearm_mask[sender_id >> 5U] |=
        1UL << (sender_id & 31U);
  }
}

static inline bool ControlAuthorityPolicy_RearmRequired(
    const ControlAuthorityPolicy *policy, uint8_t sender_id)
{
  return (policy != NULL) &&
         ((policy->sender_rearm_mask[sender_id >> 5U] &
           (1UL << (sender_id & 31U))) != 0U);
}

static inline bool ControlAuthorityPolicy_CanEstablish(
    const ControlAuthorityPolicy *policy, uint8_t sender_id,
    bool neutral_disabled)
{
  return !ControlAuthorityPolicy_RearmRequired(policy, sender_id) ||
         neutral_disabled;
}

static inline void ControlAuthorityPolicy_MarkAuthorityExpired(
    ControlAuthorityPolicy *policy, uint8_t sender_id)
{
  if (policy == NULL)
  {
    return;
  }
  ControlAuthorityPolicy_RequireRearm(policy, sender_id);
  policy->safety_round_pending = 1U;
}

static inline bool ControlAuthorityPolicy_BeginSafetyRound(
    ControlAuthorityPolicy *policy, uint32_t now, uint32_t hold_ms)
{
  if ((policy == NULL) || (policy->safety_round_pending == 0U))
  {
    return false;
  }
  policy->safety_round_pending = 0U;
  policy->safety_hold_active = 1U;
  policy->safety_hold_deadline = now + hold_ms;
  return true;
}

static inline bool ControlAuthorityPolicy_SafetyHoldActive(
    ControlAuthorityPolicy *policy, uint32_t now)
{
  if ((policy == NULL) || (policy->safety_hold_active == 0U))
  {
    return false;
  }
  if ((int32_t)(now - policy->safety_hold_deadline) >= 0)
  {
    policy->safety_hold_active = 0U;
    return false;
  }
  return true;
}

static inline bool ControlAuthorityPolicy_CompleteNeutralRearm(
    ControlAuthorityPolicy *policy, uint8_t sender_id,
    bool neutral_was_applied)
{
  if ((policy == NULL) || !neutral_was_applied ||
      !ControlAuthorityPolicy_RearmRequired(policy, sender_id))
  {
    return false;
  }
  policy->sender_rearm_mask[sender_id >> 5U] &=
      ~(1UL << (sender_id & 31U));
  return true;
}

#endif /* CONTROL_AUTHORITY_POLICY_H */
