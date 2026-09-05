#include "safety_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "adc.h"
#include "atecc608.h"
#include "iwdg.h"
#include "main.h"
#include "run_permit_policy.h"
#include "security_identity.h"
#include "security_mcu_identity.h"
#include "security_ota.h"
#include "security_factory.h"
#include "secure_timebase.h"
#include "software_i2c.h"
#include "spi.h"
#include "steering_can.h"
#include "steering_control.h"
#include "tim.h"
#include "valve_config_store.h"
#include "valve_control.h"
#include "vehicle_can.h"

#define TPIC_SPI_TIMEOUT_US          1000UL
#define TPIC_LATCH_PULSE_US             1UL
#define ENGINE_TAKEOVER_SETTLE_MS       50UL
#define ENGINE_TRIGGER_ACTIVE_MS      1000UL
#define ENGINE_RESTORE_SETTLE_MS        50UL
#define ENGINE_INTER_TRIGGER_MS      2000UL
#define VALVE_TELEMETRY_RING_SIZE     128U
#define VALVE_CONFIG_SAVE_INTERVAL_MS 10000UL
#define OTA_CONFIRM_WATCHDOG_GRACE_MS  5000UL
#define ESTOP_RELEASE_DEBOUNCE_MS         20U
#define ADC_VREFINT_INDEX                7U

_Static_assert(sizeof(SAFETY_ActuatorCommand) == 36U,
               "SAFETY_ActuatorCommand ABI must remain 36 bytes");
_Static_assert(offsetof(SAFETY_ActuatorCommand, steering_enable) == 31U,
               "steering_enable ABI offset changed");
_Static_assert(offsetof(SAFETY_ActuatorCommand, steering_source) == 32U,
               "steering_source ABI offset changed");
_Static_assert(offsetof(SAFETY_ActuatorCommand, steering_flags) == 33U,
               "steering_flags ABI offset changed");
_Static_assert(offsetof(SAFETY_ActuatorCommand,
                        steering_velocity_tdeg_per_s) == 34U,
               "steering_velocity ABI offset changed");
_Static_assert(sizeof(SAFETY_ActuatorSnapshot) == 60U,
               "frozen actuator snapshot ABI changed");
_Static_assert(sizeof(SAFETY_SteeringSnapshot) == 72U,
               "steering snapshot ABI changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot,
                        requested_velocity_tdeg_per_s) == 56U,
               "steering requested velocity ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot,
                        applied_velocity_tdeg_per_s) == 58U,
               "steering applied velocity ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot,
                        speed_command_permille) == 60U,
               "steering speed command ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot,
                        motor_speed_feedback_raw) == 62U,
               "steering motor feedback ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot,
                        motor_fault_code) == 64U,
               "steering motor fault ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot, source) == 66U,
               "steering source ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot, state) == 67U,
               "steering state ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot, bus_state) == 68U,
               "steering bus state ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot, command_enable) == 69U,
               "steering command enable ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot,
                        motor_enable_confirmed) == 70U,
               "steering enable confirmation ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot, reserved) == 71U,
               "steering reserved tail ABI offset changed");
_Static_assert(sizeof(SAFETY_J1939Snapshot) == 84U,
               "J1939 snapshot ABI changed");

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
static SAFETY_SecurityStatus security_status;
static uint8_t engine_start_active;
static uint8_t engine_start_lockout;
static uint8_t last_engine_start_request;
static int8_t previous_engine_speed_request;
static uint8_t ota_running_images_confirmed;
static uint8_t ota_confirmation_deadline_armed;
static uint32_t ota_confirmation_deadline;
static uint8_t tpic_outputs_enabled;
static uint8_t run_permit_restore_pending;
static uint16_t physical_estop_inactive_ms;

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

static void Safety_SetOtaConfirmationState(uint32_t confirmed)
{
  uint32_t primask = Safety_EnterCritical();

  ota_running_images_confirmed = (confirmed != 0U) ? 1U : 0U;
  if (ota_running_images_confirmed != 0U)
  {
    safety_status &= ~SAFETY_STATUS_OTA_UNCONFIRMED;
  }
  else
  {
    safety_status |= SAFETY_STATUS_OTA_UNCONFIRMED;
  }
  Safety_ExitCritical(primask);
}

static bool Safety_TimeReached(uint32_t now, uint32_t deadline)
{
  return (int32_t)(now - deadline) >= 0;
}

static bool Safety_TpicWaitFlag(uint32_t flag)
{
  bool elapsed;
  uint16_t start = SecureTimebase_NowUs16();

  if (!SecureTimebase_IsRunning())
  {
    return false;
  }
  while ((SPI4->SR & flag) == 0U)
  {
    if (!SecureTimebase_HasElapsed(start, TPIC_SPI_TIMEOUT_US, &elapsed) ||
        elapsed)
    {
      return false;
    }
  }
  return true;
}

static bool Safety_TpicDelayUs(uint32_t microseconds)
{
  return SecureTimebase_DelayUs(microseconds);
}

