#include "safety_service.h"

#include <stddef.h>

#include "adc.h"
#include "iwdg.h"
#include "main.h"
#include "tim.h"

static volatile uint16_t slow_adc_dma[SAFETY_SLOW_ADC_COUNT];
static volatile uint16_t current_adc_dma[SAFETY_CURRENT_ADC_COUNT];
static volatile uint16_t slow_adc_snapshot[SAFETY_SLOW_ADC_COUNT];
static volatile uint16_t current_adc_snapshot[SAFETY_CURRENT_ADC_COUNT];
static volatile uint32_t slow_sequence;
static volatile uint32_t current_sequence;
static volatile uint32_t safety_status;
static uint32_t last_command_sequence;
static uint32_t last_watchdog_heartbeat = UINT32_MAX;
static uint8_t command_sequence_valid;
static uint8_t timer_ready;

static uint32_t Safety_EnterCritical(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  __DMB();
  return primask;
}

static void Safety_ExitCritical(uint32_t primask)
{
  __DMB();
  if (primask == 0U)
  {
    __enable_irq();
  }
}

static void Safety_ForceOutputsSafe(void)
{
  if (timer_ready != 0U)
  {
    TIM4->CCR1 = 0U;
    TIM4->CCR2 = 0U;
  }

  /* OE_N high disables the TPIC outputs. CLR_N and buffer enable low are the
     board-level fail-safe states selected from the ECU schematic. */
  GPIOE->BSRR = TPIC_OE_N_Pin;
  GPIOE->BSRR = ((uint32_t)(TPIC_CLR_N_Pin | TPIC_CTRL_BUF_EN_Pin | TPIC_RCK_Pin) << 16U);
  safety_status &= ~SAFETY_STATUS_OUTPUTS_ARMED;
}

static void Safety_LatchFault(uint32_t reason)
{
  uint32_t primask = Safety_EnterCritical();
  safety_status |= SAFETY_STATUS_FAULT_LATCHED | reason;
  Safety_ForceOutputsSafe();
  Safety_ExitCritical(primask);
}

int32_t Safety_ServiceInit(void)
{
  uint32_t primask;

  timer_ready = 1U;
  primask = Safety_EnterCritical();
  Safety_ForceOutputsSafe();
  safety_status = 0U;
  if (HAL_GPIO_ReadPin(ESTOP_DETECT_GPIO_Port, ESTOP_DETECT_Pin) == GPIO_PIN_SET)
  {
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED;
  }
  Safety_ExitCritical(primask);

  if ((HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK) ||
      (HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED) != HAL_OK))
  {
    Safety_LatchFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }

  if ((HAL_ADC_Start_DMA(&hadc1, (uint32_t *)(uintptr_t)slow_adc_dma,
                         SAFETY_SLOW_ADC_COUNT) != HAL_OK) ||
      (HAL_ADC_Start_DMA(&hadc2, (uint32_t *)(uintptr_t)current_adc_dma,
                         SAFETY_CURRENT_ADC_COUNT) != HAL_OK))
  {
    Safety_LatchFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }

  /* CH4 is internal-only and produces the ADC2 trigger at the PWM midpoint. */
  if ((HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1) != HAL_OK) ||
      (HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_2) != HAL_OK) ||
      (HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_4) != HAL_OK) ||
      (HAL_TIM_Base_Start(&htim6) != HAL_OK))
  {
    Safety_LatchFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }

  safety_status |= SAFETY_STATUS_READY | SAFETY_STATUS_ADC_RUNNING;
  if (HAL_IWDG_Refresh(&hiwdg) != HAL_OK)
  {
    Safety_LatchFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }

  return SAFETY_RESULT_OK;
}

uint32_t Safety_GetStatus(void)
{
  uint32_t primask = Safety_EnterCritical();
  uint32_t status;

  if (HAL_GPIO_ReadPin(ESTOP_DETECT_GPIO_Port, ESTOP_DETECT_Pin) == GPIO_PIN_SET)
  {
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED;
    Safety_ForceOutputsSafe();
  }
  else
  {
    safety_status &= ~SAFETY_STATUS_ESTOP_ACTIVE;
  }
  status = safety_status;
  Safety_ExitCritical(primask);
  return status;
}

int32_t Safety_GetAdcSnapshot(SAFETY_AdcSnapshot *snapshot)
{
  uint32_t index;
  uint32_t primask;

  if (snapshot == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }

  primask = Safety_EnterCritical();
  snapshot->status = safety_status;
  snapshot->slow_sequence = slow_sequence;
  snapshot->current_sequence = current_sequence;
  for (index = 0U; index < SAFETY_SLOW_ADC_COUNT; ++index)
  {
    snapshot->slow_adc[index] = slow_adc_snapshot[index];
  }
  for (index = 0U; index < SAFETY_CURRENT_ADC_COUNT; ++index)
  {
    snapshot->current_adc[index] = current_adc_snapshot[index];
  }
  snapshot->reserved = 0U;
  Safety_ExitCritical(primask);

  return SAFETY_RESULT_OK;
}

