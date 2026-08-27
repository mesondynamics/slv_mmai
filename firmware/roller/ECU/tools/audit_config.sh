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

require_file() {
  local task_file="$1"
  local task_description="$2"
  if [[ -f "${task_project_dir}/${task_file}" ]]; then
    printf 'PASS  %s\n' "${task_description}"
  else
    printf 'FAIL  %s (%s)\n' "${task_description}" "${task_file}" >&2
    task_failures=$((task_failures + 1))
  fi
}

reject_file() {
  local task_file="$1"
  local task_description="$2"
  if [[ -e "${task_project_dir}/${task_file}" ]]; then
    printf 'FAIL  %s (%s)\n' "${task_description}" "${task_file}" >&2
    task_failures=$((task_failures + 1))
  else
    printf 'PASS  %s\n' "${task_description}"
  fi
}

require_pattern ECU.ioc '^PB2.GPIO_Label=ESTOP_DETECT$' 'E-stop is mapped to schematic pin PB2'
reject_pattern ECU.ioc '(^|,)(PG2|LPUART1|NUCLEO-H563ZI)(\\:I)?(,|$)|^BSP_IP_NAME=' 'No Nucleo/LPUART/PG2 ghost configuration remains'
require_pattern ECU.ioc '^board=custom$' 'CubeMX project targets the custom ECU board'
require_pattern ECU.ioc '^ProjectManager.KeepUserCode=true$' 'CubeMX user-code preservation is enabled'
require_pattern ECU.ioc '^PB8.GPIO_Label=SW_I2C_SDA$' 'PCB R1 swapped PB8 is persistently assigned software SDA'
require_pattern ECU.ioc '^PB9.GPIO_Label=SW_I2C_SCL$' 'PCB R1 swapped PB9 is persistently assigned software SCL'
require_pattern ECU.ioc '^PB8.GPIO_ModeDefaultOutputPP=GPIO_MODE_OUTPUT_OD$' 'PB8 software I2C is open-drain in CubeMX'
require_pattern ECU.ioc '^PB9.GPIO_ModeDefaultOutputPP=GPIO_MODE_OUTPUT_OD$' 'PB9 software I2C is open-drain in CubeMX'
reject_pattern ECU.ioc '^(PB8|PB9)[.]Signal=I2C1_' 'Hardware I2C alternate functions are not used on PCB R1'
reject_file Secure/Core/Src/i2c.c 'Obsolete generated hardware I2C implementation is absent'
require_pattern ECU.ioc '^GTZC_S.ADC_Secure=GTZC_TZSC_PERIPH_SEC$' 'ADC1/ADC2 are Secure'
require_pattern ECU.ioc '^GTZC_S.TIM4_Secure=GTZC_TZSC_PERIPH_SEC$' 'Valve PWM timer is Secure'
require_pattern ECU.ioc '^GTZC_S.IWDG_Secure=GTZC_TZSC_PERIPH_SEC$' 'IWDG is Secure'
require_pattern ECU.ioc '^GTZC_S.SPI4_Secure=GTZC_TZSC_PERIPH_SEC$' 'TPIC SPI4 is Secure'
require_pattern ECU.ioc '^GPDMA1.ChannelPRIV_GPDMACH0=DMA_CHANNEL_PRIV$' 'ADC1 DMA channel is privileged for Secure SRAM access'
require_pattern ECU.ioc '^GPDMA1.ChannelPRIV_GPDMACH1=DMA_CHANNEL_PRIV$' 'ADC2 DMA channel is privileged for Secure SRAM access'
require_pattern ECU.ioc '^ADC1.ExternalTrigConv=ADC_EXTERNALTRIG_T6_TRGO$' 'Slow ADC scan is hardware-triggered at 1 kHz'
require_pattern ECU.ioc '^ADC2.ExternalTrigConv=ADC_EXTERNALTRIG_T4_TRGO$' 'Current ADC scan is synchronized to PWM'
require_pattern ECU.ioc '^ADC2[.]SamplingTime-0.*=ADC_SAMPLETIME_12CYCLES_5$' 'Forward-current ADC sampling time supports the 20 kHz loop'
require_pattern ECU.ioc '^ADC2[.]SamplingTime-1.*=ADC_SAMPLETIME_12CYCLES_5$' 'Reverse-current ADC sampling time supports the 20 kHz loop'
require_pattern ECU.ioc '^ADC1[.]SamplingTime-7.*ChannelRegularConversion=ADC_SAMPLETIME_640CYCLES_5$' 'VREFINT sampling time satisfies the datasheet minimum'
require_pattern ECU.ioc '^TIM4[.]Channel-PWM.*Generation4.*No.*Output=TIM_CHANNEL_4$' 'Internal TIM4 CH4 midpoint trigger is persistent'
require_pattern ECU.ioc '^TIM4.CounterMode=TIM_COUNTERMODE_CENTERALIGNED1$' 'Valve PWM is center aligned'
require_pattern ECU.ioc '^TIM4.PulseNoDither_4=1$' 'ADC2 trigger is at the center-aligned PWM conduction valley'
require_pattern ECU.ioc '^NVIC1.ADC2_IRQn=true\\:1\\:0' 'ADC2 control-loop IRQ priority is persisted as 1'
require_pattern ECU.ioc '^NVIC1.GPDMA1_Channel1_IRQn=true\\:1\\:0' 'ADC2 DMA IRQ priority is persisted as 1'
require_pattern ECU.ioc '^FDCAN1.CalculateBaudRateNominal=250000$' 'FDCAN1 nominal bitrate is 250 kbit/s'
require_pattern ECU.ioc '^FDCAN2.CalculateBaudRateNominal=250000$' 'FDCAN2 nominal bitrate is 250 kbit/s'
require_pattern ECU.ioc '^FDCAN1.ExtFiltersNbr=6$' 'CAN1 reserves exactly six J1939 extended filters'
require_pattern ECU.ioc '^FDCAN1.StdFiltersNbr=0$' 'CAN1 has no unspecified standard-ID filters'
require_pattern ECU.ioc '^FDCAN2.ExtFiltersNbr=0$' 'CAN2 remains reserved and unstarted'
require_pattern ECU.ioc '^SPI4.DataSize=SPI_DATASIZE_8BIT$' 'TPIC SPI words are 8 bit'
require_pattern ECU.ioc '^SPI4.BaudRatePrescaler=SPI_BAUDRATEPRESCALER_32$' 'TPIC SPI clock is derated to 3.90625 Mbit/s'
require_pattern ECU.ioc '^ETH.MACAddr=8A\\:EA\\:B5\\:00\\:00\\:02$' 'Legacy ECU MAC address is persisted in CubeMX'
require_pattern ECU.ioc '^TIM2.ICFilter_CH1=8$' 'Speed capture input filter is persisted'
require_pattern ECU.ioc '^IWDG.Reload=1999$' 'Secure watchdog timeout baseline is persisted'
require_pattern ECU.ioc '^RCC.EnbaleCSS=true$' 'HSE clock security system is enabled'
require_pattern Secure/Core/Src/tim.c 'sConfigOC.Pulse = 1;' 'Generated TIM4 ADC trigger compare is present'
require_pattern Secure/Core/Src/adc.c 'HAL_NVIC_SetPriority[(]ADC2_IRQn, 1, 0[)]' 'Generated ADC2 IRQ priority is 1'
require_pattern Secure/Core/Src/gpdma.c 'HAL_NVIC_SetPriority[(]GPDMA1_Channel1_IRQn, 1, 0[)]' 'Generated ADC2 DMA IRQ priority is 1'
require_pattern Secure/Core/Src/gpio.c 'GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;' 'Generated software I2C pins remain open-drain'
require_pattern Secure/Core/Src/gpio.c 'TPIC_OE_N_Pin, GPIO_PIN_SET' 'Generated TPIC output-enable boot level is fail-safe'
require_pattern Secure/Core/Src/gtzc_s.c 'GTZC_PERIPH_ADC12, GTZC_TZSC_PERIPH_SEC' 'Generated GTZC code secures ADC12'
require_pattern Secure/Core/Src/gtzc_s.c 'GTZC_PERIPH_SPI4, GTZC_TZSC_PERIPH_SEC' 'Generated GTZC code secures SPI4'
require_pattern Secure/Core/Src/adc.c 'handle_GPDMA1_Channel0, DMA_CHANNEL_PRIV[|]DMA_CHANNEL_SEC' 'Generated ADC1 DMA is Secure and privileged'
require_pattern Secure/Core/Src/adc.c 'handle_GPDMA1_Channel1, DMA_CHANNEL_PRIV[|]DMA_CHANNEL_SEC' 'Generated ADC2 DMA is Secure and privileged'
require_pattern Secure/Core/Src/gtzc_s.c '^#define ETH_DMA_SRAM3_PRIV_VECTOR_COUNT[[:space:]]+4U' 'ETH DMA non-privileged SRAM3 exception is bounded to 64 KiB'
require_pattern Secure/Core/Src/gtzc_s.c 'HAL_GTZC_MPCBB_GetConfigMem[(]SRAM3_BASE, &ethernet_dma_area_desc[)]' 'ETH DMA SRAM3 exception preserves generated GTZC attributes'
require_pattern Secure/Core/Src/gtzc_s.c 'MPCBB_PrivConfig_array\[ethernet_dma_vector\] = 0U;' 'ETH DMA SRAM3 vectors are made non-privileged in preserved USER CODE'
require_pattern Secure/App/Src/software_i2c.c 'pulse < 9U' 'Software I2C includes bounded nine-clock bus recovery'
require_pattern Secure/App/Src/safety_service.c 'SAFETY_STATUS_COMMAND_TIMEOUT' 'Secure actuator command timeout path is present'
require_pattern Secure/App/Src/safety_service.c 'SAFETY_ENGINE_START_MAX_MS' 'Secure engine starter hold limit is present'
require_pattern Secure/App/Src/safety_service.c 'GPIOE->BSRR = TPIC_CTRL_BUF_EN_Pin;' 'TPIC level shifter is enabled while OE remains fail-safe high'
require_pattern Secure/App/Src/safety_service.c 'if [(]!Safety_TpicShift[(]0U[)][)]' 'TPIC zero image is clocked and latched before outputs are armed'
require_pattern Secure/App/Src/safety_service.c '^#define ENGINE_TRIGGER_ACTIVE_MS[[:space:]]+1000UL$' 'K24/K25 engine-speed trigger is exactly one second'
require_pattern Secure/App/Src/safety_service.c 'mask &= ~[(]SAFETY_RELAY_ENGINE_SPEED_OVERRIDE' 'K3/K24/K25 are cleared before dynamic engine-speed selection'
reject_pattern Secure/App/Src/safety_service.c 'SAFETY_RELAY_VIB_REAR_OVERRIDE [|][[:space:]]*SAFETY_RELAY_ENGINE_SPEED_OVERRIDE' 'K3 is absent from the persistent normal-authority takeover mask'
require_pattern Secure/App/Src/safety_service.c 'engine_speed_state == ENGINE_SPEED_TRIGGER_ACTIVE' 'K24/K25 act only in the bounded trigger state'
require_pattern Secure/App/Src/safety_service.c 'ValveControl_Step' 'Secure ADC callback runs the valve current controller'
require_pattern Secure/App/Src/safety_service.c 'Safety_SaveValveConfig' 'Valve configuration persistence is owned by Secure code'
require_pattern Secure/App/Src/valve_control.c '^#define VALVE_OVERCURRENT_FAST_MA[[:space:]]+2500U$' 'Immutable fast overcurrent threshold is present'
require_pattern Secure/App/Src/valve_control.c 'SAFETY_STATUS_VALVE_OPEN_LOAD' 'Open-load protection is present'
require_pattern Secure/App/Src/valve_control.c 'VALVE_DIRECTION_DEADTIME_SAMPLES' 'Safe direction reversal deadtime is present'
require_pattern Secure/App/Src/valve_config_store.c '^#define VALVE_STORE_SECTOR_A[[:space:]]+125U$' 'Valve parameter journal starts in Secure sector 125'
require_pattern Secure/App/Src/valve_config_store.c '^#define VALVE_STORE_SECTOR_B[[:space:]]+126U$' 'Valve parameter journal alternates through Secure sector 126'
require_pattern Secure/App/Src/valve_config_store.c 'VALVE_STORE_COMMIT' 'Valve parameter records use a final commit marker'
require_pattern Secure_nsclib/safety_api.h 'K12; 1=run circuit' 'K12 physical run-permit semantics are explicit in the shared ABI'
require_pattern NonSecure/App/Src/ecu_app.c 'SECURE_SafetyKickWatchdog' 'NonSecure health heartbeat feeds Secure IWDG service'
require_pattern NonSecure/App/Src/ecu_network.c '#define CONTROL_TIMEOUT_MS[[:space:]]+250UL' 'UDP control authority expires after 250 ms'
require_pattern NonSecure/App/Src/ecu_network.c 'Network_TimeExpired[(]now, slot->last_update_tick, CONTROL_TIMEOUT_MS[)]' 'UDP timeout comparison is wrap-safe and cannot expire a callback timestamp from the next tick'
require_pattern NonSecure/App/Src/ecu_network.c 'slot->sequence_valid = false;' 'Expired UDP authority starts a recoverable new sequence session'
require_pattern NonSecure/App/Src/ecu_network.c '#define CONTROL_PRIORITY_EMERGENCY[[:space:]]+255U' 'Emergency network priority is fixed at 255'
require_pattern NonSecure/App/Src/ecu_network.c '#define CONTROL_MAX_SENDERS[[:space:]]+6U' 'Six-sender control capacity is retained in V2'
require_pattern NonSecure/App/Src/ecu_network.c 'datagram[.]control[.]emergency_stop_request != 0U' 'V2 emergency request is promoted to fail-safe emergency authority'
require_pattern NonSecure/App/Src/ecu_data_model.c 'run_permit_on = [(]control->emergency_stop_request == 0U[)]' 'Network E-stop semantic is inverted at the physical K12 boundary'
require_pattern NonSecure/App/Src/ecu_network.c 'udp_bind[(]transmit_pcb, IP_ANY_TYPE, ECU_STATUS_PORT[)]' 'Status source port remains UDP/50001'
require_pattern NonSecure/App/Src/ecu_network.c 'ECU_MESSAGE_VALVE_CONFIG_APPLY' 'V2 live valve tuning service is present'
require_pattern NonSecure/App/Src/ecu_network.c 'ECU_MESSAGE_VALVE_TELEMETRY' 'V2 valve telemetry service is present'
reject_pattern NonSecure/App 'throttle_percent' 'Obsolete percentage valve control is absent from active NonSecure code'
reject_pattern Secure/App 'throttle_percent' 'Obsolete percentage valve control is absent from Secure code'
require_pattern NonSecure/App/Network/lwipopts.h '^#define MEM_ALIGNMENT[[:space:]]+32$' 'Ethernet zero-copy pools preserve 32-byte buffer alignment'
require_pattern NonSecure/CMakeLists.txt 'eth_dma_sram3_guard[.]ld' 'NonSecure link includes the ETH DMA SRAM3 window guard'
require_pattern NonSecure/App/Linker/eth_dma_sram3_guard.ld 'ASSERT[(]__bss_end__ <= 0x20060000' 'LwIP/static data cannot outgrow the 64 KiB ETH DMA window'
require_pattern NonSecure/App/Inc/ecu_network.h '#define ECU_IP_ADDRESS_3[[:space:]]+11U' 'ECU static address is 172.16.0.11'
require_pattern NonSecure/App/Src/j1939.c 'HAL_FDCAN_ConfigGlobalFilter.*FDCAN_REJECT' 'CAN1 global filter rejects unspecified traffic'
require_pattern NonSecure/App/Inc/speed_sensor.h 'SPEED_SENSOR_PULSES_PER_KILOMETER 20000UL' 'Legacy speed calibration is retained'
require_pattern NonSecure/App/Src/ecu_protocol.c '_Static_assert[(]sizeof[(]ECU_StatusPayloadV2[)] == 100U' 'V2 UDP status ABI has a compile-time size guard'
require_pattern NonSecure/App/Inc/ecu_protocol.h '^#define ECU_V2_MAGIC[[:space:]]+0x32554345UL' 'V2 protocol magic is explicit'
require_file ThirdParty/LwIP/src/core/init.c 'Pinned LwIP sources are vendored outside generated directories'
require_file Drivers/BSP/Components/lan8742/lan8742.c 'LAN8742 PHY driver is vendored'
require_file tools/ecu_debug_ui/index.html 'Zero-dependency ECU acceptance UI is present'
require_file tools/configure_ecu_network.sh 'Idempotent enp2s0 bench-network helper is present'
reject_pattern tools/configure_ecu_network.sh '[|][[:space:]]*rg[[:space:]]' 'sudo network helper has no user-PATH ripgrep dependency'
require_file tools/check_ecu_latency.sh 'Ethernet latency regression gate is present'
require_pattern NonSecure/App/Network/ethernetif.c 'HAL_ETH_Transmit_IT' 'Ethernet transmit path is non-blocking'
reject_pattern NonSecure/App/Network/ethernetif.c 'HAL_ETH_Transmit[(]' 'Blocking Ethernet transmit is absent'
require_pattern NonSecure/App/Src/ecu_network.c '^#define TELEMETRY_SEND_PERIOD_MS[[:space:]]+8UL$' '1 kHz samples are batched into bounded-rate telemetry frames'
require_file tools/program_ecu.sh 'Guarded TrustZone provisioning and flash helper is present'
require_pattern tools/program_ecu.sh 'check_target_identity' 'Flash helper verifies the reviewed target identity before writes'
require_pattern tools/program_ecu.sh 'backup_existing_flash' 'TrustZone provisioning backs up existing Flash before OB migration'
require_pattern tools/program_ecu.sh 'backup_valve_parameters' 'Normal firmware updates back up valve parameter sectors'
require_pattern tools/program_ecu.sh 'verify_valve_parameters_unchanged' 'Normal firmware updates verify valve parameters are preserved'
require_pattern tools/program_ecu.sh 'SECBOOTADD=0xC0000 SECBOOT_LOCK=0xC3' 'TrustZone provisioning persists the Secure boot address before reset'
require_file tools/ecu_bench_test.py 'Guarded Ethernet actuator acceptance test is present'
require_pattern Secure/STM32H563xx_FLASH_s.ld 'ORIGIN = 0xc000000,    LENGTH = 1000K' 'Secure Flash excludes both valve parameter sectors and NSC'
require_pattern Secure/App/Linker/valve_config_flash_guard.ld 'ASSERT[(]__data_source_end <= 0x0C0FA000' 'Additional linker guard protects valve parameter sectors after CubeMX regeneration'
require_pattern Secure/STM32H563xx_FLASH_s.ld 'ORIGIN = 0xc0fe000,    LENGTH = 8K' 'NSC linker region is the last Bank1 sector'
require_pattern NonSecure/STM32H563xx_FLASH_ns.ld 'ORIGIN = 0x8100000,    LENGTH = 1024K' 'NonSecure Flash linker region matches Bank2'

if ((task_failures != 0)); then
  printf '\nConfiguration audit failed: %d check(s).\n' "${task_failures}" >&2
  exit 1
fi

printf '\nConfiguration audit passed.\n'
