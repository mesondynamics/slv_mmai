#include "software_i2c.h"

#include "main.h"

#define SOFTWARE_I2C_HALF_PERIOD_US       5UL
#define SOFTWARE_I2C_STRETCH_TIMEOUT_US   1000UL

static uint32_t cycles_per_us;

static void SoftwareI2C_DelayUs(uint32_t microseconds)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t cycles = microseconds * cycles_per_us;

  while ((uint32_t)(DWT->CYCCNT - start) < cycles)
  {
  }
}

static void SoftwareI2C_SdaRelease(void)
{
  SW_I2C_SDA_GPIO_Port->BSRR = SW_I2C_SDA_Pin;
}

static void SoftwareI2C_SdaLow(void)
{
  SW_I2C_SDA_GPIO_Port->BSRR = (uint32_t)SW_I2C_SDA_Pin << 16U;
}

static void SoftwareI2C_SclRelease(void)
{
  SW_I2C_SCL_GPIO_Port->BSRR = SW_I2C_SCL_Pin;
}

static void SoftwareI2C_SclLow(void)
{
  SW_I2C_SCL_GPIO_Port->BSRR = (uint32_t)SW_I2C_SCL_Pin << 16U;
}

static bool SoftwareI2C_SdaIsHigh(void)
{
  return (SW_I2C_SDA_GPIO_Port->IDR & SW_I2C_SDA_Pin) != 0U;
}

static bool SoftwareI2C_WaitSclHigh(void)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t timeout_cycles = SOFTWARE_I2C_STRETCH_TIMEOUT_US * cycles_per_us;

  SoftwareI2C_SclRelease();
  while ((SW_I2C_SCL_GPIO_Port->IDR & SW_I2C_SCL_Pin) == 0U)
  {
    if ((uint32_t)(DWT->CYCCNT - start) >= timeout_cycles)
    {
      return false;
    }
  }
  return true;
}

static bool SoftwareI2C_Start(void)
{
  SoftwareI2C_SdaRelease();
  if (!SoftwareI2C_WaitSclHigh())
  {
    return false;
  }
  SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
  if (!SoftwareI2C_SdaIsHigh())
  {
    return false;
  }
  SoftwareI2C_SdaLow();
  SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
  SoftwareI2C_SclLow();
  return true;
}

static bool SoftwareI2C_Stop(void)
{
  SoftwareI2C_SdaLow();
  SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
  if (!SoftwareI2C_WaitSclHigh())
  {
    SoftwareI2C_SdaRelease();
    return false;
  }
  SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
  SoftwareI2C_SdaRelease();
  SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
  return SoftwareI2C_SdaIsHigh();
}

static bool SoftwareI2C_WriteByte(uint8_t value)
{
  uint32_t bit;

  for (bit = 0U; bit < 8U; ++bit)
  {
    if ((value & 0x80U) != 0U)
    {
      SoftwareI2C_SdaRelease();
    }
    else
    {
      SoftwareI2C_SdaLow();
    }
    SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
    if (!SoftwareI2C_WaitSclHigh())
    {
      return false;
    }
    SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
    SoftwareI2C_SclLow();
    value <<= 1U;
  }

  SoftwareI2C_SdaRelease();
  SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
  if (!SoftwareI2C_WaitSclHigh())
  {
    return false;
  }
  bit = SoftwareI2C_SdaIsHigh() ? 1U : 0U;
  SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
  SoftwareI2C_SclLow();
  return bit == 0U;
}

static bool SoftwareI2C_ReadByte(uint8_t *value, bool acknowledge)
{
  uint32_t bit;
  uint8_t result = 0U;

  SoftwareI2C_SdaRelease();
  for (bit = 0U; bit < 8U; ++bit)
  {
    result <<= 1U;
    SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
    if (!SoftwareI2C_WaitSclHigh())
    {
      return false;
    }
    if (SoftwareI2C_SdaIsHigh())
    {
      result |= 1U;
    }
    SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
    SoftwareI2C_SclLow();
  }

  if (acknowledge)
  {
    SoftwareI2C_SdaLow();
  }
  else
  {
    SoftwareI2C_SdaRelease();
  }
  SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
  if (!SoftwareI2C_WaitSclHigh())
  {
    return false;
  }
  SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
  SoftwareI2C_SclLow();
  SoftwareI2C_SdaRelease();
  *value = result;
  return true;
}

bool SoftwareI2C_RecoverBus(void)
{
  uint32_t pulse;

  SoftwareI2C_SdaRelease();
  if (!SoftwareI2C_WaitSclHigh())
  {
    return false;
  }

  if (!SoftwareI2C_SdaIsHigh())
  {
    for (pulse = 0U; pulse < 9U; ++pulse)
    {
      SoftwareI2C_SclLow();
      SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
      if (!SoftwareI2C_WaitSclHigh())
      {
        return false;
      }
      SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US);
      if (SoftwareI2C_SdaIsHigh())
      {
        break;
      }
    }
  }

  if (!SoftwareI2C_Stop())
  {
    return false;
  }
  return SoftwareI2C_SdaIsHigh() &&
         ((SW_I2C_SCL_GPIO_Port->IDR & SW_I2C_SCL_Pin) != 0U);
}

bool SoftwareI2C_Init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  cycles_per_us = SystemCoreClock / 1000000UL;
  if (cycles_per_us == 0U)
  {
    cycles_per_us = 1U;
  }
  return SoftwareI2C_RecoverBus();
}

bool SoftwareI2C_Write(uint8_t address_7bit, const uint8_t *data, size_t length)
{
  size_t index;
  bool success;

  if (((data == NULL) && (length != 0U)) || (address_7bit > 0x7FU))
  {
    return false;
  }
  if (!SoftwareI2C_Start())
  {
    (void)SoftwareI2C_RecoverBus();
    return false;
  }

  success = SoftwareI2C_WriteByte((uint8_t)(address_7bit << 1U));
  for (index = 0U; success && (index < length); ++index)
  {
    success = SoftwareI2C_WriteByte(data[index]);
  }
  return SoftwareI2C_Stop() && success;
}

bool SoftwareI2C_Read(uint8_t address_7bit, uint8_t *data, size_t length)
{
  size_t index;
  bool success;

  if ((data == NULL) || (length == 0U) || (address_7bit > 0x7FU))
  {
    return false;
  }
  if (!SoftwareI2C_Start())
  {
    (void)SoftwareI2C_RecoverBus();
    return false;
  }

  success = SoftwareI2C_WriteByte((uint8_t)((address_7bit << 1U) | 1U));
  for (index = 0U; success && (index < length); ++index)
  {
    success = SoftwareI2C_ReadByte(&data[index], index + 1U < length);
  }
  return SoftwareI2C_Stop() && success;
}