int32_t Safety_ClearFault(uint32_t request_token)
{
  uint32_t primask;

  if (request_token != SAFETY_CLEAR_FAULT_TOKEN)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  primask = Safety_EnterCritical();
  if (HAL_GPIO_ReadPin(ESTOP_DETECT_GPIO_Port, ESTOP_DETECT_Pin) == GPIO_PIN_SET)
  {
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED;
    Safety_ForceOutputsSafe();
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_ESTOP_ACTIVE;
  }
  if ((safety_status & (SAFETY_STATUS_GTZC_VIOLATION |
                       SAFETY_STATUS_INTERNAL_ERROR)) != 0U)
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  Safety_ForceOutputsSafe();
  safety_status &= ~(SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED);
  command_sequence_valid = 0U;
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

int32_t Safety_ArmOutputs(uint32_t request_token)
{
  uint32_t primask;

  if (request_token != SAFETY_ARM_TOKEN)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  primask = Safety_EnterCritical();
  if ((safety_status & SAFETY_STATUS_READY) == 0U)
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_NOT_READY;
  }
  if (HAL_GPIO_ReadPin(ESTOP_DETECT_GPIO_Port, ESTOP_DETECT_Pin) == GPIO_PIN_SET)
  {
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED;
    Safety_ForceOutputsSafe();
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_ESTOP_ACTIVE;
  }
  if ((safety_status & SAFETY_STATUS_FAULT_LATCHED) != 0U)
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_FAULT_LATCHED;
  }
  TIM4->CCR1 = 0U;
  TIM4->CCR2 = 0U;
  GPIOE->BSRR = TPIC_OE_N_Pin;
  GPIOE->BSRR = ((uint32_t)(TPIC_CTRL_BUF_EN_Pin | TPIC_CLR_N_Pin | TPIC_RCK_Pin) << 16U);
  __DSB();
  /* Latch a cleared shift register before exposing any downstream outputs. */
  GPIOE->BSRR = TPIC_RCK_Pin;
  GPIOE->BSRR = ((uint32_t)TPIC_RCK_Pin << 16U);
  GPIOE->BSRR = TPIC_CLR_N_Pin | TPIC_CTRL_BUF_EN_Pin;
  GPIOE->BSRR = ((uint32_t)TPIC_OE_N_Pin << 16U);
  safety_status |= SAFETY_STATUS_OUTPUTS_ARMED;
  command_sequence_valid = 0U;
  Safety_ExitCritical(primask);

  return SAFETY_RESULT_OK;
}

int32_t Safety_DisarmOutputs(void)
{
  uint32_t primask = Safety_EnterCritical();
  Safety_ForceOutputsSafe();
  command_sequence_valid = 0U;
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

int32_t Safety_SetPwm(uint16_t forward_compare,
                      uint16_t reverse_compare,
                      uint32_t command_sequence)
{
  uint32_t primask;

  if ((forward_compare > SAFETY_PWM_MAX_COMPARE) ||
      (reverse_compare > SAFETY_PWM_MAX_COMPARE))
  {
    return SAFETY_RESULT_RANGE;
  }
  if ((forward_compare != 0U) && (reverse_compare != 0U))
  {
    return SAFETY_RESULT_DIRECTION;
  }
  primask = Safety_EnterCritical();
  if (HAL_GPIO_ReadPin(ESTOP_DETECT_GPIO_Port, ESTOP_DETECT_Pin) == GPIO_PIN_SET)
  {
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED;
    Safety_ForceOutputsSafe();
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_ESTOP_ACTIVE;
  }
  if ((safety_status & SAFETY_STATUS_FAULT_LATCHED) != 0U)
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_FAULT_LATCHED;
  }
  if ((safety_status & SAFETY_STATUS_OUTPUTS_ARMED) == 0U)
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_NOT_ARMED;
  }
  if ((command_sequence_valid != 0U) &&
      ((int32_t)(command_sequence - last_command_sequence) <= 0))
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_STALE_SEQUENCE;
  }
  TIM4->CCR1 = forward_compare;
  TIM4->CCR2 = reverse_compare;
  last_command_sequence = command_sequence;
  command_sequence_valid = 1U;
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

int32_t Safety_KickWatchdog(uint32_t heartbeat)
{
  if ((safety_status & SAFETY_STATUS_READY) == 0U)
  {
    return SAFETY_RESULT_NOT_READY;
  }
  if (heartbeat == last_watchdog_heartbeat)
  {
    return SAFETY_RESULT_STALE_SEQUENCE;
  }
  if (HAL_IWDG_Refresh(&hiwdg) != HAL_OK)
  {
    Safety_LatchFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  last_watchdog_heartbeat = heartbeat;
  return SAFETY_RESULT_OK;
}

void Safety_FaultFromException(void)
{
  Safety_LatchFault(SAFETY_STATUS_INTERNAL_ERROR);
}

void HAL_GPIO_EXTI_Rising_Callback(uint16_t gpio_pin)
{
  if (gpio_pin == ESTOP_DETECT_Pin)
  {
    Safety_LatchFault(SAFETY_STATUS_ESTOP_ACTIVE);
  }
}

void HAL_GPIO_EXTI_Falling_Callback(uint16_t gpio_pin)
{
  if (gpio_pin == ESTOP_DETECT_Pin)
  {
    uint32_t primask = Safety_EnterCritical();
    safety_status &= ~SAFETY_STATUS_ESTOP_ACTIVE;
    Safety_ExitCritical(primask);
  }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  uint32_t index;

  if (hadc->Instance == ADC1)
  {
    for (index = 0U; index < SAFETY_SLOW_ADC_COUNT; ++index)
    {
      slow_adc_snapshot[index] = slow_adc_dma[index];
    }
    __DMB();
    ++slow_sequence;
  }
  else if (hadc->Instance == ADC2)
  {
    for (index = 0U; index < SAFETY_CURRENT_ADC_COUNT; ++index)
    {
      current_adc_snapshot[index] = current_adc_dma[index];
    }
    __DMB();
    ++current_sequence;
  }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  (void)hadc;
  Safety_LatchFault(SAFETY_STATUS_INTERNAL_ERROR);
}

void HAL_GTZC_TZIC_Callback(uint32_t periph_id)
{
  (void)periph_id;
  Safety_LatchFault(SAFETY_STATUS_GTZC_VIOLATION);
}
