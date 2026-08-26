#include "valve_control.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define VALVE_CALIBRATION_SAMPLES       2048U
#define VALVE_CURRENT_ZERO_LIMIT_MA      100U
#define VALVE_OVERCURRENT_FAST_MA       2500U
#define VALVE_OVERCURRENT_SUSTAINED_MA  2200U
#define VALVE_OVERCURRENT_SAMPLES         40U
#define VALVE_INACTIVE_CURRENT_MA        150U
#define VALVE_INACTIVE_CURRENT_SAMPLES   200U
#define VALVE_OUTPUT_OFF_CURRENT_MA      100U
#define VALVE_OUTPUT_OFF_SAMPLES        2000U
#define VALVE_OPEN_LOAD_TARGET_MA        300U
#define VALVE_OPEN_LOAD_CURRENT_MA        50U
#define VALVE_OPEN_LOAD_SAMPLES         6000U
#define VALVE_DIRECTION_ZERO_SAMPLES     400U
#define VALVE_DIRECTION_DEADTIME_SAMPLES 100U
#define VALVE_DIRECTION_TIMEOUT_SAMPLES 10000U
#define VALVE_Q20_ONE                 1048576LL

static uint16_t ValveControl_FilterAlpha(uint16_t cutoff_hz)
{
  uint32_t numerator = 6283UL * cutoff_hz;
  uint32_t denominator = 20000000UL + numerator;
  return (uint16_t)(((uint64_t)numerator * 32768ULL +
                     (denominator / 2UL)) / denominator);
}

static bool ValveControl_ValidateChannel(
    const SAFETY_ValveChannelConfig *channel)
{
  return (channel != NULL) &&
         (channel->kp_permille_per_amp <= 2000U) &&
         (channel->ki_permille_per_amp_second <= 20000U) &&
         (channel->filter_cutoff_hz >= 50U) &&
         (channel->filter_cutoff_hz <= 2000U) &&
         (channel->rise_slew_ma_per_s >= 50U) &&
         (channel->rise_slew_ma_per_s <= 10000U) &&
         (channel->fall_slew_ma_per_s >= 50U) &&
         (channel->fall_slew_ma_per_s <= 10000U) &&
         (channel->max_duty_permille >= 50U) &&
         (channel->max_duty_permille <= 950U) &&
         (channel->current_gain_ppm >= 900000) &&
         (channel->current_gain_ppm <= 1100000) &&
         (channel->current_offset_ma >= -100) &&
         (channel->current_offset_ma <= 100) &&
         (channel->reserved == 0U);
}

