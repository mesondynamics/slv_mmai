#include "run_permit_policy.h"

bool RunPermitPolicy_CanClose(uint32_t safety_status,
                              uint32_t security_flags,
                              bool physical_estop_active,
                              bool running_images_confirmed)
{
  return ((safety_status & SAFETY_STATUS_READY) != 0U) &&
         ((security_flags & SAFETY_SECURITY_AUTHENTICATED) != 0U) &&
         !physical_estop_active && running_images_confirmed &&
         ((safety_status & SAFETY_STATUS_NETWORK_ESTOP_LATCHED) == 0U) &&
         ((safety_status & RUN_PERMIT_CRITICAL_STATUS_MASK) == 0U);
}
