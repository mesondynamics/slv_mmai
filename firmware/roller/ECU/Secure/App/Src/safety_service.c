#include "safety_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "adc.h"
#include "iwdg.h"
#include "main.h"
#include "software_i2c.h"
#include "spi.h"
#include "tim.h"
#include "valve_config_store.h"
#include "valve_control.h"

#define TPIC_SPI_TIMEOUT_US          1000UL
#define TPIC_LATCH_PULSE_US             1UL
#define ENGINE_TAKEOVER_SETTLE_MS       50UL
#define ENGINE_TRIGGER_ACTIVE_MS      1000UL
#define ENGINE_RESTORE_SETTLE_MS        50UL
#define ENGINE_INTER_TRIGGER_MS      2000UL
#define VALVE_TELEMETRY_RING_SIZE     128U
#define VALVE_CONFIG_SAVE_INTERVAL_MS 10000UL
#define ADC_VREFINT_INDEX                7U

typedef enum
{
  ENGINE_SPEED_IDLE = 0,
  ENGINE_SPEED_TAKEOVER_SETTLE,
  ENGINE_SPEED_TRIGGER_ACTIVE,
  ENGINE_SPEED_RESTORE_SETTLE,
  ENGINE_SPEED_INTER_TRIGGER
} EngineSpeedState;

static volatile uint16_t slow_adc_dma[SAFETY_SLOW_ADC_COUNT];
static volatile uint16_t current_adc_dma[SAFETY_CURRENT_ADC_COUNT];
static volatile uint16_t slow_adc_snapshot[SAFETY_SLOW_ADC_COUNT];
static volatile uint16_t current_adc_snapshot[SAFETY_CURRENT_ADC_COUNT];
static volatile uint32_t slow_sequence;
static volatile uint32_t current_sequence;
static volatile uint32_t safety_status;
static volatile uint32_t secure_uptime_ms;
static volatile uint32_t requested_relay_mask;
static volatile uint32_t applied_relay_mask;
static volatile uint32_t last_command_tick;
static volatile uint32_t last_command_sequence;
static ValveControl valve_control;
static SAFETY_ValveConfigSnapshot valve_config_snapshot;
static SAFETY_ValveTelemetrySample valve_telemetry[VALVE_TELEMETRY_RING_SIZE];
static volatile uint32_t valve_telemetry_write_sequence;
static volatile uint32_t valve_telemetry_read_sequence;
static volatile uint32_t valve_telemetry_dropped;
static volatile uint32_t last_current_tick;
static volatile uint32_t last_config_save_tick;
static volatile int8_t engine_speed_level;
static volatile int8_t engine_speed_remaining;
static volatile EngineSpeedState engine_speed_state;
static volatile uint32_t engine_state_tick;
static volatile uint32_t engine_start_deadline;
static uint32_t last_watchdog_heartbeat = UINT32_MAX;
static uint8_t command_sequence_valid;
static uint8_t command_fresh;
static uint8_t timer_ready;
static uint8_t software_i2c_bus_ok;
static uint8_t engine_start_active;
static uint8_t engine_start_lockout;
static uint8_t last_engine_start_request;
static int8_t previous_engine_speed_request;

static void Safety_RecordValveTelemetry(void)
{
  SAFETY_ValveTelemetrySample *sample;
  uint32_t sequence;

  if ((valve_control.loop_sequence % VALVE_TELEMETRY_DECIMATION) != 0U)
  {
    return;
  }
  sequence = valve_telemetry_write_sequence;
  if ((sequence - valve_telemetry_read_sequence) >=
      VALVE_TELEMETRY_RING_SIZE)
  {
    ++valve_telemetry_read_sequence;
    ++valve_telemetry_dropped;
  }
  sample = &valve_telemetry[sequence & (VALVE_TELEMETRY_RING_SIZE - 1U)];
  sample->timestamp_us = valve_control.loop_sequence * 50UL;
  sample->requested_target_ma = valve_control.requested_target_ma;
  sample->applied_target_ma = ValveControl_AppliedTargetMa(&valve_control);
  sample->forward_current_ma = valve_control.forward_current_ma;
  sample->reverse_current_ma = valve_control.reverse_current_ma;
  sample->forward_duty_permille = valve_control.forward_duty_permille;
  sample->reverse_duty_permille = valve_control.reverse_duty_permille;
  sample->state = (uint16_t)valve_control.state;
  sample->fault_flags = (uint16_t)(valve_control.fault_flags >> 12U);
  __DMB();
  valve_telemetry_write_sequence = sequence + 1U;
}

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

static bool Safety_TimeReached(uint32_t now, uint32_t deadline)
{
  return (int32_t)(now - deadline) >= 0;
}

static bool Safety_TpicWaitFlag(uint32_t flag)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t timeout_cycles = (SystemCoreClock / 1000000UL) * TPIC_SPI_TIMEOUT_US;

  while ((SPI4->SR & flag) == 0U)
  {
    if ((uint32_t)(DWT->CYCCNT - start) >= timeout_cycles)
    {
      return false;
    }
  }
  return true;
}

