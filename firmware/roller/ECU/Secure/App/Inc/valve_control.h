#ifndef VALVE_CONTROL_H
#define VALVE_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "safety_api.h"

#define VALVE_CONTROL_FREQUENCY_HZ 20000UL
#define VALVE_TELEMETRY_DECIMATION 20U

typedef struct
{
  SAFETY_ValveConfig config;
  int64_t forward_integrator_q20;
  int64_t reverse_integrator_q20;
  int32_t forward_filtered_q16;
  int32_t reverse_filtered_q16;
  int32_t applied_target_q16;
  uint32_t forward_zero_accumulator;
  uint32_t reverse_zero_accumulator;
  uint32_t loop_sequence;
  uint32_t fault_flags;
  uint16_t forward_zero_raw;
  uint16_t reverse_zero_raw;
  uint16_t forward_current_ma;
  uint16_t reverse_current_ma;
  uint16_t forward_duty_permille;
  uint16_t reverse_duty_permille;
  uint16_t forward_alpha_q15;
  uint16_t reverse_alpha_q15;
  uint16_t calibration_samples;
  uint16_t low_current_samples;
  uint16_t deadtime_samples;
  uint16_t direction_transition_samples;
  uint16_t overcurrent_samples;
  uint16_t inactive_current_samples;
  uint16_t output_off_current_samples;
  uint16_t open_load_samples;
  int16_t requested_target_ma;
  SAFETY_ValveState state;
  uint8_t calibrated;
  uint8_t limited;
} ValveControl;

void ValveControl_DefaultConfig(SAFETY_ValveConfig *config);
bool ValveControl_ValidateConfig(const SAFETY_ValveConfig *config);
void ValveControl_Init(ValveControl *control, const SAFETY_ValveConfig *config);
void ValveControl_ForceSafe(ValveControl *control);
int32_t ValveControl_SetTarget(ValveControl *control, int16_t target_ma);
int32_t ValveControl_ApplyConfig(ValveControl *control,
                                 const SAFETY_ValveConfig *config);
void ValveControl_Step(ValveControl *control, uint16_t forward_raw,
                       uint16_t reverse_raw, uint16_t vdda_mv,
                       bool outputs_allowed);
uint16_t ValveControl_ForwardCompare(const ValveControl *control);
uint16_t ValveControl_ReverseCompare(const ValveControl *control);
int16_t ValveControl_AppliedTargetMa(const ValveControl *control);
bool ValveControl_IsIdle(const ValveControl *control);

#endif /* VALVE_CONTROL_H */
