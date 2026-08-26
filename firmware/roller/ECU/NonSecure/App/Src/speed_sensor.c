#include "speed_sensor.h"

#include <limits.h>

#include "main.h"
#include "tim.h"

static volatile uint32_t last_capture;
static volatile uint32_t period_ticks;
static volatile uint32_t capture_tick_ms;
static volatile uint32_t capture_sequence;
static volatile uint8_t first_capture_seen;
static uint32_t timer_tick_hz;
static uint32_t filtered_frequency_centi_hz;
static uint32_t last_filtered_sequence;
static uint8_t filter_valid;

static uint16_t SpeedSensor_ClampU16(uint32_t value)
{
  return (value > UINT16_MAX) ? UINT16_MAX : (uint16_t)value;
}

bool SpeedSensor_Init(void)
{
  uint32_t timer_clock = HAL_RCC_GetPCLK1Freq();

  if (timer_clock != HAL_RCC_GetHCLKFreq())
  {
    timer_clock *= 2U;
  }
  timer_tick_hz = timer_clock / (htim2.Init.Prescaler + 1U);
  last_capture = 0U;
  period_ticks = 0U;
  capture_tick_ms = 0U;
  capture_sequence = 0U;
  first_capture_seen = 0U;
  filtered_frequency_centi_hz = 0U;
  last_filtered_sequence = 0U;
  filter_valid = 0U;
  __HAL_TIM_SET_COUNTER(&htim2, 0U);
  __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_CC1 | TIM_FLAG_UPDATE);
  return HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_1) == HAL_OK;
}

void SpeedSensor_GetMeasurement(SpeedSensorMeasurement *measurement)
{
  uint32_t primask;
  uint32_t period;
  uint32_t tick;
  uint32_t sequence;
  uint32_t raw_frequency;
  uint32_t speed;

  if (measurement == NULL)
  {
    return;
  }
  measurement->frequency_centi_hz = 0U;
  measurement->speed_centi_kph = 0U;
  measurement->signal_present = false;

  primask = __get_PRIMASK();
  __disable_irq();
  period = period_ticks;
  tick = capture_tick_ms;
  sequence = capture_sequence;
  if (primask == 0U) { __enable_irq(); }

  if ((period == 0U) || ((HAL_GetTick() - tick) > SPEED_SENSOR_TIMEOUT_MS))
  {
    filter_valid = 0U;
    return;
  }

  if (sequence != last_filtered_sequence)
  {
    raw_frequency = (uint32_t)(((uint64_t)timer_tick_hz * 100ULL +
                                (period / 2U)) / period);
    if (filter_valid == 0U)
    {
      filtered_frequency_centi_hz = raw_frequency;
      filter_valid = 1U;
    }
    else
    {
      /* Same alpha=0.2 first-order filter as the previous ECU. */
      filtered_frequency_centi_hz =
          ((filtered_frequency_centi_hz * 4U) + raw_frequency + 2U) / 5U;
    }
    last_filtered_sequence = sequence;
  }

  speed = (uint32_t)(((uint64_t)filtered_frequency_centi_hz * 3600ULL +
                      (SPEED_SENSOR_PULSES_PER_KILOMETER / 2U)) /
                     SPEED_SENSOR_PULSES_PER_KILOMETER);
  measurement->frequency_centi_hz =
      SpeedSensor_ClampU16(filtered_frequency_centi_hz);
  measurement->speed_centi_kph = SpeedSensor_ClampU16(speed);
  measurement->signal_present = true;
}

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
  uint32_t capture;

  if ((htim->Instance != TIM2) ||
      (htim->Channel != HAL_TIM_ACTIVE_CHANNEL_1))
  {
    return;
  }
  capture = HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1);
  if (first_capture_seen != 0U)
  {
    uint32_t delta = capture - last_capture;
    if (delta != 0U)
    {
      period_ticks = delta;
      capture_tick_ms = HAL_GetTick();
      ++capture_sequence;
    }
  }
  else
  {
    first_capture_seen = 1U;
    capture_tick_ms = HAL_GetTick();
  }
  last_capture = capture;
}