static void Safety_TpicDelayUs(uint32_t microseconds)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t cycles_per_us = SystemCoreClock / 1000000UL;
  uint32_t delay_cycles;

  if (cycles_per_us == 0U) { cycles_per_us = 1U; }
  delay_cycles = cycles_per_us * microseconds;
  while ((uint32_t)(DWT->CYCCNT - start) < delay_cycles)
  {
  }
}

static void Safety_TpicLatch(void)
{
  GPIOE->BSRR = TPIC_RCK_Pin;
  Safety_TpicDelayUs(TPIC_LATCH_PULSE_US);
  GPIOE->BSRR = (uint32_t)TPIC_RCK_Pin << 16U;
  __DSB();
}

static bool Safety_TpicShift(uint32_t relay_mask)
{
  uint8_t bytes[4];
  uint32_t index;

  relay_mask &= ~SAFETY_RELAY_RESERVED_MASK;
  /* MSB first: the earliest bit propagates to the furthest cascaded TPIC. */
  bytes[0] = (uint8_t)(relay_mask >> 24U);
  bytes[1] = (uint8_t)(relay_mask >> 16U);
  bytes[2] = (uint8_t)(relay_mask >> 8U);
  bytes[3] = (uint8_t)relay_mask;

  CLEAR_BIT(SPI4->CR1, SPI_CR1_SPE);
  SPI4->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC | SPI_IFCR_UDRC |
               SPI_IFCR_OVRC | SPI_IFCR_MODFC;
  MODIFY_REG(SPI4->CR2, SPI_CR2_TSIZE, 4U);
  SET_BIT(SPI4->CR1, SPI_CR1_SPE);
  SET_BIT(SPI4->CR1, SPI_CR1_CSTART);

  for (index = 0U; index < sizeof(bytes); ++index)
  {
    if (!Safety_TpicWaitFlag(SPI_SR_TXP))
    {
      CLEAR_BIT(SPI4->CR1, SPI_CR1_SPE);
      return false;
    }
    *(__IO uint8_t *)&SPI4->TXDR = bytes[index];
  }
  if (!Safety_TpicWaitFlag(SPI_SR_EOT))
  {
    CLEAR_BIT(SPI4->CR1, SPI_CR1_SPE);
    return false;
  }

  SPI4->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC;
  CLEAR_BIT(SPI4->CR1, SPI_CR1_SPE);
  Safety_TpicLatch();
  return true;
}

static void Safety_ResetCommandState(void)
{
  requested_relay_mask = 0U;
  applied_relay_mask = 0U;
  ValveControl_ForceSafe(&valve_control);
  command_sequence_valid = 0U;
  command_fresh = 0U;
  engine_speed_level = 0;
  engine_speed_remaining = 0;
  engine_speed_state = ENGINE_SPEED_IDLE;
  engine_state_tick = secure_uptime_ms;
  previous_engine_speed_request = 0;
  engine_start_active = 0U;
  engine_start_lockout = 0U;
  last_engine_start_request = 0U;
  engine_start_deadline = 0U;
}

static void Safety_ForceOutputsSafe(void)
{
  if (timer_ready != 0U)
  {
    TIM4->CCR1 = 0U;
    TIM4->CCR2 = 0U;
  }

  /* OE_N high immediately removes relay drive. CLR_N and the AHCT541 buffer
     enable low then make the stored command safe even if NonSecure stalls. */
  GPIOE->BSRR = TPIC_OE_N_Pin;
  GPIOE->BSRR = (uint32_t)(TPIC_CLR_N_Pin | TPIC_CTRL_BUF_EN_Pin |
                           TPIC_RCK_Pin) << 16U;
  Safety_ResetCommandState();
  safety_status &= ~SAFETY_STATUS_OUTPUTS_ARMED;
}

static void Safety_LatchFault(uint32_t reason)
{
  uint32_t primask = Safety_EnterCritical();
  safety_status |= SAFETY_STATUS_FAULT_LATCHED | reason;
  Safety_ForceOutputsSafe();
  Safety_ExitCritical(primask);
}

static bool Safety_AllBooleanFieldsValid(const SAFETY_ActuatorCommand *command)
{
  const uint8_t fields[] = {
    command->pump_enable, command->pump_select,
    command->headlamp_front_on, command->headlamp_rear_on,
    command->led_front_on, command->led_rear_on,
    command->buzzer_reverse_on, command->buzzer_main_on,
    command->vib_strong_on, command->vib_weak_on,
    command->vib_front_selected, command->vib_rear_selected,
    command->engine_start_request, command->speed_mode_high,
    command->parking_brake_on, command->run_permit_on,
    command->turn_signal_right_on, command->turn_signal_left_on
  };
  uint32_t index;

  for (index = 0U; index < sizeof(fields); ++index)
  {
    if (fields[index] > 1U)
    {
      return false;
    }
  }
  for (index = 0U; index < sizeof(command->reserved); ++index)
  {
    if (command->reserved[index] != 0U)
    {
      return false;
    }
  }
  return true;
}