void ValveControl_DefaultConfig(SAFETY_ValveConfig *config)
{
  SAFETY_ValveChannelConfig defaults = {
    .kp_permille_per_amp = 400U,
    .ki_permille_per_amp_second = 4000U,
    .filter_cutoff_hz = 500U,
    .rise_slew_ma_per_s = 1000U,
    .fall_slew_ma_per_s = 2000U,
    .max_duty_permille = 600U,
    .current_gain_ppm = 1000000,
    .current_offset_ma = 0,
    .reserved = 0U
  };

  if (config == NULL)
  {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->version = SAFETY_VALVE_CONFIG_VERSION;
  config->size = sizeof(*config);
  config->forward = defaults;
  config->reverse = defaults;
}

bool ValveControl_ValidateConfig(const SAFETY_ValveConfig *config)
{
  return (config != NULL) &&
         (config->version == SAFETY_VALVE_CONFIG_VERSION) &&
         (config->size == sizeof(*config)) &&
         ValveControl_ValidateChannel(&config->forward) &&
         ValveControl_ValidateChannel(&config->reverse);
}

void ValveControl_Init(ValveControl *control, const SAFETY_ValveConfig *config)
{
  SAFETY_ValveConfig defaults;

  if (control == NULL)
  {
    return;
  }
  memset(control, 0, sizeof(*control));
  if (!ValveControl_ValidateConfig(config))
  {
    ValveControl_DefaultConfig(&defaults);
    config = &defaults;
  }
  control->config = *config;
  control->forward_alpha_q15 =
      ValveControl_FilterAlpha(config->forward.filter_cutoff_hz);
  control->reverse_alpha_q15 =
      ValveControl_FilterAlpha(config->reverse.filter_cutoff_hz);
  control->state = SAFETY_VALVE_STATE_OFF;
}

void ValveControl_ForceSafe(ValveControl *control)
{
  if (control == NULL)
  {
    return;
  }
  control->requested_target_ma = 0;
  control->applied_target_q16 = 0;
  control->forward_integrator_q20 = 0;
  control->reverse_integrator_q20 = 0;
  control->forward_duty_permille = 0U;
  control->reverse_duty_permille = 0U;
  control->low_current_samples = 0U;
  control->deadtime_samples = 0U;
  control->direction_transition_samples = 0U;
  control->state = (control->fault_flags != 0U) ?
                   SAFETY_VALVE_STATE_FAULT : SAFETY_VALVE_STATE_OFF;
}

int32_t ValveControl_SetTarget(ValveControl *control, int16_t target_ma)
{
  if (control == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  if ((target_ma < -SAFETY_VALVE_TARGET_MAX_MA) ||
      (target_ma > SAFETY_VALVE_TARGET_MAX_MA))
  {
    return SAFETY_RESULT_RANGE;
  }
  if ((target_ma > -SAFETY_VALVE_TARGET_DEADBAND_MA) &&
      (target_ma < SAFETY_VALVE_TARGET_DEADBAND_MA))
  {
    target_ma = 0;
  }
  control->requested_target_ma = target_ma;
  return SAFETY_RESULT_OK;
}

static int32_t ValveControl_Clamp32(int64_t value, int32_t minimum,
                                    int32_t maximum)
{
  if (value < minimum) { return minimum; }
  if (value > maximum) { return maximum; }
  return (int32_t)value;
}

static uint16_t ValveControl_ConvertCurrent(uint16_t raw, uint16_t zero_raw,
                                            uint16_t vdda_mv,
                                            const SAFETY_ValveChannelConfig *cfg)
{
  int32_t delta = (int32_t)raw - (int32_t)zero_raw;
  int64_t current_ma;

  if (delta < 0) { delta = 0; }
  current_ma = ((int64_t)delta * vdda_mv * cfg->current_gain_ppm) /
               (4095LL * 1000000LL);
  current_ma += cfg->current_offset_ma;
  return (uint16_t)ValveControl_Clamp32(current_ma, 0, 4000);
}

static int32_t ValveControl_RampTarget(int32_t current_q16,
                                       uint16_t desired_ma,
                                       const SAFETY_ValveChannelConfig *cfg)
{
  int32_t desired_q16 = (int32_t)desired_ma << 16;
  uint16_t rate = (desired_q16 > current_q16) ?
                  cfg->rise_slew_ma_per_s : cfg->fall_slew_ma_per_s;
  int32_t step_q16 = (int32_t)(((uint32_t)rate << 16) /
                               VALVE_CONTROL_FREQUENCY_HZ);

  if (step_q16 < 1) { step_q16 = 1; }
  if (current_q16 < desired_q16)
  {
    current_q16 += step_q16;
    if (current_q16 > desired_q16) { current_q16 = desired_q16; }
  }
  else if (current_q16 > desired_q16)
  {
    current_q16 -= step_q16;
    if (current_q16 < desired_q16) { current_q16 = desired_q16; }
  }
  return current_q16;
}

static uint16_t ValveControl_RunPi(int32_t target_ma, int32_t current_ma,
                                   const SAFETY_ValveChannelConfig *cfg,
                                   int64_t *integrator_q20, uint8_t *limited)
{
  int32_t error_ma = target_ma - current_ma;
  int32_t proportional = ((int32_t)cfg->kp_permille_per_amp * error_ma) /
                         1000;
  int64_t increment = ((int64_t)cfg->ki_permille_per_amp_second * error_ma *
                       VALVE_Q20_ONE) /
                      (1000LL * VALVE_CONTROL_FREQUENCY_HZ);
  int64_t candidate_integrator = *integrator_q20 + increment;
  int64_t candidate_output_q20 =
      ((int64_t)proportional * VALVE_Q20_ONE) + candidate_integrator;
  int64_t maximum_q20 = (int64_t)cfg->max_duty_permille * VALVE_Q20_ONE;
  int32_t output;

  if (!((candidate_output_q20 > maximum_q20 && error_ma > 0) ||
        (candidate_output_q20 < 0 && error_ma < 0)))
  {
    *integrator_q20 = candidate_integrator;
  }
  if (*integrator_q20 < 0) { *integrator_q20 = 0; }
  if (*integrator_q20 > maximum_q20) { *integrator_q20 = maximum_q20; }

  output = proportional + (int32_t)(*integrator_q20 / VALVE_Q20_ONE);
  if (output > cfg->max_duty_permille)
  {
    output = cfg->max_duty_permille;
    *limited = 1U;
  }
  if (output < 0) { output = 0; }
  return (uint16_t)output;
}

static void ValveControl_LatchFault(ValveControl *control, uint32_t fault)
{
  control->fault_flags |= fault;
  ValveControl_ForceSafe(control);
}

static void ValveControl_CheckFaults(ValveControl *control,
                                     uint16_t forward_raw,
                                     uint16_t reverse_raw)
{
  uint16_t active_current = 0U;
  uint16_t inactive_current = 0U;
  uint16_t active_duty = 0U;
  uint16_t active_limit = 0U;
  uint16_t target = (uint16_t)((control->applied_target_q16 + 32768) >> 16);

  if ((control->forward_current_ma > VALVE_OVERCURRENT_FAST_MA) ||
      (control->reverse_current_ma > VALVE_OVERCURRENT_FAST_MA))
  {
    ValveControl_LatchFault(control, SAFETY_STATUS_VALVE_OVERCURRENT);
    return;
  }
  if ((control->forward_current_ma > VALVE_OVERCURRENT_SUSTAINED_MA) ||
      (control->reverse_current_ma > VALVE_OVERCURRENT_SUSTAINED_MA))
  {
    if (control->overcurrent_samples < UINT16_MAX)
    {
      ++control->overcurrent_samples;
    }
    if (control->overcurrent_samples >= VALVE_OVERCURRENT_SAMPLES)
    {
      ValveControl_LatchFault(control, SAFETY_STATUS_VALVE_OVERCURRENT);
      return;
    }
  }
  else
  {
    control->overcurrent_samples = 0U;
  }
  if ((forward_raw >= 4080U) || (reverse_raw >= 4080U))
  {
    ValveControl_LatchFault(control, SAFETY_STATUS_VALVE_SENSOR_FAULT);
    return;
  }

  if ((control->state == SAFETY_VALVE_STATE_FORWARD) ||
      (control->state == SAFETY_VALVE_STATE_REVERSING_TO_REVERSE))
  {
    active_current = control->forward_current_ma;
    inactive_current = control->reverse_current_ma;
    active_duty = control->forward_duty_permille;
    active_limit = control->config.forward.max_duty_permille;
  }
  else if ((control->state == SAFETY_VALVE_STATE_REVERSE) ||
           (control->state == SAFETY_VALVE_STATE_REVERSING_TO_FORWARD))
  {
    active_current = control->reverse_current_ma;
    inactive_current = control->forward_current_ma;
    active_duty = control->reverse_duty_permille;
    active_limit = control->config.reverse.max_duty_permille;
  }

  if (inactive_current > VALVE_INACTIVE_CURRENT_MA)
  {
    if (control->inactive_current_samples < UINT16_MAX)
    {
      ++control->inactive_current_samples;
    }
    if (control->inactive_current_samples >= VALVE_INACTIVE_CURRENT_SAMPLES)
    {
      ValveControl_LatchFault(control, SAFETY_STATUS_VALVE_SENSOR_FAULT);
      return;
    }
  }
  else
  {
    control->inactive_current_samples = 0U;
  }

  if ((control->forward_duty_permille == 0U) &&
      (control->reverse_duty_permille == 0U) &&
      ((control->forward_current_ma > VALVE_OUTPUT_OFF_CURRENT_MA) ||
       (control->reverse_current_ma > VALVE_OUTPUT_OFF_CURRENT_MA)))
  {
    if (control->output_off_current_samples < UINT16_MAX)
    {
      ++control->output_off_current_samples;
    }
    if (control->output_off_current_samples >= VALVE_OUTPUT_OFF_SAMPLES)
    {
      ValveControl_LatchFault(control, SAFETY_STATUS_VALVE_SENSOR_FAULT);
      return;
    }
  }
  else
  {
    control->output_off_current_samples = 0U;
  }

  if ((target >= VALVE_OPEN_LOAD_TARGET_MA) &&
      (active_duty >= active_limit) &&
      (active_current < VALVE_OPEN_LOAD_CURRENT_MA))
  {
    if (control->open_load_samples < UINT16_MAX)
    {
      ++control->open_load_samples;
    }
    if (control->open_load_samples >= VALVE_OPEN_LOAD_SAMPLES)
    {
      ValveControl_LatchFault(control, SAFETY_STATUS_VALVE_OPEN_LOAD);
    }
  }
  else
  {
    control->open_load_samples = 0U;
  }
}

static void ValveControl_AdvanceState(ValveControl *control)
{
  int16_t request = control->requested_target_ma;
  uint16_t request_abs = (uint16_t)((request < 0) ? -request : request);
  uint16_t active_current = 0U;
  const SAFETY_ValveChannelConfig *cfg = &control->config.forward;
  bool reversing = false;

  if (control->state == SAFETY_VALVE_STATE_OFF)
  {
    control->applied_target_q16 = 0;
    if (request > 0) { control->state = SAFETY_VALVE_STATE_FORWARD; }
    else if (request < 0) { control->state = SAFETY_VALVE_STATE_REVERSE; }
  }
  if ((control->state == SAFETY_VALVE_STATE_FORWARD) && (request < 0))
  {
    control->state = SAFETY_VALVE_STATE_REVERSING_TO_REVERSE;
    control->low_current_samples = 0U;
    control->direction_transition_samples = 0U;
  }
  else if ((control->state == SAFETY_VALVE_STATE_REVERSE) && (request > 0))
  {
    control->state = SAFETY_VALVE_STATE_REVERSING_TO_FORWARD;
    control->low_current_samples = 0U;
    control->direction_transition_samples = 0U;
  }

  if ((control->state == SAFETY_VALVE_STATE_FORWARD) ||
      (control->state == SAFETY_VALVE_STATE_REVERSING_TO_REVERSE))
  {
    cfg = &control->config.forward;
    active_current = control->forward_current_ma;
  }
  else if ((control->state == SAFETY_VALVE_STATE_REVERSE) ||
           (control->state == SAFETY_VALVE_STATE_REVERSING_TO_FORWARD))
  {
    cfg = &control->config.reverse;
    active_current = control->reverse_current_ma;
  }

  reversing = (control->state == SAFETY_VALVE_STATE_REVERSING_TO_FORWARD) ||
              (control->state == SAFETY_VALVE_STATE_REVERSING_TO_REVERSE);
  control->applied_target_q16 = ValveControl_RampTarget(
      control->applied_target_q16, reversing ? 0U : request_abs, cfg);

  if (reversing && (control->applied_target_q16 == 0))
  {
    if (control->direction_transition_samples < UINT16_MAX)
    {
      ++control->direction_transition_samples;
    }
    if (active_current < SAFETY_VALVE_TARGET_DEADBAND_MA)
    {
      if (control->low_current_samples < UINT16_MAX)
      {
        ++control->low_current_samples;
      }
    }
    else
    {
      control->low_current_samples = 0U;
    }
    if (control->low_current_samples >= VALVE_DIRECTION_ZERO_SAMPLES)
    {
      if (control->deadtime_samples < VALVE_DIRECTION_DEADTIME_SAMPLES)
      {
        ++control->deadtime_samples;
      }
      else
      {
        control->forward_integrator_q20 = 0;
        control->reverse_integrator_q20 = 0;
        control->low_current_samples = 0U;
        control->deadtime_samples = 0U;
        control->direction_transition_samples = 0U;
        control->state = (request < 0) ? SAFETY_VALVE_STATE_REVERSE :
                         (request > 0) ? SAFETY_VALVE_STATE_FORWARD :
                                         SAFETY_VALVE_STATE_OFF;
      }
    }
    if (control->direction_transition_samples >=
        VALVE_DIRECTION_TIMEOUT_SAMPLES)
    {
      ValveControl_LatchFault(control,
                              SAFETY_STATUS_VALVE_DIRECTION_FAULT);
    }
  }
  else if (!reversing && (request == 0) &&
           (control->applied_target_q16 == 0) &&
           (active_current < SAFETY_VALVE_TARGET_DEADBAND_MA))
  {
    control->state = SAFETY_VALVE_STATE_OFF;
    control->forward_integrator_q20 = 0;
    control->reverse_integrator_q20 = 0;
  }
}

void ValveControl_Step(ValveControl *control, uint16_t forward_raw,
                       uint16_t reverse_raw, uint16_t vdda_mv,
                       bool outputs_allowed)
{
  int32_t target_ma;
  int32_t filtered_ma;

  if (control == NULL) { return; }
  ++control->loop_sequence;
  if ((vdda_mv < 2800U) || (vdda_mv > 3600U)) { vdda_mv = 3300U; }

  if (control->calibrated == 0U)
  {
    control->forward_zero_accumulator += forward_raw;
    control->reverse_zero_accumulator += reverse_raw;
    ++control->calibration_samples;
    control->forward_duty_permille = 0U;
    control->reverse_duty_permille = 0U;
    if (control->calibration_samples >= VALVE_CALIBRATION_SAMPLES)
    {
      control->forward_zero_raw = (uint16_t)(
          control->forward_zero_accumulator / VALVE_CALIBRATION_SAMPLES);
      control->reverse_zero_raw = (uint16_t)(
          control->reverse_zero_accumulator / VALVE_CALIBRATION_SAMPLES);
      if ((((uint32_t)control->forward_zero_raw * vdda_mv) / 4095U >
           VALVE_CURRENT_ZERO_LIMIT_MA) ||
          (((uint32_t)control->reverse_zero_raw * vdda_mv) / 4095U >
           VALVE_CURRENT_ZERO_LIMIT_MA))
      {
        ValveControl_LatchFault(control,
                                SAFETY_STATUS_VALVE_SENSOR_FAULT);
      }
      else
      {
        control->calibrated = 1U;
      }
    }
    return;
  }

  control->forward_current_ma = ValveControl_ConvertCurrent(
      forward_raw, control->forward_zero_raw, vdda_mv,
      &control->config.forward);
  control->reverse_current_ma = ValveControl_ConvertCurrent(
      reverse_raw, control->reverse_zero_raw, vdda_mv,
      &control->config.reverse);
  control->forward_filtered_q16 += (int32_t)(
      ((int64_t)control->forward_alpha_q15 *
       (((int32_t)control->forward_current_ma << 16) -
        control->forward_filtered_q16)) >> 15);
  control->reverse_filtered_q16 += (int32_t)(
      ((int64_t)control->reverse_alpha_q15 *
       (((int32_t)control->reverse_current_ma << 16) -
        control->reverse_filtered_q16)) >> 15);
  control->forward_current_ma = (uint16_t)ValveControl_Clamp32(
      (control->forward_filtered_q16 + 32768) >> 16, 0, 4000);
  control->reverse_current_ma = (uint16_t)ValveControl_Clamp32(
      (control->reverse_filtered_q16 + 32768) >> 16, 0, 4000);

  if (!outputs_allowed || (control->fault_flags != 0U))
  {
    ValveControl_ForceSafe(control);
    return;
  }

  control->limited = 0U;
  ValveControl_AdvanceState(control);
  target_ma = (control->applied_target_q16 + 32768) >> 16;
  control->forward_duty_permille = 0U;
  control->reverse_duty_permille = 0U;
  if (control->state == SAFETY_VALVE_STATE_FORWARD)
  {
    filtered_ma = control->forward_current_ma;
    control->forward_duty_permille = ValveControl_RunPi(
        target_ma, filtered_ma, &control->config.forward,
        &control->forward_integrator_q20, &control->limited);
  }
  else if (control->state == SAFETY_VALVE_STATE_REVERSE)
  {
    filtered_ma = control->reverse_current_ma;
    control->reverse_duty_permille = ValveControl_RunPi(
        target_ma, filtered_ma, &control->config.reverse,
        &control->reverse_integrator_q20, &control->limited);
  }
  ValveControl_CheckFaults(control, forward_raw, reverse_raw);
}

int32_t ValveControl_ApplyConfig(ValveControl *control,
                                 const SAFETY_ValveConfig *config)
{
  int32_t target_ma;
  int32_t current_ma;
  int32_t proportional;
  int64_t output_q20;

  if ((control == NULL) || !ValveControl_ValidateConfig(config))
  {
    return SAFETY_RESULT_RANGE;
  }
  target_ma = (control->applied_target_q16 + 32768) >> 16;
  if (control->state == SAFETY_VALVE_STATE_FORWARD)
  {
    current_ma = control->forward_current_ma;
    proportional = ((int32_t)config->forward.kp_permille_per_amp *
                    (target_ma - current_ma)) / 1000;
    output_q20 = (int64_t)control->forward_duty_permille * VALVE_Q20_ONE;
    control->forward_integrator_q20 = output_q20 -
        ((int64_t)proportional * VALVE_Q20_ONE);
  }
  else if (control->state == SAFETY_VALVE_STATE_REVERSE)
  {
    current_ma = control->reverse_current_ma;
    proportional = ((int32_t)config->reverse.kp_permille_per_amp *
                    (target_ma - current_ma)) / 1000;
    output_q20 = (int64_t)control->reverse_duty_permille * VALVE_Q20_ONE;
    control->reverse_integrator_q20 = output_q20 -
        ((int64_t)proportional * VALVE_Q20_ONE);
  }
  control->config = *config;
  control->forward_alpha_q15 =
      ValveControl_FilterAlpha(config->forward.filter_cutoff_hz);
  control->reverse_alpha_q15 =
      ValveControl_FilterAlpha(config->reverse.filter_cutoff_hz);
  return SAFETY_RESULT_OK;
}

uint16_t ValveControl_ForwardCompare(const ValveControl *control)
{
  return (control == NULL) ? 0U : (uint16_t)(
      ((uint32_t)control->forward_duty_permille * SAFETY_PWM_PERIOD_COUNTS) /
      1000U);
}

uint16_t ValveControl_ReverseCompare(const ValveControl *control)
{
  return (control == NULL) ? 0U : (uint16_t)(
      ((uint32_t)control->reverse_duty_permille * SAFETY_PWM_PERIOD_COUNTS) /
      1000U);
}

int16_t ValveControl_AppliedTargetMa(const ValveControl *control)
{
  int16_t magnitude;
  if (control == NULL) { return 0; }
  magnitude = (int16_t)((control->applied_target_q16 + 32768) >> 16);
  if ((control->state == SAFETY_VALVE_STATE_REVERSE) ||
      (control->state == SAFETY_VALVE_STATE_REVERSING_TO_FORWARD))
  {
    magnitude = (int16_t)-magnitude;
  }
  return magnitude;
}

bool ValveControl_IsIdle(const ValveControl *control)
{
  return (control != NULL) &&
         (control->requested_target_ma == 0) &&
         (control->applied_target_q16 == 0) &&
         (control->forward_duty_permille == 0U) &&
         (control->reverse_duty_permille == 0U) &&
         (control->forward_current_ma < SAFETY_VALVE_TARGET_DEADBAND_MA) &&
         (control->reverse_current_ma < SAFETY_VALVE_TARGET_DEADBAND_MA);
}