static bool Safety_TpicLatch(void)
{
  GPIOE->BSRR = TPIC_RCK_Pin;
  if (!Safety_TpicDelayUs(TPIC_LATCH_PULSE_US))
  {
    GPIOE->BSRR = (uint32_t)TPIC_RCK_Pin << 16U;
    return false;
  }
  GPIOE->BSRR = (uint32_t)TPIC_RCK_Pin << 16U;
  __DSB();
  return true;
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
  return Safety_TpicLatch();
}

static void Safety_ResetCommandState(void)
{
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
  SteeringCan_RequestSafe(secure_uptime_ms);
}

static bool Safety_PhysicalEStopActive(void)
{
  return HAL_GPIO_ReadPin(ESTOP_DETECT_GPIO_Port,
                          ESTOP_DETECT_Pin) == GPIO_PIN_SET;
}

static bool Safety_RunPermitCanClose(void)
{
  return RunPermitPolicy_CanClose(
      safety_status, security_status.flags, Safety_PhysicalEStopActive(),
      ota_running_images_confirmed != 0U);
}

static void Safety_StopControlledOutputs(void)
{
  if (timer_ready != 0U)
  {
    TIM4->CCR1 = 0U;
    TIM4->CCR2 = 0U;
  }

  Safety_ResetCommandState();
  safety_status &= ~SAFETY_STATUS_OUTPUTS_ARMED;
}

static void Safety_HardDisableOutputs(void)
{
  Safety_StopControlledOutputs();

  /* OE_N high immediately removes relay drive. CLR_N and the AHCT541 buffer
     enable low then make the stored command safe even if NonSecure stalls. */
  GPIOE->BSRR = TPIC_OE_N_Pin;
  GPIOE->BSRR = (uint32_t)(TPIC_CLR_N_Pin | TPIC_CTRL_BUF_EN_Pin |
                           TPIC_RCK_Pin) << 16U;
  requested_relay_mask = 0U;
  applied_relay_mask = 0U;
  tpic_outputs_enabled = 0U;
}

static bool Safety_EnableTpicBaseline(uint32_t baseline_mask)
{
  /* OE_N remains high until the buffer, clear line, shift register and storage
     register all contain the reviewed baseline. This prevents a stale TPIC
     register from appearing at a relay during startup or physical-E-stop
     recovery. */
  GPIOE->BSRR = TPIC_OE_N_Pin;
  GPIOE->BSRR = (uint32_t)(TPIC_CLR_N_Pin | TPIC_CTRL_BUF_EN_Pin |
                           TPIC_RCK_Pin) << 16U;
  GPIOE->BSRR = TPIC_CTRL_BUF_EN_Pin;
  if (!Safety_TpicDelayUs(TPIC_LATCH_PULSE_US))
  {
    return false;
  }
  GPIOE->BSRR = TPIC_CLR_N_Pin;
  if (!Safety_TpicShift(baseline_mask))
  {
    return false;
  }
  GPIOE->BSRR = (uint32_t)TPIC_OE_N_Pin << 16U;
  tpic_outputs_enabled = 1U;
  return true;
}

static bool Safety_ApplyRunPermitBaseline(void)
{
  const uint32_t baseline_mask = SAFETY_RELAY_ESTOP_RUN_PERMIT;

  Safety_StopControlledOutputs();
  if (!Safety_RunPermitCanClose())
  {
    run_permit_restore_pending = 0U;
    Safety_HardDisableOutputs();
    return true;
  }

  if (((tpic_outputs_enabled != 0U) &&
       !Safety_TpicShift(baseline_mask)) ||
      ((tpic_outputs_enabled == 0U) &&
       !Safety_EnableTpicBaseline(baseline_mask)))
  {
    safety_status |= SAFETY_STATUS_TPIC_ERROR |
                     SAFETY_STATUS_FAULT_LATCHED;
    Safety_HardDisableOutputs();
    return false;
  }
  requested_relay_mask = baseline_mask;
  applied_relay_mask = baseline_mask;
  run_permit_restore_pending = 0U;
  return true;
}

static void Safety_QuiesceControlledOutputs(void)
{
  (void)Safety_ApplyRunPermitBaseline();
}

static void Safety_LatchCriticalFault(uint32_t reason)
{
  uint32_t primask = Safety_EnterCritical();
  safety_status |= SAFETY_STATUS_FAULT_LATCHED | reason;
  Safety_HardDisableOutputs();
  Safety_ExitCritical(primask);
}

static void Safety_LatchIsolatedFault(uint32_t reason)
{
  uint32_t primask = Safety_EnterCritical();
  safety_status |= SAFETY_STATUS_FAULT_LATCHED | reason;
  Safety_QuiesceControlledOutputs();
  Safety_ExitCritical(primask);
}

static bool Safety_RefreshStartupWatchdog(void)
{
  if (HAL_IWDG_Refresh(&hiwdg) == HAL_OK)
  {
    return true;
  }
  Safety_LatchCriticalFault(SAFETY_STATUS_INTERNAL_ERROR);
  return false;
}