static int32_t Safety_ValidateCommand(const SAFETY_ActuatorCommand *command)
{
  if (command == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  if (command->api_version != SAFETY_ACTUATOR_API_VERSION)
  {
    return SAFETY_RESULT_UNSUPPORTED_VERSION;
  }
  if ((command->valve_current_target_ma < -SAFETY_VALVE_TARGET_MAX_MA) ||
      (command->valve_current_target_ma > SAFETY_VALVE_TARGET_MAX_MA) ||
      (command->reserved_current != 0U))
  {
    return SAFETY_RESULT_RANGE;
  }
  if ((command->valve_current_target_ma != 0) &&
      (command->run_permit_on == 0U))
  {
    return SAFETY_RESULT_CONFLICT;
  }
  if (!Safety_AllBooleanFieldsValid(command) ||
      (command->engine_speed_level < -3) ||
      (command->engine_speed_level > 3))
  {
    return SAFETY_RESULT_RANGE;
  }
  if ((command->vib_strong_on != 0U) && (command->vib_weak_on != 0U))
  {
    return SAFETY_RESULT_CONFLICT;
  }
  return SAFETY_RESULT_OK;
}

static uint32_t Safety_BuildBaseRelayMask(const SAFETY_ActuatorCommand *command)
{
  uint32_t mask = SAFETY_RELAY_VIB_ENABLE_OVERRIDE |
                  SAFETY_RELAY_VIB_HIGH_OVERRIDE |
                  SAFETY_RELAY_VIB_LOW_OVERRIDE |
                  SAFETY_RELAY_VIB_FRONT_OVERRIDE |
                  SAFETY_RELAY_VIB_REAR_OVERRIDE |
                  SAFETY_RELAY_TRAVEL_SPEED_OVERRIDE;

  if (command->pump_enable != 0U)
  {
    mask |= (command->pump_select != 0U) ? SAFETY_RELAY_PUMP_2 :
                                          SAFETY_RELAY_PUMP_1;
  }
  if (command->headlamp_front_on != 0U) { mask |= SAFETY_RELAY_FRONT_MAIN; }
  if (command->headlamp_rear_on != 0U)  { mask |= SAFETY_RELAY_REAR_MAIN; }
  if (command->led_front_on != 0U)      { mask |= SAFETY_RELAY_FRONT_LED; }
  if (command->led_rear_on != 0U)       { mask |= SAFETY_RELAY_REAR_LED; }
  if (command->buzzer_reverse_on != 0U) { mask |= SAFETY_RELAY_REVERSE_ALARM; }
  if (command->buzzer_main_on != 0U)    { mask |= SAFETY_RELAY_HORN; }
  if (command->vib_strong_on != 0U)     { mask |= SAFETY_RELAY_VIB_HIGH_LEVEL; }
  if (command->vib_weak_on != 0U)       { mask |= SAFETY_RELAY_VIB_LOW_LEVEL; }
  if (command->vib_front_selected != 0U){ mask |= SAFETY_RELAY_VIB_FRONT_LEVEL; }
  if (command->vib_rear_selected != 0U) { mask |= SAFETY_RELAY_VIB_REAR_LEVEL; }
  if (command->speed_mode_high != 0U)   { mask |= SAFETY_RELAY_TRAVEL_SPEED_LEVEL; }
  if (command->parking_brake_on != 0U)  { mask |= SAFETY_RELAY_PARK_BRAKE_OVERRIDE; }
  if (command->run_permit_on != 0U) { mask |= SAFETY_RELAY_ESTOP_RUN_PERMIT; }
  if (command->turn_signal_right_on != 0U) { mask |= SAFETY_RELAY_RIGHT_TURN; }
  if (command->turn_signal_left_on != 0U)  { mask |= SAFETY_RELAY_LEFT_TURN; }
  return mask;
}

static uint32_t Safety_ApplyDynamicRelayBits(uint32_t mask)
{
  /* K3 cuts the OEM engine-speed signal only during a high/low trigger.
     K24 applies +BAT/high speed; K25 applies PGND/low speed.  Clear all three
     first so K24/K25 can never be commanded together and K3 is never left in
     takeover after a completed or aborted trigger. */
  mask &= ~(SAFETY_RELAY_ENGINE_SPEED_OVERRIDE |
            SAFETY_RELAY_ENGINE_SPEED_UP |
            SAFETY_RELAY_ENGINE_SPEED_DOWN |
            SAFETY_RELAY_ENGINE_START);

  if ((engine_speed_state == ENGINE_SPEED_TAKEOVER_SETTLE) ||
      (engine_speed_state == ENGINE_SPEED_TRIGGER_ACTIVE) ||
      (engine_speed_state == ENGINE_SPEED_RESTORE_SETTLE))
  {
    mask |= SAFETY_RELAY_ENGINE_SPEED_OVERRIDE;
  }
  if (engine_speed_state == ENGINE_SPEED_TRIGGER_ACTIVE)
  {
    mask |= (engine_speed_remaining > 0) ? SAFETY_RELAY_ENGINE_SPEED_UP :
                                          SAFETY_RELAY_ENGINE_SPEED_DOWN;
  }
  if (engine_start_active != 0U)
  {
    mask |= SAFETY_RELAY_ENGINE_START;
  }
  return mask;
}

static void Safety_ApplyEngineRequest(int8_t request)
{
  if (request == previous_engine_speed_request)
  {
    return;
  }
  previous_engine_speed_request = request;

  if (request == 0)
  {
    engine_speed_level = 0;
    engine_speed_remaining = 0;
    /* A normal cancel releases K24/K25 first and retains K3 for the contact
       settling interval. Fault/timeout paths still force every output off
       immediately through Safety_ForceOutputsSafe(). */
    if ((engine_speed_state == ENGINE_SPEED_TAKEOVER_SETTLE) ||
        (engine_speed_state == ENGINE_SPEED_TRIGGER_ACTIVE) ||
        (engine_speed_state == ENGINE_SPEED_RESTORE_SETTLE))
    {
      engine_speed_state = ENGINE_SPEED_RESTORE_SETTLE;
    }
    else
    {
      engine_speed_state = ENGINE_SPEED_IDLE;
    }
    engine_state_tick = secure_uptime_ms;
  }
  else if (engine_speed_state == ENGINE_SPEED_IDLE)
  {
    engine_speed_level = request;
    engine_speed_remaining = request;
    engine_speed_state = ENGINE_SPEED_TAKEOVER_SETTLE;
    engine_state_tick = secure_uptime_ms;
  }
  /* A direction/count change while a pulse train is active is deliberately
     ignored, matching the previous ECU and preventing conflicting relays. */
}

static void Safety_ApplyStartRequest(uint8_t request)
{
  if (request == 0U)
  {
    engine_start_active = 0U;
    engine_start_lockout = 0U;
    safety_status &= ~SAFETY_STATUS_START_LIMIT;
  }
  else if ((last_engine_start_request == 0U) &&
           (engine_start_lockout == 0U))
  {
    engine_start_active = 1U;
    engine_start_deadline = secure_uptime_ms + SAFETY_ENGINE_START_MAX_MS;
  }
  last_engine_start_request = request;
}

static bool Safety_AdvanceEngineSpeed(void)
{
  uint32_t elapsed = secure_uptime_ms - engine_state_tick;
  EngineSpeedState old_state = engine_speed_state;

  switch (engine_speed_state)
  {
    case ENGINE_SPEED_TAKEOVER_SETTLE:
      if (elapsed >= ENGINE_TAKEOVER_SETTLE_MS)
      {
        engine_speed_state = ENGINE_SPEED_TRIGGER_ACTIVE;
      }
      break;
    case ENGINE_SPEED_TRIGGER_ACTIVE:
      if (elapsed >= ENGINE_TRIGGER_ACTIVE_MS)
      {
        /* Release K24/K25 before reconnecting the OEM signal with K3. */
        engine_speed_state = ENGINE_SPEED_RESTORE_SETTLE;
      }
      break;
    case ENGINE_SPEED_RESTORE_SETTLE:
      if (elapsed >= ENGINE_RESTORE_SETTLE_MS)
      {
        if (engine_speed_remaining > 0) { --engine_speed_remaining; }
        else if (engine_speed_remaining < 0) { ++engine_speed_remaining; }
        engine_speed_state = (engine_speed_remaining == 0) ?
                             ENGINE_SPEED_IDLE : ENGINE_SPEED_INTER_TRIGGER;
      }
      break;
    case ENGINE_SPEED_INTER_TRIGGER:
      if (elapsed >= ENGINE_INTER_TRIGGER_MS)
      {
        engine_speed_state = ENGINE_SPEED_TAKEOVER_SETTLE;
      }
      break;
    case ENGINE_SPEED_IDLE:
    default:
      break;
  }

  if (engine_speed_state != old_state)
  {
    engine_state_tick = secure_uptime_ms;
    return true;
  }
  return false;
}

static void Safety_OneMillisecondTick(void)
{
  uint32_t primask = Safety_EnterCritical();
  bool relay_change = false;

  ++secure_uptime_ms;
  if (HAL_GPIO_ReadPin(ESTOP_DETECT_GPIO_Port, ESTOP_DETECT_Pin) == GPIO_PIN_SET)
  {
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED;
    Safety_ForceOutputsSafe();
    Safety_ExitCritical(primask);
    return;
  }

  if (((safety_status & SAFETY_STATUS_ADC_RUNNING) != 0U) &&
      ((secure_uptime_ms - last_current_tick) > 2U))
  {
    safety_status |= SAFETY_STATUS_VALVE_SENSOR_FAULT |
                     SAFETY_STATUS_FAULT_LATCHED;
    valve_control.fault_flags |= SAFETY_STATUS_VALVE_SENSOR_FAULT;
    Safety_ForceOutputsSafe();
    Safety_ExitCritical(primask);
    return;
  }

  if (((safety_status & SAFETY_STATUS_OUTPUTS_ARMED) != 0U) &&
      (command_fresh != 0U) &&
      ((secure_uptime_ms - last_command_tick) > SAFETY_COMMAND_TIMEOUT_MS))
  {
    safety_status |= SAFETY_STATUS_COMMAND_TIMEOUT;
    Safety_ForceOutputsSafe();
    Safety_ExitCritical(primask);
    return;
  }

  if ((engine_start_active != 0U) &&
      Safety_TimeReached(secure_uptime_ms, engine_start_deadline))
  {
    engine_start_active = 0U;
    engine_start_lockout = 1U;
    safety_status |= SAFETY_STATUS_START_LIMIT;
    relay_change = true;
  }
  relay_change = Safety_AdvanceEngineSpeed() || relay_change;

  if (relay_change && ((safety_status & SAFETY_STATUS_OUTPUTS_ARMED) != 0U))
  {
    uint32_t dynamic_mask = Safety_ApplyDynamicRelayBits(requested_relay_mask);
    if (!Safety_TpicShift(dynamic_mask))
    {
      safety_status |= SAFETY_STATUS_TPIC_ERROR | SAFETY_STATUS_FAULT_LATCHED;
      Safety_ForceOutputsSafe();
    }
    else
    {
      applied_relay_mask = dynamic_mask;
    }
  }
  Safety_ExitCritical(primask);
}

int32_t Safety_ServiceInit(void)
{
  ADC_AnalogWDGConfTypeDef valve_watchdog = {0};
  SAFETY_ValveConfig initial_config;
  uint32_t persisted_generation = 0U;
  uint32_t persisted_crc = 0U;
  bool persisted_valid;
  uint32_t primask;

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  timer_ready = 1U;
  primask = Safety_EnterCritical();
  safety_status = 0U;
  secure_uptime_ms = 0U;
  valve_telemetry_write_sequence = 0U;
  valve_telemetry_read_sequence = 0U;
  valve_telemetry_dropped = 0U;
  last_current_tick = 0U;
  last_config_save_tick = UINT32_MAX - VALVE_CONFIG_SAVE_INTERVAL_MS;
  persisted_valid = ValveConfigStore_Load(
      &initial_config, &persisted_generation, &persisted_crc) &&
      ValveControl_ValidateConfig(&initial_config);
  if (!persisted_valid)
  {
    ValveControl_DefaultConfig(&initial_config);
    safety_status |= SAFETY_STATUS_VALVE_CONFIG_DEFAULTED;
  }
  ValveControl_Init(&valve_control, &initial_config);
  memset(&valve_config_snapshot, 0, sizeof(valve_config_snapshot));
  valve_config_snapshot.config = initial_config;
  valve_config_snapshot.active_revision = 1U;
  valve_config_snapshot.persisted_generation = persisted_generation;
  valve_config_snapshot.persisted_crc32c = persisted_crc;
  valve_config_snapshot.persisted_valid = persisted_valid ? 1U : 0U;
  valve_config_snapshot.using_defaults = persisted_valid ? 0U : 1U;
  Safety_ForceOutputsSafe();
  if (HAL_GPIO_ReadPin(ESTOP_DETECT_GPIO_Port, ESTOP_DETECT_Pin) == GPIO_PIN_SET)
  {
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED;
  }
  Safety_ExitCritical(primask);

  software_i2c_bus_ok = SoftwareI2C_Init() ? 1U : 0U;
  if (software_i2c_bus_ok == 0U)
  {
    /* ATECC608C support is intentionally deferred. A stuck temporary software
       bus is reported but does not prevent the vehicle safety functions. */
    safety_status |= SAFETY_STATUS_SW_I2C_BUS_FAULT;
  }

  if ((HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK) ||
      (HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED) != HAL_OK))
  {
    Safety_LatchFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  valve_watchdog.WatchdogNumber = ADC_ANALOGWATCHDOG_1;
  valve_watchdog.WatchdogMode = ADC_ANALOGWATCHDOG_ALL_REG;
  valve_watchdog.Channel = ADC_CHANNEL_18;
  valve_watchdog.ITMode = ENABLE;
  valve_watchdog.HighThreshold = 3102U;
  valve_watchdog.LowThreshold = 0U;
  valve_watchdog.FilteringConfig = ADC_AWD_FILTERING_NONE;
  if (HAL_ADC_AnalogWDGConfig(&hadc2, &valve_watchdog) != HAL_OK)
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

  if ((HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1) != HAL_OK) ||
      (HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_2) != HAL_OK) ||
      (HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_4) != HAL_OK) ||
      (HAL_TIM_Base_Start_IT(&htim6) != HAL_OK))
  {
    Safety_LatchFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }

  safety_status |= SAFETY_STATUS_READY | SAFETY_STATUS_ADC_RUNNING |
                   SAFETY_STATUS_VALVE_CALIBRATING;
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

