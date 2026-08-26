#!/usr/bin/env bash
set -euo pipefail

task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_failures=0

require_pattern() {
  local task_file="$1"
  local task_pattern="$2"
  local task_description="$3"
  if rg -q -- "${task_pattern}" "${task_project_dir}/${task_file}"; then
    printf 'PASS  %s\n' "${task_description}"
  else
    printf 'FAIL  %s (%s)\n' "${task_description}" "${task_file}" >&2
    task_failures=$((task_failures + 1))
  fi
}

reject_pattern() {
  local task_file="$1"
  local task_pattern="$2"
  local task_description="$3"
  if rg -q -- "${task_pattern}" "${task_project_dir}/${task_file}"; then
    printf 'FAIL  %s (%s)\n' "${task_description}" "${task_file}" >&2
    task_failures=$((task_failures + 1))
  else
    printf 'PASS  %s\n' "${task_description}"
  fi
}

require_pattern ECU.ioc '^PB2.GPIO_Label=ESTOP_DETECT$' 'E-stop is mapped to schematic pin PB2'
reject_pattern ECU.ioc '(^|,)(PG2|LPUART1|NUCLEO-H563ZI)(\:I)?(,|$)|^BSP_IP_NAME=' 'No Nucleo/LPUART/PG2 ghost configuration remains'
require_pattern ECU.ioc '^board=custom$' 'CubeMX project targets the custom ECU board'
require_pattern ECU.ioc '^ProjectManager.KeepUserCode=true$' 'CubeMX user-code preservation is enabled'
require_pattern ECU.ioc '^PB8.Signal=I2C1_SCL$' 'PB8 remains the MCU I2C1 SCL pin'
require_pattern ECU.ioc '^PB9.Signal=I2C1_SDA$' 'PB9 remains the MCU I2C1 SDA pin'
require_pattern ECU.ioc '^I2C1.I2C_Coeff_DF=2$' 'I2C1 digital filter is persisted in CubeMX'
require_pattern ECU.ioc '^GTZC_S.ADC_Secure=GTZC_TZSC_PERIPH_SEC$' 'ADC1/ADC2 are Secure'
require_pattern ECU.ioc '^GTZC_S.TIM4_Secure=GTZC_TZSC_PERIPH_SEC$' 'Valve PWM timer is Secure'
require_pattern ECU.ioc '^GTZC_S.IWDG_Secure=GTZC_TZSC_PERIPH_SEC$' 'IWDG is Secure'
require_pattern ECU.ioc '^ADC1.ExternalTrigConv=ADC_EXTERNALTRIG_T6_TRGO$' 'Slow ADC scan is hardware-triggered at 1 kHz'
require_pattern ECU.ioc '^ADC2.ExternalTrigConv=ADC_EXTERNALTRIG_T4_TRGO$' 'Current ADC scan is synchronized to PWM'
require_pattern ECU.ioc '^ADC1.SamplingTime-7\\#ChannelRegularConversion=ADC_SAMPLETIME_640CYCLES_5$' 'VREFINT sampling time satisfies the datasheet minimum'
require_pattern ECU.ioc '^TIM4.Channel-PWM\\ Generation4\\ No\\ Output=TIM_CHANNEL_4$' 'Internal TIM4 CH4 midpoint trigger is persistent'
require_pattern ECU.ioc '^TIM4.CounterMode=TIM_COUNTERMODE_CENTERALIGNED1$' 'Valve PWM is center aligned'
require_pattern ECU.ioc '^FDCAN1.CalculateBaudRateNominal=250000$' 'FDCAN1 nominal bitrate is 250 kbit/s'
require_pattern ECU.ioc '^FDCAN2.CalculateBaudRateNominal=250000$' 'FDCAN2 nominal bitrate is 250 kbit/s'
require_pattern ECU.ioc '^SPI4.DataSize=SPI_DATASIZE_8BIT$' 'TPIC SPI words are 8 bit'
require_pattern ECU.ioc '^SPI4.BaudRatePrescaler=SPI_BAUDRATEPRESCALER_8$' 'TPIC SPI clock remains derated'
require_pattern ECU.ioc '^TIM2.ICFilter_CH1=8$' 'Speed capture input filter is persisted'
require_pattern ECU.ioc '^IWDG.Reload=1999$' 'Secure watchdog timeout baseline is persisted'
require_pattern ECU.ioc '^RCC.EnbaleCSS=true$' 'HSE clock security system is enabled'
require_pattern Secure/Core/Src/tim.c 'sConfigOC.Pulse = 3125;' 'Generated TIM4 midpoint compare is present'
require_pattern Secure/Core/Src/i2c.c 'HAL_I2CEx_ConfigDigitalFilter\(&hi2c1, 2\)' 'Generated I2C1 digital filter is present'
require_pattern Secure/Core/Src/gpio.c 'TPIC_OE_N_Pin, GPIO_PIN_SET' 'Generated TPIC output-enable boot level is fail-safe'
require_pattern Secure/Core/Src/gtzc_s.c 'GTZC_PERIPH_ADC12, GTZC_TZSC_PERIPH_SEC' 'Generated GTZC code secures ADC12'
require_pattern Secure/App/Src/safety_service.c 'Safety_LatchFault\(SAFETY_STATUS_ESTOP_ACTIVE\)' 'Secure E-stop fault latching is present'
require_pattern NonSecure/App/Src/ecu_app.c 'SECURE_SafetyKickWatchdog' 'NonSecure health heartbeat feeds Secure IWDG service'
require_pattern Secure/STM32H563xx_FLASH_s.ld 'ORIGIN = 0xc000000,    LENGTH = 1016K' 'Secure Flash linker region matches Bank1 minus NSC'
require_pattern Secure/STM32H563xx_FLASH_s.ld 'ORIGIN = 0xc0fe000,    LENGTH = 8K' 'NSC linker region is the last Bank1 sector'
require_pattern NonSecure/STM32H563xx_FLASH_ns.ld 'ORIGIN = 0x8100000,    LENGTH = 1024K' 'NonSecure Flash linker region matches Bank2'

if ((task_failures != 0)); then
  printf '\nConfiguration audit failed: %d check(s).\n' "${task_failures}" >&2
  exit 1
fi

printf '\nConfiguration audit passed.\n'
