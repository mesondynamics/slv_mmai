#include "software_i2c.h"

#include "main.h"
#include "secure_timebase.h"

#define SOFTWARE_I2C_HALF_PERIOD_US       5UL
#define SOFTWARE_I2C_STRETCH_TIMEOUT_US   1000UL
#define SOFTWARE_I2C_WAKE_LOW_US            80UL
#define SOFTWARE_I2C_WAKE_HIGH_US         2500UL

static bool SoftwareI2C_DelayUs(uint32_t microseconds)
{
  return SecureTimebase_DelayUs(microseconds);
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
  bool elapsed;
  uint16_t start = SecureTimebase_NowUs16();

  if (!SecureTimebase_IsRunning())
  {
    return false;
  }
  SoftwareI2C_SclRelease();
  while ((SW_I2C_SCL_GPIO_Port->IDR & SW_I2C_SCL_Pin) == 0U)
  {
    if (!SecureTimebase_HasElapsed(start, SOFTWARE_I2C_STRETCH_TIMEOUT_US,
                                   &elapsed) || elapsed)
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
  if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
  {
    return false;
  }
  if (!SoftwareI2C_SdaIsHigh())
  {
    return false;
  }
  SoftwareI2C_SdaLow();
  if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
  {
    return false;
  }
  SoftwareI2C_SclLow();
  return true;
}

static bool SoftwareI2C_Stop(void)
{
  SoftwareI2C_SdaLow();
  if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
  {
    return false;
  }
  if (!SoftwareI2C_WaitSclHigh())
  {
    SoftwareI2C_SdaRelease();
    return false;
  }
  if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
  {
    SoftwareI2C_SdaRelease();
    return false;
  }
  SoftwareI2C_SdaRelease();
  return SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US) &&
         SoftwareI2C_SdaIsHigh();
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
    if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
    {
      return false;
    }
    if (!SoftwareI2C_WaitSclHigh())
    {
      return false;
    }
    if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
    {
      return false;
    }
    SoftwareI2C_SclLow();
    value <<= 1U;
  }

  SoftwareI2C_SdaRelease();
  if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
  {
    return false;
  }
  if (!SoftwareI2C_WaitSclHigh())
  {
    return false;
  }
  bit = SoftwareI2C_SdaIsHigh() ? 1U : 0U;
  if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
  {
    return false;
  }
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
    if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
    {
      return false;
    }
    if (!SoftwareI2C_WaitSclHigh())
    {
      return false;
    }
    if (SoftwareI2C_SdaIsHigh())
    {
      result |= 1U;
    }
    if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
    {
      return false;
    }
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
  if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
  {
    return false;
  }
  if (!SoftwareI2C_WaitSclHigh())
  {
    return false;
  }
  if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
  {
    return false;
  }
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
      if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
      {
        return false;
      }
      if (!SoftwareI2C_WaitSclHigh())
      {
        return false;
      }
      if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
      {
        return false;
      }
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

bool SoftwareI2C_Synchronize(void)
{
  uint32_t pulse;

  /* A reset can leave the peer driving a partial response.  First recover an
     electrically stuck bus, then unconditionally reset the ATECC I/O channel
     exactly as specified for host-reset/noise recovery. */
  if (!SoftwareI2C_RecoverBus() || !SoftwareI2C_Start())
  {
    return false;
  }

  /* SoftwareI2C_Start() leaves SCL/SDA low.  Release SDA while SCL is low and
     clock nine complete bits with SDA held high by the pull-up. */
  SoftwareI2C_SdaRelease();
  for (pulse = 0U; pulse < 9U; ++pulse)
  {
    if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US) ||
        !SoftwareI2C_WaitSclHigh() ||
        !SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
    {
      return false;
    }
    SoftwareI2C_SclLow();
  }

  /* The second START and following STOP terminate any partial input/output
     group without depending on the ATECC's prior awake/busy state. */
  if (!SoftwareI2C_Start() || !SoftwareI2C_Stop())
  {
    return false;
  }
  return SoftwareI2C_SdaIsHigh() &&
         ((SW_I2C_SCL_GPIO_Port->IDR & SW_I2C_SCL_Pin) != 0U);
}

bool SoftwareI2C_Init(void)
{
  if (!SecureTimebase_IsRunning())
  {
    return false;
  }
  return SoftwareI2C_RecoverBus();
}

bool SoftwareI2C_WakeToken(void)
{
  SoftwareI2C_SdaRelease();
  if (!SoftwareI2C_WaitSclHigh())
  {
    return false;
  }
  if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_HALF_PERIOD_US))
  {
    return false;
  }
  if (!SoftwareI2C_SdaIsHigh())
  {
    return false;
  }
  /* CryptoAuthentication I2C wake token: SDA low for tWLO while SCL stays
     high, followed by tWHI before reading the four-byte wake response. */
  SoftwareI2C_SdaLow();
  if (!SoftwareI2C_DelayUs(SOFTWARE_I2C_WAKE_LOW_US))
  {
    SoftwareI2C_SdaRelease();
    return false;
  }
  SoftwareI2C_SdaRelease();
  return SoftwareI2C_DelayUs(SOFTWARE_I2C_WAKE_HIGH_US) &&
         SoftwareI2C_SdaIsHigh() &&
         ((SW_I2C_SCL_GPIO_Port->IDR & SW_I2C_SCL_Pin) != 0U);
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