int32_t Safety_GetActuatorSnapshot(SAFETY_ActuatorSnapshot *snapshot)
{
  uint32_t primask;
  uint32_t remaining = 0U;

  if (snapshot == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  primask = Safety_EnterCritical();
  if ((engine_start_active != 0U) &&
      !Safety_TimeReached(secure_uptime_ms, engine_start_deadline))
  {
    remaining = engine_start_deadline - secure_uptime_ms;
  }
  snapshot->status = safety_status;
  snapshot->requested_relay_mask = requested_relay_mask;
  snapshot->applied_relay_mask = applied_relay_mask;
  snapshot->last_command_sequence = last_command_sequence;
  snapshot->secure_uptime_ms = secure_uptime_ms;
  snapshot->command_age_ms = (command_fresh != 0U) ?
                             secure_uptime_ms - last_command_tick : UINT32_MAX;
  snapshot->valve_requested_target_ma = valve_control.requested_target_ma;
  snapshot->valve_applied_target_ma =
      ValveControl_AppliedTargetMa(&valve_control);
  snapshot->forward_current_ma = valve_control.forward_current_ma;
  snapshot->reverse_current_ma = valve_control.reverse_current_ma;
  snapshot->forward_duty_permille = valve_control.forward_duty_permille;
  snapshot->reverse_duty_permille = valve_control.reverse_duty_permille;
  snapshot->engine_start_remaining_ms = (uint16_t)remaining;
  snapshot->engine_speed_level = engine_speed_level;
  snapshot->engine_speed_remaining = engine_speed_remaining;
  snapshot->engine_speed_state = (uint8_t)engine_speed_state;
  snapshot->physical_estop_active =
      (HAL_GPIO_ReadPin(ESTOP_DETECT_GPIO_Port, ESTOP_DETECT_Pin) == GPIO_PIN_SET) ? 1U : 0U;
  snapshot->software_i2c_bus_ok = software_i2c_bus_ok;
  snapshot->valve_state = (uint8_t)valve_control.state;
  snapshot->valve_config_dirty = valve_config_snapshot.dirty;
  snapshot->valve_active_revision = valve_config_snapshot.active_revision;
  snapshot->valve_persisted_generation =
      valve_config_snapshot.persisted_generation;
  snapshot->valve_fault_flags = valve_control.fault_flags;
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

int32_t Safety_GetValveConfig(SAFETY_ValveConfigSnapshot *snapshot)
{
  uint32_t primask;

  if (snapshot == NULL) { return SAFETY_RESULT_BAD_ARGUMENT; }
  primask = Safety_EnterCritical();
  *snapshot = valve_config_snapshot;
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

int32_t Safety_ApplyValveConfig(const SAFETY_ValveConfig *config)
{
  uint32_t primask;
  int32_t result;

  if (!ValveControl_ValidateConfig(config)) { return SAFETY_RESULT_RANGE; }
  primask = Safety_EnterCritical();
  result = ValveControl_ApplyConfig(&valve_control, config);
  if (result == SAFETY_RESULT_OK)
  {
    valve_config_snapshot.config = *config;
    ++valve_config_snapshot.active_revision;
    valve_config_snapshot.dirty = 1U;
    valve_config_snapshot.using_defaults = 0U;
  }
  Safety_ExitCritical(primask);
  return result;
}

int32_t Safety_SaveValveConfig(void)
{
  SAFETY_ValveConfig config;
  uint32_t generation;
  uint32_t crc32c;
  uint32_t primask;

  primask = Safety_EnterCritical();
  if (((safety_status & SAFETY_STATUS_OUTPUTS_ARMED) != 0U) ||
      !ValveControl_IsIdle(&valve_control) || (command_fresh != 0U))
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_BUSY;
  }
  if ((secure_uptime_ms - last_config_save_tick) <
      VALVE_CONFIG_SAVE_INTERVAL_MS)
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_RATE_LIMITED;
  }
  if ((valve_config_snapshot.dirty == 0U) &&
      (valve_config_snapshot.persisted_valid != 0U))
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_OK;
  }
  config = valve_config_snapshot.config;
  generation = valve_config_snapshot.persisted_generation + 1U;
  Safety_ExitCritical(primask);

  (void)HAL_IWDG_Refresh(&hiwdg);
  if (!ValveConfigStore_Save(&config, generation, &crc32c))
  {
    Safety_LatchFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  (void)HAL_IWDG_Refresh(&hiwdg);

  primask = Safety_EnterCritical();
  valve_config_snapshot.persisted_generation = generation;
  valve_config_snapshot.persisted_crc32c = crc32c;
  valve_config_snapshot.persisted_valid = 1U;
  valve_config_snapshot.dirty = 0U;
  valve_config_snapshot.using_defaults = 0U;
  last_config_save_tick = secure_uptime_ms;
  safety_status &= ~SAFETY_STATUS_VALVE_CONFIG_DEFAULTED;
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

int32_t Safety_ReloadValveConfig(void)
{
  SAFETY_ValveConfig config;
  uint32_t generation;
  uint32_t crc32c;
  uint32_t primask;
  int32_t result;

  if (!ValveConfigStore_Load(&config, &generation, &crc32c) ||
      !ValveControl_ValidateConfig(&config))
  {
    return SAFETY_RESULT_NOT_PERSISTED;
  }
  primask = Safety_EnterCritical();
  result = ValveControl_ApplyConfig(&valve_control, &config);
  if (result == SAFETY_RESULT_OK)
  {
    valve_config_snapshot.config = config;
    ++valve_config_snapshot.active_revision;
    valve_config_snapshot.persisted_generation = generation;
    valve_config_snapshot.persisted_crc32c = crc32c;
    valve_config_snapshot.persisted_valid = 1U;
    valve_config_snapshot.dirty = 0U;
    valve_config_snapshot.using_defaults = 0U;
    safety_status &= ~SAFETY_STATUS_VALVE_CONFIG_DEFAULTED;
  }
  Safety_ExitCritical(primask);
  return result;
}

int32_t Safety_ReadValveTelemetry(SAFETY_ValveTelemetryBatch *batch)
{
  uint32_t primask;
  uint32_t available;
  uint32_t index;

  if (batch == NULL) { return SAFETY_RESULT_BAD_ARGUMENT; }
  primask = Safety_EnterCritical();
  available = valve_telemetry_write_sequence - valve_telemetry_read_sequence;
  if (available > SAFETY_VALVE_TELEMETRY_BATCH_MAX)
  {
    available = SAFETY_VALVE_TELEMETRY_BATCH_MAX;
  }
  batch->first_sequence = valve_telemetry_read_sequence;
  batch->dropped_samples = valve_telemetry_dropped;
  batch->sample_count = (uint16_t)available;
  batch->sample_period_us = 1000U;
  for (index = 0U; index < available; ++index)
  {
    batch->samples[index] = valve_telemetry[
        (valve_telemetry_read_sequence + index) &
        (VALVE_TELEMETRY_RING_SIZE - 1U)];
  }
  for (; index < SAFETY_VALVE_TELEMETRY_BATCH_MAX; ++index)
  {
    memset(&batch->samples[index], 0, sizeof(batch->samples[index]));
  }
  valve_telemetry_read_sequence += available;
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
                       SAFETY_STATUS_INTERNAL_ERROR |
                       SAFETY_STATUS_TPIC_ERROR)) != 0U)
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  Safety_ForceOutputsSafe();
  if ((valve_control.forward_current_ma >= SAFETY_VALVE_TARGET_DEADBAND_MA) ||
      (valve_control.reverse_current_ma >= SAFETY_VALVE_TARGET_DEADBAND_MA))
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_BUSY;
  }
  valve_control.fault_flags = 0U;
  ValveControl_ForceSafe(&valve_control);
  safety_status &= ~(SAFETY_STATUS_ESTOP_ACTIVE |
                     SAFETY_STATUS_FAULT_LATCHED |
                     SAFETY_STATUS_COMMAND_TIMEOUT |
                     SAFETY_STATUS_COMMAND_REJECTED |
                     SAFETY_STATUS_START_LIMIT |
                     SAFETY_STATUS_VALVE_OVERCURRENT |
                     SAFETY_STATUS_VALVE_OPEN_LOAD |
                     SAFETY_STATUS_VALVE_SENSOR_FAULT |
                     SAFETY_STATUS_VALVE_DIRECTION_FAULT |
                     SAFETY_STATUS_VALVE_LIMITED);
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
  if (((safety_status & SAFETY_STATUS_READY) == 0U) ||
      (valve_control.calibrated == 0U))
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

  Safety_ForceOutputsSafe();
  /* Keep TPIC OE_N high while reconnecting the AHCT541.  CLR_N is still low
     at this point, so enabling the buffer cannot expose an old shift value.
     Release CLR_N, explicitly shift/latch 32 zeroes, and only then expose the
     TPIC outputs.  A latch pulse issued while the buffer is disabled would
     never reach the TPIC and could restore a stale storage-register value. */
  GPIOE->BSRR = TPIC_CTRL_BUF_EN_Pin;
  Safety_TpicDelayUs(TPIC_LATCH_PULSE_US);
  GPIOE->BSRR = TPIC_CLR_N_Pin;
  if (!Safety_TpicShift(0U))
  {
    safety_status |= SAFETY_STATUS_TPIC_ERROR | SAFETY_STATUS_FAULT_LATCHED;
    Safety_ForceOutputsSafe();
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_TPIC_ERROR;
  }
  GPIOE->BSRR = (uint32_t)TPIC_OE_N_Pin << 16U;
  safety_status &= ~(SAFETY_STATUS_COMMAND_TIMEOUT |
                     SAFETY_STATUS_COMMAND_REJECTED |
                     SAFETY_STATUS_START_LIMIT);
  safety_status |= SAFETY_STATUS_OUTPUTS_ARMED;
  command_fresh = 1U;
  last_command_tick = secure_uptime_ms;
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

