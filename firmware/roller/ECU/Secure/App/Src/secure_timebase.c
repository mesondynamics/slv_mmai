#include "secure_timebase.h"

#include "tim.h"

#define SECURE_TIMEBASE_PRESCALER  249UL
#define SECURE_TIMEBASE_PERIOD     65535UL
#define SECURE_TIMEBASE_PCLK1_HZ   125000000UL
#define SECURE_TIMEBASE_SYSCLK_HZ  250000000UL

static uint8_t secure_timebase_running;

bool SecureTimebase_Init(void)
{
  secure_timebase_running = 0U;

  /* APB1 is 125 MHz and its timer multiplier produces a 250 MHz TIM7 clock.
   * The CubeMX-owned prescaler therefore yields a 1 MHz counter.  Fail closed
   * if generated configuration and the reviewed timing contract diverge. */
  if ((htim7.Instance != TIM7) ||
      (htim7.Init.Prescaler != SECURE_TIMEBASE_PRESCALER) ||
      (htim7.Init.CounterMode != TIM_COUNTERMODE_UP) ||
      (htim7.Init.Period != SECURE_TIMEBASE_PERIOD) ||
      (HAL_RCC_GetPCLK1Freq() != SECURE_TIMEBASE_PCLK1_HZ) ||
      (SystemCoreClock != SECURE_TIMEBASE_SYSCLK_HZ))
  {
    return false;
  }

  __HAL_TIM_SET_COUNTER(&htim7, 0U);
  if (HAL_TIM_Base_Start(&htim7) != HAL_OK)
  {
    return false;
  }
  if ((TIM7->CR1 & TIM_CR1_CEN) == 0U)
  {
    return false;
  }

  secure_timebase_running = 1U;
  __DMB();
  return true;
}

bool SecureTimebase_IsRunning(void)
{
  return (secure_timebase_running != 0U) &&
         ((TIM7->CR1 & TIM_CR1_CEN) != 0U);
}

uint16_t SecureTimebase_NowUs16(void)
{
  return (uint16_t)TIM7->CNT;
}

bool SecureTimebase_HasElapsed(uint16_t start, uint32_t interval_us,
                               bool *elapsed)
{
  if ((elapsed == NULL) ||
      (interval_us > SECURE_TIMEBASE_MAX_INTERVAL_US) ||
      !SecureTimebase_IsRunning())
  {
    return false;
  }
  *elapsed = (uint16_t)(SecureTimebase_NowUs16() - start) >=
             (uint16_t)interval_us;
  return true;
}

bool SecureTimebase_DelayUs(uint32_t interval_us)
{
  bool elapsed;
  uint16_t start;

  if (interval_us == 0U)
  {
    return true;
  }
  if ((interval_us > SECURE_TIMEBASE_MAX_INTERVAL_US) ||
      !SecureTimebase_IsRunning())
  {
    return false;
  }

  start = SecureTimebase_NowUs16();
  do
  {
    if (!SecureTimebase_HasElapsed(start, interval_us, &elapsed))
    {
      return false;
    }
  } while (!elapsed);
  return true;
}
