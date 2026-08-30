#ifndef ROLLER_PAIR_POLICY_H
#define ROLLER_PAIR_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
  uint8_t major;
  uint8_t minor;
  uint16_t revision;
  uint32_t build;
  uint32_t security_counter;
} RollerPair_ReleaseIdentity;

static inline bool RollerPair_ReleaseIdentitiesMatch(
    const RollerPair_ReleaseIdentity *left,
    const RollerPair_ReleaseIdentity *right)
{
  return (left != NULL) && (right != NULL) &&
      (left->major == right->major) &&
      (left->minor == right->minor) &&
      (left->revision == right->revision) &&
      (left->build == right->build) &&
      (left->security_counter == right->security_counter);
}

/* True only when every side is either stationary or performing the same
   operation, and at least one side still needs that operation. This describes
   both a clean paired swap and the resumable state after one side completed. */
static inline bool RollerPair_SwapTypesFormOperation(
    const uint8_t *swap_types, size_t image_count,
    uint8_t none_type, uint8_t operation_type)
{
  bool saw_operation = false;
  size_t index;

  if ((swap_types == NULL) || (image_count < 2U))
  {
    return false;
  }
  for (index = 0U; index < image_count; ++index)
  {
    if (swap_types[index] == operation_type)
    {
      saw_operation = true;
    }
    else if (swap_types[index] != none_type)
    {
      /* A different/failed operation cannot form one paired transition. */
      return false;
    }
  }
  return saw_operation;
}

/* Roller ECU ships Secure and NonSecure as one dependency-locked release.
   A partially programmed image_ok pair must therefore revert as one unit. */
static inline bool RollerPair_SwapTypesRequireRevert(
    const uint8_t *swap_types, size_t image_count,
    uint8_t none_type, uint8_t revert_type)
{
  return RollerPair_SwapTypesFormOperation(
      swap_types, image_count, none_type, revert_type);
}

static inline bool RollerPair_SwapTypesConflict(
    const uint8_t *swap_types, size_t image_count,
    uint8_t test_type, uint8_t revert_type)
{
  bool saw_test = false;
  bool saw_revert = false;
  size_t index;

  if ((swap_types == NULL) || (image_count < 2U))
  {
    return true;
  }
  for (index = 0U; index < image_count; ++index)
  {
    saw_test = saw_test || (swap_types[index] == test_type);
    saw_revert = saw_revert || (swap_types[index] == revert_type);
  }
  return saw_test && saw_revert;
}

/* A resumed PERM swap has already crossed the point where the later clean-swap
   policy can reject it.  Detect it before MCUboot performs any recovery write. */
static inline bool RollerPair_PartialSwapIsForbidden(
    uint8_t swap_type, uint8_t permanent_type)
{
  return swap_type == permanent_type;
}

/* Anti-rollback counters are advanced only after every application image is
   explicitly confirmed. This prevents a one-flag power loss from making the
   previous paired release permanently ineligible for rollback. */
static inline bool RollerPair_AllImagesConfirmed(
    const uint8_t *image_ok, size_t image_count, uint8_t set_flag)
{
  size_t index;

  if ((image_ok == NULL) || (image_count < 2U))
  {
    return false;
  }
  for (index = 0U; index < image_count; ++index)
  {
    if (image_ok[index] != set_flag)
    {
      return false;
    }
  }
  return true;
}

/* Advancing an NV anti-rollback counter is irreversible.  Confirmation bits
   alone are not sufficient: every primary must have passed full MCUboot image
   validation in this boot and all protected release identities must describe
   the same paired release. */
static inline bool RollerPair_ReleaseReadyToCommit(
    const uint8_t *image_ok,
    const bool *primary_verified,
    const RollerPair_ReleaseIdentity *identities,
    size_t image_count,
    uint8_t set_flag)
{
  size_t index;

  if ((primary_verified == NULL) || (identities == NULL) ||
      !RollerPair_AllImagesConfirmed(image_ok, image_count, set_flag))
  {
    return false;
  }

  for (index = 0U; index < image_count; ++index)
  {
    if (!primary_verified[index])
    {
      return false;
    }
    if ((index != 0U) &&
        !RollerPair_ReleaseIdentitiesMatch(&identities[0], &identities[index]))
    {
      return false;
    }
  }
  return true;
}

#endif /* ROLLER_PAIR_POLICY_H */