int32_t Safety_DisarmOutputs(void)
{
  uint32_t primask = Safety_EnterCritical();
  Safety_ForceOutputsSafe();
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

int32_t Safety_SubmitActuatorCommand(const SAFETY_ActuatorCommand *command)
{
  uint32_t primask;
  uint32_t new_mask;
  int32_t result = Safety_ValidateCommand(command);

  if (result != SAFETY_RESULT_OK)
  {
    primask = Safety_EnterCritical();
    safety_status |= SAFETY_STATUS_COMMAND_REJECTED;
    Safety_ExitCritical(primask);
    return result;
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
      ((int32_t)(command->sequence - last_command_sequence) <= 0))
  {
    safety_status |= SAFETY_STATUS_COMMAND_REJECTED;
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_STALE_SEQUENCE;
  }

  Safety_ApplyEngineRequest(command->engine_speed_level);
  Safety_ApplyStartRequest(command->engine_start_request);
  new_mask = Safety_BuildBaseRelayMask(command);
  requested_relay_mask = new_mask;
  new_mask = Safety_ApplyDynamicRelayBits(new_mask);

  if (!Safety_TpicShift(new_mask))
  {
    safety_status |= SAFETY_STATUS_TPIC_ERROR | SAFETY_STATUS_FAULT_LATCHED;
    Safety_ForceOutputsSafe();
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_TPIC_ERROR;
  }

  result = ValveControl_SetTarget(&valve_control,
                                  command->valve_current_target_ma);
  if (result != SAFETY_RESULT_OK)
  {
    safety_status |= SAFETY_STATUS_COMMAND_REJECTED;
    Safety_ForceOutputsSafe();
    Safety_ExitCritical(primask);
    return result;
  }
  applied_relay_mask = new_mask;
  last_command_sequence = command->sequence;
  command_sequence_valid = 1U;
  command_fresh = 1U;
  last_command_tick = secure_uptime_ms;
  safety_status &= ~(SAFETY_STATUS_COMMAND_TIMEOUT |
                     SAFETY_STATUS_COMMAND_REJECTED);
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

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM6)
  {
    Safety_OneMillisecondTick();
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
    uint32_t vdda_mv = 3300U;
    bool outputs_allowed;

    for (index = 0U; index < SAFETY_CURRENT_ADC_COUNT; ++index)
    {
      current_adc_snapshot[index] = current_adc_dma[index];
    }
    __DMB();
    ++current_sequence;
    last_current_tick = secure_uptime_ms;
    if (slow_adc_snapshot[ADC_VREFINT_INDEX] != 0U)
    {
      vdda_mv = __HAL_ADC_CALC_VREFANALOG_VOLTAGE(
          slow_adc_snapshot[ADC_VREFINT_INDEX], ADC_RESOLUTION_12B);
    }
    outputs_allowed =
        ((safety_status & SAFETY_STATUS_OUTPUTS_ARMED) != 0U) &&
        ((safety_status & SAFETY_STATUS_FAULT_LATCHED) == 0U);
    ValveControl_Step(&valve_control, current_adc_dma[0], current_adc_dma[1],
                      (uint16_t)vdda_mv, outputs_allowed);
    TIM4->CCR1 = ValveControl_ForwardCompare(&valve_control);
    TIM4->CCR2 = ValveControl_ReverseCompare(&valve_control);
    if (valve_control.calibrated != 0U)
    {
      safety_status &= ~SAFETY_STATUS_VALVE_CALIBRATING;
    }
    else
    {
      safety_status |= SAFETY_STATUS_VALVE_CALIBRATING;
    }
    if (valve_control.limited != 0U)
    {
      safety_status |= SAFETY_STATUS_VALVE_LIMITED;
    }
    else
    {
      safety_status &= ~SAFETY_STATUS_VALVE_LIMITED;
    }
    Safety_RecordValveTelemetry();
    if (valve_control.fault_flags != 0U)
    {
      safety_status |= SAFETY_STATUS_FAULT_LATCHED |
                       valve_control.fault_flags;
      Safety_ForceOutputsSafe();
    }
  }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  (void)hadc;
  Safety_LatchFault(SAFETY_STATUS_INTERNAL_ERROR);
}

void HAL_ADC_LevelOutOfWindowCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC2)
  {
    valve_control.fault_flags |= SAFETY_STATUS_VALVE_OVERCURRENT;
    Safety_LatchFault(SAFETY_STATUS_VALVE_OVERCURRENT);
  }
}

void HAL_GTZC_TZIC_Callback(uint32_t periph_id)
{
  (void)periph_id;
  Safety_LatchFault(SAFETY_STATUS_GTZC_VIOLATION);
}