static bool Safety_AteccRecoveryService(void *context)
{
  (void)context;
  /* The IWDG is already running.  The driver invokes this before every short
     recovery slice while allowing one bounded pre-reset ATECC execution. */
  return Safety_RefreshStartupWatchdog();
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

static bool Safety_SteeringCommandValuesValid(
    const SAFETY_ActuatorCommand *command)
{
  return (command->steering_source >=
          (uint8_t)SAFETY_STEERING_SOURCE_REMOTE) &&
         (command->steering_source <=
          (uint8_t)SAFETY_STEERING_SOURCE_AUTONOMOUS) &&
         SteeringControl_CommandValuesValid(
             command->steering_velocity_tdeg_per_s,
             command->steering_enable, command->steering_flags,
             command->run_permit_on);
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
       settling interval. Fault/timeout paths quiesce all controlled outputs;
       only the independently managed K12 manual-drive baseline may remain. */
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
  uint32_t primask;
  bool relay_change = false;
  bool steering_active_fault;

  ++secure_uptime_ms;
  /* FDCAN polling and bounded nonblocking register writes run outside the
     global safety critical section. ADC/EXTI fault paths only post an atomic
     steering-safe request, consumed here by the single state-machine owner. */
  SteeringCan_Process(secure_uptime_ms);
  /* Vehicle CAN is passive telemetry and always runs after the safety-critical
     steering state machine. Its parser and FIFO drain both have fixed budgets. */
  VehicleCan_Process(secure_uptime_ms);
  steering_active_fault = SteeringCan_ConsumeActiveFault();

  primask = Safety_EnterCritical();
  if (VehicleCan_IsHealthy())
  {
    safety_status &= ~SAFETY_STATUS_VEHICLE_CAN_FAULT;
  }
  else
  {
    /* CAN1 telemetry health is a live, isolated diagnostic. It deliberately
       neither latches the global fault nor forces unrelated outputs safe. */
    safety_status |= SAFETY_STATUS_VEHICLE_CAN_FAULT;
  }
  if (steering_active_fault)
  {
    /* A steering transport fault is isolated to that actuator. The steering
       service has already scheduled zero-then-disable; unrelated vehicle
       relays and valve control remain under the normal Secure watchdog. */
    safety_status |= SAFETY_STATUS_STEERING_CAN_FAULT;
  }
  if (Safety_PhysicalEStopActive())
  {
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED;
    physical_estop_inactive_ms = 0U;
    run_permit_restore_pending = 1U;
    Safety_HardDisableOutputs();
    Safety_ExitCritical(primask);
    return;
  }
  safety_status &= ~SAFETY_STATUS_ESTOP_ACTIVE;
  if (run_permit_restore_pending != 0U)
  {
    if (physical_estop_inactive_ms < ESTOP_RELEASE_DEBOUNCE_MS)
    {
      ++physical_estop_inactive_ms;
    }
    if ((physical_estop_inactive_ms >= ESTOP_RELEASE_DEBOUNCE_MS) &&
        !Safety_ApplyRunPermitBaseline())
    {
      Safety_ExitCritical(primask);
      return;
    }
  }

  if (((safety_status & SAFETY_STATUS_ADC_RUNNING) != 0U) &&
      ((secure_uptime_ms - last_current_tick) > 2U))
  {
    safety_status |= SAFETY_STATUS_VALVE_SENSOR_FAULT |
                     SAFETY_STATUS_FAULT_LATCHED;
    valve_control.fault_flags |= SAFETY_STATUS_VALVE_SENSOR_FAULT;
    Safety_QuiesceControlledOutputs();
    Safety_ExitCritical(primask);
    return;
  }

  if (((safety_status & SAFETY_STATUS_OUTPUTS_ARMED) != 0U) &&
      (command_fresh != 0U) &&
      ((secure_uptime_ms - last_command_tick) > SAFETY_COMMAND_TIMEOUT_MS))
  {
    safety_status |= SAFETY_STATUS_COMMAND_TIMEOUT;
    Safety_QuiesceControlledOutputs();
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
      Safety_HardDisableOutputs();
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
  uint32_t running_images_confirmed;

  if (!SecureTimebase_IsRunning())
  {
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  timer_ready = 1U;
  primask = Safety_EnterCritical();
  safety_status = 0U;
  secure_uptime_ms = 0U;
  tpic_outputs_enabled = 0U;
  run_permit_restore_pending = 0U;
  physical_estop_inactive_ms = 0U;
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
  Safety_HardDisableOutputs();
  if (VehicleCan_Init(secure_uptime_ms) != SAFETY_RESULT_OK)
  {
    /* CAN1 is read-only vehicle telemetry. Keep Ethernet/OTA and unrelated
       actuators available while publishing the isolated diagnostic fault. */
    safety_status |= SAFETY_STATUS_VEHICLE_CAN_FAULT;
  }
  if (SteeringCan_Init(secure_uptime_ms) != SAFETY_RESULT_OK)
  {
    /* CAN2 steering is an isolated actuator domain. A missing/unavailable
       motor or local controller startup failure keeps steering fail-safe and
       visible in diagnostics without preventing Ethernet or other outputs. */
    safety_status |= SAFETY_STATUS_STEERING_CAN_FAULT;
  }
  if (Safety_PhysicalEStopActive())
  {
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED;
  }
  Safety_ExitCritical(primask);

  software_i2c_bus_ok = SoftwareI2C_Init() ? 1U : 0U;
  if (software_i2c_bus_ok == 0U)
  {
    /* The PCB-revision-1 software-I2C bus is part of the startup identity
       check. Any electrical bus fault keeps every actuator quarantined. */
    safety_status |= SAFETY_STATUS_SW_I2C_BUS_FAULT;
  }
  memset(&security_status, 0, sizeof(security_status));
  security_status.api_version = SAFETY_SECURITY_API_VERSION;
  security_status.flags = SAFETY_SECURITY_READ_ONLY_PROBE |
                          SAFETY_SECURITY_QUARANTINE;
  security_status.auth_result = SECURITY_IDENTITY_ATECC_UNAVAILABLE;
  (void)SecurityMcuIdentity_Get(security_status.mcu_uid);
  if (software_i2c_bus_ok != 0U)
  {
    ATECC608_ProbeResult probe;
    security_status.atecc_result = ATECC608_ProbeWithRecovery(
        &probe, Safety_AteccRecoveryService, NULL);
    if (security_status.atecc_result == ATECC608_RESULT_OK)
    {
      security_status.flags |= SAFETY_SECURITY_ATECC_PRESENT;
      security_status.config_crc32c = probe.config_crc32c;
      memcpy(security_status.serial, probe.serial,
             sizeof(security_status.serial));
      memcpy(security_status.revision, probe.revision,
             sizeof(security_status.revision));
      security_status.i2c_address = probe.i2c_address;
      security_status.config_locked = probe.config_locked;
      security_status.data_locked = probe.data_locked;
      security_status.device_status = probe.device_status;
      if (probe.config_locked != 0U)
      {
        security_status.flags |= SAFETY_SECURITY_CONFIG_LOCKED;
      }
      if (probe.data_locked != 0U)
      {
        security_status.flags |= SAFETY_SECURITY_DATA_LOCKED;
      }
      if (Safety_RefreshStartupWatchdog())
      {
        security_status.auth_result = SecurityIdentity_Authenticate(
            &probe, &security_status.device_status,
            &security_status.pairing_generation);
        (void)Safety_RefreshStartupWatchdog();
      }
      if (security_status.pairing_generation != 0U)
      {
        security_status.flags |= SAFETY_SECURITY_PAIRING_PRESENT;
      }
      if (security_status.auth_result == SECURITY_IDENTITY_OK)
      {
        security_status.flags |= SAFETY_SECURITY_PAIRING_PRESENT |
                                 SAFETY_SECURITY_AUTHENTICATED;
        security_status.flags &= ~SAFETY_SECURITY_QUARANTINE;
        safety_status |= SAFETY_STATUS_ATECC_AUTHENTICATED;
      }
      else if (security_status.auth_result ==
               SECURITY_IDENTITY_NOT_PROVISIONED)
      {
        safety_status |= SAFETY_STATUS_ATECC_UNPAIRED;
      }
      else
      {
        safety_status |= SAFETY_STATUS_ATECC_AUTH_FAILED;
      }
    }
    else
    {
      safety_status |= SAFETY_STATUS_ATECC_MISSING;
    }
  }
  else
  {
    security_status.atecc_result = ATECC608_RESULT_BUS;
    safety_status |= SAFETY_STATUS_ATECC_MISSING;
  }
  SecurityOta_Init();
  if (SecurityOta_RunningImagesConfirmed(&running_images_confirmed) !=
      SAFETY_RESULT_OK)
  {
    Safety_LatchCriticalFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  Safety_SetOtaConfirmationState(running_images_confirmed);
  /* Start the confirmation budget from Secure service initialization, not
     from a NonSecure-selected first watchdog call. secure_uptime_ms begins
     advancing as soon as TIM6 is started below, so NonSecure cannot defer or
     renew this deadline. */
  ota_confirmation_deadline_armed =
      (running_images_confirmed == 0U) ? 1U : 0U;
  ota_confirmation_deadline = OTA_CONFIRM_WATCHDOG_GRACE_MS;

  if ((HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK) ||
      (HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED) != HAL_OK))
  {
    Safety_LatchCriticalFault(SAFETY_STATUS_INTERNAL_ERROR);
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
    Safety_LatchCriticalFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  if ((HAL_ADC_Start_DMA(&hadc1, (uint32_t *)(uintptr_t)slow_adc_dma,
                         SAFETY_SLOW_ADC_COUNT) != HAL_OK) ||
      (HAL_ADC_Start_DMA(&hadc2, (uint32_t *)(uintptr_t)current_adc_dma,
                         SAFETY_CURRENT_ADC_COUNT) != HAL_OK))
  {
    Safety_LatchCriticalFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }

  if ((HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1) != HAL_OK) ||
      (HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_2) != HAL_OK) ||
      (HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_4) != HAL_OK) ||
      (HAL_TIM_Base_Start_IT(&htim6) != HAL_OK))
  {
    Safety_LatchCriticalFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }

  safety_status |= SAFETY_STATUS_READY | SAFETY_STATUS_ADC_RUNNING |
                   SAFETY_STATUS_VALVE_CALIBRATING;
  /* K12 is never coupled to Ethernet readiness. It is restored by the Secure
     1 ms owner only after a stable physical-input interval, authenticated
     ATECC/MCU pairing, and confirmed running images. */
  run_permit_restore_pending = 1U;
  physical_estop_inactive_ms = 0U;
  if (HAL_IWDG_Refresh(&hiwdg) != HAL_OK)
  {
    Safety_LatchCriticalFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  return SAFETY_RESULT_OK;
}

uint32_t Safety_GetStatus(void)
{
  uint32_t primask = Safety_EnterCritical();
  uint32_t status;

  if (Safety_PhysicalEStopActive())
  {
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED;
    physical_estop_inactive_ms = 0U;
    run_permit_restore_pending = 1U;
    Safety_HardDisableOutputs();
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

int32_t Safety_GetSteeringSnapshot(SAFETY_SteeringSnapshot *snapshot)
{
  uint32_t primask;

  if (snapshot == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  primask = Safety_EnterCritical();
  SteeringCan_GetSnapshot(snapshot, secure_uptime_ms,
                          last_command_sequence);
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

int32_t Safety_GetJ1939Snapshot(SAFETY_J1939Snapshot *snapshot)
{
  int32_t result;
  uint32_t primask;

  if (snapshot == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  primask = Safety_EnterCritical();
  result = VehicleCan_GetSnapshot(snapshot, secure_uptime_ms);
  Safety_ExitCritical(primask);
  return result;
}

int32_t Safety_GetSecurityStatus(SAFETY_SecurityStatus *status)
{
  uint32_t primask;

  if (status == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  primask = Safety_EnterCritical();
  *status = security_status;
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

static void Safety_UpdateOtaStatusBits(const SAFETY_OtaStatus *status)
{
  uint32_t primask = Safety_EnterCritical();

  safety_status &= ~(SAFETY_STATUS_OTA_ACTIVE | SAFETY_STATUS_OTA_READY);
  if (status->state == SAFETY_OTA_STATE_RECEIVING)
  {
    safety_status |= SAFETY_STATUS_OTA_ACTIVE;
  }
  else if (status->state == SAFETY_OTA_STATE_READY)
  {
    safety_status |= SAFETY_STATUS_OTA_READY;
  }
  Safety_ExitCritical(primask);
}

static void Safety_EnterOtaQuarantine(void)
{
  uint32_t primask = Safety_EnterCritical();

  /* Set the policy inhibit before the first potentially long erase/program
     operation. If the physical E-stop is released during that operation, the
     1 ms owner still sees OTA_ACTIVE and cannot restore K12. */
  safety_status |= SAFETY_STATUS_OTA_ACTIVE;
  run_permit_restore_pending = 0U;
  Safety_HardDisableOutputs();
  Safety_ExitCritical(primask);
}

static int32_t Safety_CompleteOtaOperation(int32_t operation_result,
                                           SAFETY_OtaStatus *status)
{
  SAFETY_OtaStatus actual_status;

  if (SecurityOta_GetStatus(&actual_status) != SAFETY_RESULT_OK)
  {
    Safety_LatchCriticalFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  *status = actual_status;
  Safety_UpdateOtaStatusBits(&actual_status);
  if ((operation_result != SAFETY_RESULT_OK) &&
      (actual_status.state != SAFETY_OTA_STATE_RECEIVING) &&
      (actual_status.state != SAFETY_OTA_STATE_READY))
  {
    uint32_t primask = Safety_EnterCritical();

    /* A rejected/failed BEGIN that never opened an OTA session must not
       strand manual driving. Recovery remains deferred until PB2 has been
       inactive for the reviewed debounce interval. */
    run_permit_restore_pending = 1U;
    physical_estop_inactive_ms = 0U;
    Safety_ExitCritical(primask);
  }
  return operation_result;
}

int32_t Safety_OtaGetStatus(SAFETY_OtaStatus *status)
{
  int32_t result = SecurityOta_GetStatus(status);
  if (result == SAFETY_RESULT_OK) { Safety_UpdateOtaStatusBits(status); }
  return result;
}

int32_t Safety_OtaBegin(const SAFETY_OtaBeginRequest *request,
                        SAFETY_OtaStatus *status)
{
  int32_t result;

  if ((request == NULL) || (status == NULL))
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  if (ota_running_images_confirmed == 0U)
  {
    return SAFETY_RESULT_NOT_READY;
  }
  /* Service update is permitted only in a mechanically verified stopped
     state. The NC physical E-stop both opens the external K12 coil supply and
     provides this independent Secure input. */
  if (!Safety_PhysicalEStopActive())
  {
    return SAFETY_RESULT_NOT_READY;
  }
  Safety_EnterOtaQuarantine();
  result = SecurityOta_Begin(request, status);
  return Safety_CompleteOtaOperation(result, status);
}

int32_t Safety_OtaWrite(const SAFETY_OtaChunk *chunk,
                        SAFETY_OtaStatus *status)
{
  int32_t result;
  uint32_t primask;
  if ((chunk == NULL) || (status == NULL))
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  if (ota_running_images_confirmed == 0U)
  {
    return SAFETY_RESULT_NOT_READY;
  }
  if (!Safety_PhysicalEStopActive())
  {
    primask = Safety_EnterCritical();
    if ((safety_status & (SAFETY_STATUS_OTA_ACTIVE |
                          SAFETY_STATUS_OTA_READY)) != 0U)
    {
      Safety_HardDisableOutputs();
    }
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_NOT_READY;
  }
  Safety_EnterOtaQuarantine();
  result = SecurityOta_Write(chunk, status);
  return Safety_CompleteOtaOperation(result, status);
}

int32_t Safety_OtaFinish(uint32_t update_sequence,
                         SAFETY_OtaStatus *status)
{
  int32_t result;
  uint32_t primask;
  if (status == NULL) { return SAFETY_RESULT_BAD_ARGUMENT; }
  if (ota_running_images_confirmed == 0U)
  {
    return SAFETY_RESULT_NOT_READY;
  }
  if (!Safety_PhysicalEStopActive())
  {
    primask = Safety_EnterCritical();
    if ((safety_status & (SAFETY_STATUS_OTA_ACTIVE |
                          SAFETY_STATUS_OTA_READY)) != 0U)
    {
      Safety_HardDisableOutputs();
    }
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_NOT_READY;
  }
  Safety_EnterOtaQuarantine();
  result = SecurityOta_Finish(update_sequence, status);
  return Safety_CompleteOtaOperation(result, status);
}

int32_t Safety_OtaConfirmRunningImages(void)
{
  int32_t result;
  uint32_t confirmed = 0U;

  if (((security_status.flags & SAFETY_SECURITY_AUTHENTICATED) == 0U) ||
      ((safety_status & SAFETY_STATUS_READY) == 0U))
  {
    return SAFETY_RESULT_AUTHENTICATION;
  }
  if ((ota_running_images_confirmed == 0U) &&
      !Safety_PhysicalEStopActive())
  {
    /* The independent stop input must remain asserted across transfer,
       reset, Secure authentication and paired-image confirmation.  Releasing
       it early cannot make the test image operational; the watchdog then
       leaves OEMiROT to revert the unconfirmed pair. */
    uint32_t primask = Safety_EnterCritical();
    Safety_HardDisableOutputs();
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_NOT_READY;
  }
  if ((ota_running_images_confirmed == 0U) &&
      (ota_confirmation_deadline_armed != 0U) &&
      Safety_TimeReached(secure_uptime_ms, ota_confirmation_deadline))
  {
    uint32_t primask = Safety_EnterCritical();
    Safety_HardDisableOutputs();
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_NOT_READY;
  }
  result = SecurityOta_ConfirmRunningImages();
  if (result != SAFETY_RESULT_OK)
  {
    Safety_SetOtaConfirmationState(0U);
    return result;
  }
  result = SecurityOta_RunningImagesConfirmed(&confirmed);
  if ((result != SAFETY_RESULT_OK) || (confirmed == 0U))
  {
    Safety_SetOtaConfirmationState(0U);
    return (result != SAFETY_RESULT_OK) ? result : SAFETY_RESULT_STORAGE;
  }
  Safety_SetOtaConfirmationState(1U);
  ota_confirmation_deadline_armed = 0U;
  run_permit_restore_pending = 1U;
  physical_estop_inactive_ms = 0U;
  return SAFETY_RESULT_OK;
}

#if defined(ECU_FACTORY_PROVISIONING)
int32_t Safety_FactoryGetStatus(SAFETY_FactoryStatus *status)
{
  if (status == NULL) { return SAFETY_RESULT_BAD_ARGUMENT; }
  return SecurityFactory_GetStatus(status);
}

int32_t Safety_FactoryProvision(
    const SAFETY_FactoryProvisionRequest *request,
    SAFETY_FactoryStatus *status)
{
  uint32_t primask;

  if ((request == NULL) || (status == NULL))
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  primask = Safety_EnterCritical();
  safety_status |= SAFETY_STATUS_OTA_ACTIVE;
  run_permit_restore_pending = 0U;
  Safety_HardDisableOutputs();
  Safety_ExitCritical(primask);
  return SecurityFactory_Provision(request, status);
}
#endif

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

  if (ota_running_images_confirmed == 0U)
  {
    return SAFETY_RESULT_NOT_READY;
  }
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

  if (ota_running_images_confirmed == 0U)
  {
    return SAFETY_RESULT_NOT_READY;
  }

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
    Safety_LatchCriticalFault(SAFETY_STATUS_INTERNAL_ERROR);
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

  if (ota_running_images_confirmed == 0U)
  {
    return SAFETY_RESULT_NOT_READY;
  }

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
  if (Safety_PhysicalEStopActive())
  {
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED;
    Safety_HardDisableOutputs();
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
  Safety_QuiesceControlledOutputs();
  if ((valve_control.forward_current_ma >= SAFETY_VALVE_TARGET_DEADBAND_MA) ||
      (valve_control.reverse_current_ma >= SAFETY_VALVE_TARGET_DEADBAND_MA))
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_BUSY;
  }
  valve_control.fault_flags = 0U;
  ValveControl_ForceSafe(&valve_control);
  if (SteeringCan_CanClearFault())
  {
    SteeringCan_ClearFaults();
    safety_status &= ~SAFETY_STATUS_STEERING_CAN_FAULT;
  }
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
  if (ota_running_images_confirmed == 0U)
  {
    return SAFETY_RESULT_NOT_READY;
  }
  primask = Safety_EnterCritical();
  if (((safety_status & SAFETY_STATUS_READY) == 0U) ||
      (valve_control.calibrated == 0U) ||
      ((safety_status & (SAFETY_STATUS_OTA_ACTIVE |
                         SAFETY_STATUS_OTA_READY)) != 0U))
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_NOT_READY;
  }
  if (!Safety_RunPermitCanClose())
  {
    Safety_HardDisableOutputs();
    Safety_ExitCritical(primask);
    return ((safety_status & SAFETY_STATUS_NETWORK_ESTOP_LATCHED) != 0U ||
            Safety_PhysicalEStopActive()) ? SAFETY_RESULT_ESTOP_ACTIVE :
                                           SAFETY_RESULT_NOT_READY;
  }
  if ((safety_status & SAFETY_STATUS_FAULT_LATCHED) != 0U)
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_FAULT_LATCHED;
  }

  /* Preserve the already energized K12 baseline while preparing the general
     actuator owner. No zero-mask pulse is allowed at this transition. */
  if (!Safety_ApplyRunPermitBaseline())
  {
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_TPIC_ERROR;
  }
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
  Safety_QuiesceControlledOutputs();
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

int32_t Safety_AssertNetworkEStop(void)
{
  uint32_t primask = Safety_EnterCritical();

  safety_status |= SAFETY_STATUS_NETWORK_ESTOP_LATCHED;
  run_permit_restore_pending = 0U;
  Safety_HardDisableOutputs();
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

int32_t Safety_ResetNetworkEStop(uint32_t request_token)
{
  uint32_t primask;

  if (request_token != SAFETY_NETWORK_ESTOP_RESET_TOKEN)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  primask = Safety_EnterCritical();
  if (Safety_PhysicalEStopActive())
  {
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED;
    Safety_HardDisableOutputs();
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_ESTOP_ACTIVE;
  }
  if (((safety_status & SAFETY_STATUS_READY) == 0U) ||
      ((security_status.flags & SAFETY_SECURITY_AUTHENTICATED) == 0U) ||
      (ota_running_images_confirmed == 0U) ||
      ((safety_status & RUN_PERMIT_CRITICAL_STATUS_MASK) != 0U))
  {
    Safety_HardDisableOutputs();
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_NOT_READY;
  }

  safety_status &= ~SAFETY_STATUS_NETWORK_ESTOP_LATCHED;
  run_permit_restore_pending = 1U;
  /* Do not bypass the PB2 release debounce even for an authenticated RESET.
     The 1 ms Secure owner restores exactly the K12 baseline only after a new
     continuously inactive 20 ms interval. */
  physical_estop_inactive_ms = 0U;
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

int32_t Safety_SubmitActuatorCommand(const SAFETY_ActuatorCommand *command)
{
  uint32_t primask;
  uint32_t new_mask;
  int32_t steering_result;
  bool steering_rejected = false;
  bool steering_values_valid;
  int32_t result = Safety_ValidateCommand(command);

  if ((command != NULL) && (command->run_permit_on == 0U))
  {
    (void)Safety_AssertNetworkEStop();
    return SAFETY_RESULT_ESTOP_ACTIVE;
  }

  if (ota_running_images_confirmed == 0U)
  {
    return SAFETY_RESULT_NOT_READY;
  }

  if (result != SAFETY_RESULT_OK)
  {
    primask = Safety_EnterCritical();
    safety_status |= SAFETY_STATUS_COMMAND_REJECTED;
    SteeringCan_RequestSafe(secure_uptime_ms);
    Safety_ExitCritical(primask);
    return result;
  }
  steering_values_valid = Safety_SteeringCommandValuesValid(command);

  primask = Safety_EnterCritical();
  if ((safety_status & (SAFETY_STATUS_OTA_ACTIVE |
                        SAFETY_STATUS_OTA_READY)) != 0U)
  {
    Safety_HardDisableOutputs();
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_BUSY;
  }
  if (Safety_PhysicalEStopActive())
  {
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE | SAFETY_STATUS_FAULT_LATCHED;
    Safety_HardDisableOutputs();
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_ESTOP_ACTIVE;
  }
  if ((safety_status & SAFETY_STATUS_NETWORK_ESTOP_LATCHED) != 0U)
  {
    Safety_HardDisableOutputs();
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
  if (!steering_values_valid)
  {
    SteeringCan_RecordCommandRejected(secure_uptime_ms);
    steering_rejected = true;
  }
  else
  {
    steering_result = SteeringCan_SetCommand(
        command->steering_velocity_tdeg_per_s,
        command->steering_enable, command->steering_source,
        command->steering_flags, secure_uptime_ms);
    if (steering_result != SAFETY_RESULT_OK)
    {
      /* Stateful steering rejection (source handover, manual two-second
         limit, missing ACK or bus fault) clamps only CAN2 to zero/disable.
         Other validated actuators remain live under the global watchdog. */
      SteeringCan_RecordCommandRejected(secure_uptime_ms);
      steering_rejected = true;
    }
  }
  new_mask = Safety_BuildBaseRelayMask(command);
  requested_relay_mask = new_mask;
  new_mask = Safety_ApplyDynamicRelayBits(new_mask);

  if (!Safety_TpicShift(new_mask))
  {
    safety_status |= SAFETY_STATUS_TPIC_ERROR | SAFETY_STATUS_FAULT_LATCHED;
    Safety_HardDisableOutputs();
    Safety_ExitCritical(primask);
    return SAFETY_RESULT_TPIC_ERROR;
  }

  result = ValveControl_SetTarget(&valve_control,
                                  command->valve_current_target_ma);
  if (result != SAFETY_RESULT_OK)
  {
    safety_status |= SAFETY_STATUS_COMMAND_REJECTED;
    Safety_QuiesceControlledOutputs();
    Safety_ExitCritical(primask);
    return result;
  }
  applied_relay_mask = new_mask;
  last_command_sequence = command->sequence;
  command_sequence_valid = 1U;
  command_fresh = 1U;
  last_command_tick = secure_uptime_ms;
  safety_status &= ~SAFETY_STATUS_COMMAND_TIMEOUT;
  if (steering_rejected)
  {
    safety_status |= SAFETY_STATUS_COMMAND_REJECTED;
  }
  else
  {
    safety_status &= ~SAFETY_STATUS_COMMAND_REJECTED;
  }
  Safety_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

int32_t Safety_KickWatchdog(uint32_t heartbeat)
{
  uint32_t primask;

  if ((safety_status & SAFETY_STATUS_READY) == 0U)
  {
    return SAFETY_RESULT_NOT_READY;
  }
  if (heartbeat == last_watchdog_heartbeat)
  {
    return SAFETY_RESULT_STALE_SEQUENCE;
  }
  if (ota_running_images_confirmed == 0U)
  {
    if ((ota_confirmation_deadline_armed == 0U) ||
        Safety_TimeReached(secure_uptime_ms, ota_confirmation_deadline))
    {
      primask = Safety_EnterCritical();
      Safety_HardDisableOutputs();
      Safety_ExitCritical(primask);
      return SAFETY_RESULT_NOT_READY;
    }
  }
  if (HAL_IWDG_Refresh(&hiwdg) != HAL_OK)
  {
    Safety_LatchCriticalFault(SAFETY_STATUS_INTERNAL_ERROR);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  last_watchdog_heartbeat = heartbeat;
  return SAFETY_RESULT_OK;
}

void Safety_FaultFromException(void)
{
  Safety_LatchCriticalFault(SAFETY_STATUS_INTERNAL_ERROR);
}

void HAL_GPIO_EXTI_Rising_Callback(uint16_t gpio_pin)
{
  if (gpio_pin == ESTOP_DETECT_Pin)
  {
    uint32_t primask = Safety_EnterCritical();
    safety_status |= SAFETY_STATUS_ESTOP_ACTIVE |
                     SAFETY_STATUS_FAULT_LATCHED;
    physical_estop_inactive_ms = 0U;
    run_permit_restore_pending = 1U;
    Safety_HardDisableOutputs();
    Safety_ExitCritical(primask);
  }
}

void HAL_GPIO_EXTI_Falling_Callback(uint16_t gpio_pin)
{
  if (gpio_pin == ESTOP_DETECT_Pin)
  {
    uint32_t primask = Safety_EnterCritical();
    safety_status &= ~SAFETY_STATUS_ESTOP_ACTIVE;
    physical_estop_inactive_ms = 0U;
    run_permit_restore_pending = 1U;
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
      Safety_QuiesceControlledOutputs();
    }
  }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  (void)hadc;
  Safety_LatchCriticalFault(SAFETY_STATUS_INTERNAL_ERROR);
}

void HAL_ADC_LevelOutOfWindowCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC2)
  {
    valve_control.fault_flags |= SAFETY_STATUS_VALVE_OVERCURRENT;
    Safety_LatchIsolatedFault(SAFETY_STATUS_VALVE_OVERCURRENT);
  }
}

void HAL_GTZC_TZIC_Callback(uint32_t periph_id)
{
  (void)periph_id;
  Safety_LatchCriticalFault(SAFETY_STATUS_GTZC_VIOLATION);
}
