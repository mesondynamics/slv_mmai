#include <assert.h>
#include <stdint.h>

#include "valve_control.h"

static uint16_t current_to_raw(uint16_t current_ma)
{
  return (uint16_t)(((uint32_t)current_ma * 4095U) / 3300U);
}

static void calibrate(ValveControl *control)
{
  uint32_t index;
  for (index = 0U; index < 2048U; ++index)
  {
    ValveControl_Step(control, 0U, 0U, 3300U, false);
  }
  assert(control->calibrated == 1U);
  assert(ValveControl_IsIdle(control));
}

int main(void)
{
  SAFETY_ValveConfig config;
  ValveControl control;
  uint32_t index;
  uint32_t forward_plant = 0U;
  uint32_t reverse_plant = 0U;

  ValveControl_DefaultConfig(&config);
  assert(config.version == SAFETY_VALVE_CONFIG_VERSION);
  assert(config.size == sizeof(config));
  assert(ValveControl_ValidateConfig(&config));
  config.forward.filter_cutoff_hz = 49U;
  assert(!ValveControl_ValidateConfig(&config));
  ValveControl_DefaultConfig(&config);
  ValveControl_Init(&control, &config);
  calibrate(&control);

  assert(ValveControl_SetTarget(&control, 49) == SAFETY_RESULT_OK);
  assert(control.requested_target_ma == 0);
  assert(ValveControl_SetTarget(&control, 2001) == SAFETY_RESULT_RANGE);
  assert(ValveControl_SetTarget(&control, 1000) == SAFETY_RESULT_OK);
  for (index = 0U; index < 80000U; ++index)
  {
    forward_plant += ((uint32_t)control.forward_duty_permille * 3U);
    forward_plant /= 2U;
    ValveControl_Step(&control, current_to_raw((uint16_t)forward_plant), 0U,
                      3300U, true);
  }
  assert(control.state == SAFETY_VALVE_STATE_FORWARD);
  assert(control.forward_current_ma > 850U);
  assert(control.forward_current_ma < 1150U);
  assert(control.reverse_duty_permille == 0U);

  assert(ValveControl_SetTarget(&control, -1000) == SAFETY_RESULT_OK);
  for (index = 0U; index < 160000U; ++index)
  {
    forward_plant += ((uint32_t)control.forward_duty_permille * 3U);
    forward_plant /= 2U;
    reverse_plant += ((uint32_t)control.reverse_duty_permille * 3U);
    reverse_plant /= 2U;
    ValveControl_Step(&control, current_to_raw((uint16_t)forward_plant),
                      current_to_raw((uint16_t)reverse_plant), 3300U, true);
  }
  assert(control.state == SAFETY_VALVE_STATE_REVERSE);
  assert(control.reverse_current_ma > 850U);
  assert(control.reverse_current_ma < 1150U);
  assert(control.forward_duty_permille == 0U);
  assert(control.fault_flags == 0U);

  ValveControl_Init(&control, &config);
  calibrate(&control);
  assert(ValveControl_SetTarget(&control, 1000) == SAFETY_RESULT_OK);
  for (index = 0U; index < 200U; ++index)
  {
    ValveControl_Step(&control, current_to_raw(2600U), 0U, 3300U, true);
  }
  assert((control.fault_flags & SAFETY_STATUS_VALVE_OVERCURRENT) != 0U);
  assert(control.forward_duty_permille == 0U);
  assert(control.reverse_duty_permille == 0U);
  return 0;
}
