#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "run_permit_policy.h"

static bool can_close(uint32_t additional_status)
{
  return RunPermitPolicy_CanClose(
      SAFETY_STATUS_READY | SAFETY_STATUS_ATECC_AUTHENTICATED |
          additional_status,
      SAFETY_SECURITY_AUTHENTICATED, false, true);
}

int main(void)
{
  static const uint32_t critical_faults[] = {
    SAFETY_STATUS_GTZC_VIOLATION,
    SAFETY_STATUS_INTERNAL_ERROR,
    SAFETY_STATUS_TPIC_ERROR,
    SAFETY_STATUS_SW_I2C_BUS_FAULT,
    SAFETY_STATUS_ATECC_MISSING,
    SAFETY_STATUS_ATECC_UNPAIRED,
    SAFETY_STATUS_ATECC_AUTH_FAILED,
    SAFETY_STATUS_OTA_ACTIVE,
    SAFETY_STATUS_OTA_READY,
    SAFETY_STATUS_OTA_UNCONFIRMED,
    SAFETY_STATUS_NETWORK_ESTOP_LATCHED
  };
  uint32_t index;

  assert(can_close(0U));
  assert(can_close(SAFETY_STATUS_FAULT_LATCHED));
  assert(can_close(SAFETY_STATUS_COMMAND_TIMEOUT));
  assert(can_close(SAFETY_STATUS_VALVE_OVERCURRENT));
  assert(can_close(SAFETY_STATUS_STEERING_CAN_FAULT));
  assert(can_close(SAFETY_STATUS_VEHICLE_CAN_FAULT));

  for (index = 0U; index <
       (sizeof(critical_faults) / sizeof(critical_faults[0])); ++index)
  {
    assert(!can_close(critical_faults[index]));
  }
  assert(!RunPermitPolicy_CanClose(
      SAFETY_STATUS_READY, 0U, false, true));
  assert(!RunPermitPolicy_CanClose(
      SAFETY_STATUS_READY, SAFETY_SECURITY_AUTHENTICATED, true, true));
  assert(!RunPermitPolicy_CanClose(
      SAFETY_STATUS_READY, SAFETY_SECURITY_AUTHENTICATED, false, false));
  assert(!RunPermitPolicy_CanClose(
      0U, SAFETY_SECURITY_AUTHENTICATED, false, true));
  return 0;
}
