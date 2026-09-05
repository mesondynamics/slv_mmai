#include "ecu_data_model.h"

#include <limits.h>
#include <string.h>

#include "j1939.h"
#include "main.h"
#include "secure_nsc.h"
#include "speed_sensor.h"

#define ECU_ADC_REFERENCE_MV          3360UL
#define ECU_ENGINE_RUN_THRESHOLD_MV   3200U

_Static_assert(SAFETY_ACTUATOR_API_VERSION == 3UL,
               "Steering command mapping requires actuator API v3");
_Static_assert(SAFETY_STEERING_API_VERSION == 1UL,
               "Unsupported steering snapshot API");
_Static_assert(sizeof(SAFETY_ActuatorSnapshot) == 60U,
               "Frozen actuator snapshot ABI changed");
_Static_assert(sizeof(SAFETY_SteeringSnapshot) == 72U,
               "Steering snapshot ABI changed");

enum
{
  ADC_OIL_LEVEL = 0,
  ADC_OIL_TEMPERATURE = 1,
  ADC_WATER_LEVEL = 2,
  ADC_TEMPERATURE_1 = 3,
  ADC_TEMPERATURE_2 = 4,
  ADC_ENGINE_SIGNAL = 5,
  ADC_VBAT_12V = 6,
  ADC_VREFINT = 7,
  ADC_INTERNAL_TEMPERATURE = 8
};

static ECU_StatusPayloadV2 status_payload;
static ECU_SteeringStatusPayloadV2 steering_status_payload;
static ECU_ControlPayloadV2 current_control;
static SAFETY_AdcSnapshot adc_snapshot;
static SAFETY_ActuatorSnapshot actuator_snapshot;
static ECU_ControlMode authority_mode;
static uint8_t authority_sender = 0xFFU;

static uint16_t ECU_AdcToMillivolts(uint16_t raw)
{
  return (uint16_t)(((uint32_t)raw * ECU_ADC_REFERENCE_MV) / 4096UL);
}

static bool ECU_RelayIsOn(uint32_t mask)
{
  return (actuator_snapshot.applied_relay_mask & mask) != 0U;
}

static uint8_t ECU_SteeringSourceFromSender(uint8_t sender_id)
{
  switch (sender_id)
  {
    case 1U:
      return SAFETY_STEERING_SOURCE_REMOTE;

    case 3U:
      return SAFETY_STEERING_SOURCE_AUTONOMOUS;

    case 2U:
    default:
      /* Custom/unknown senders are deliberately treated as an operator
         source so they cannot inherit the longer autonomous authority path. */
      return SAFETY_STEERING_SOURCE_OPERATOR;
  }
}

static bool ECU_SenderCanControlSteering(uint8_t sender_id)
{
  return (sender_id >= 1U) && (sender_id <= 3U);
}

static uint16_t ECU_SaturateU32ToU16(uint32_t value)
{
  return (value > UINT16_MAX) ? UINT16_MAX : (uint16_t)value;
}

bool ECU_DataModelInit(void)
{
  memset(&status_payload, 0, sizeof(status_payload));
  memset(&steering_status_payload, 0, sizeof(steering_status_payload));
  memset(&current_control, 0, sizeof(current_control));
  memset(&adc_snapshot, 0, sizeof(adc_snapshot));
  memset(&actuator_snapshot, 0, sizeof(actuator_snapshot));
  authority_mode = ECU_CONTROL_IDLE;
  authority_sender = 0xFFU;
  return (SECURE_SafetyGetAdcSnapshot(&adc_snapshot) == SAFETY_RESULT_OK) &&
         (SECURE_SafetyGetActuatorSnapshot(&actuator_snapshot) == SAFETY_RESULT_OK);
}

int32_t ECU_DataModelApplyControl(const ECU_ControlPayloadV2 *control,
                                  uint16_t control_flags,
                                  uint8_t sender_id,
                                  uint32_t secure_sequence)
{
  SAFETY_ActuatorCommand command = {0};
  ECU_ControlPayloadV2 accepted_control;
  bool steering_rate_mode;
  int32_t result;

  if (control == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  if ((control->valve_current_target_ma < -SAFETY_VALVE_TARGET_MAX_MA) ||
      (control->valve_current_target_ma > SAFETY_VALVE_TARGET_MAX_MA))
  {
    return SAFETY_RESULT_RANGE;
  }
  if ((control_flags & ~ECU_CONTROL_FLAG_STEERING_RATE) != 0U)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }

  accepted_control = *control;
  steering_rate_mode =
      (control_flags & ECU_CONTROL_FLAG_STEERING_RATE) != 0U;
  if (!ECU_SenderCanControlSteering(sender_id) &&
      (steering_rate_mode ||
       (accepted_control.steering_enable != 0U) ||
       (accepted_control.steering_target_tdeg != 0) ||
       (accepted_control.steering_speed_tdeg_per_s != 0U)))
  {
    /* Defense in depth if a future transport bypasses ecu_network.c. Reject
       obsolete nonzero fields too, rather than silently acknowledging a
       custom sender whose steering request will be discarded. */
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  if (!steering_rate_mode || (accepted_control.steering_enable == 0U))
  {
    accepted_control.steering_target_tdeg = 0;
    accepted_control.steering_speed_tdeg_per_s = 0U;
    accepted_control.steering_enable = 0U;
  }

  command.api_version = SAFETY_ACTUATOR_API_VERSION;
  command.sequence = secure_sequence;
  command.valve_current_target_ma = control->valve_current_target_ma;
  command.pump_enable = control->pump_enable;
  command.pump_select = control->pump_select;
  command.headlamp_front_on = control->headlamp_front_on;
  command.headlamp_rear_on = control->headlamp_rear_on;
  command.led_front_on = control->led_front_on;
  command.led_rear_on = control->led_rear_on;
  command.buzzer_reverse_on = control->buzzer_reverse_on;
  command.buzzer_main_on = control->buzzer_main_on;
  command.vib_strong_on = control->vib_strong_on;
  command.vib_weak_on = control->vib_weak_on;
  command.vib_front_selected = control->vib_front_selected;
  command.vib_rear_selected = control->vib_rear_selected;
  command.engine_start_request = control->engine_start_request;
  command.speed_mode_high = control->speed_mode_high;
  command.engine_speed_level = control->engine_speed_level;
  command.parking_brake_on = control->parking_brake_on;
  /* Keep the previous ECU semantic: emergency_stop_request=1 requests a
     stop.  This PCB's fail-safe K12 has the opposite physical polarity, so a
     normal command energizes the run permit and an emergency releases it. */
  command.run_permit_on = (control->emergency_stop_request == 0U) ? 1U : 0U;
  command.turn_signal_right_on = control->turn_signal_right_on;
  command.turn_signal_left_on = control->turn_signal_left_on;
  command.steering_velocity_tdeg_per_s =
      accepted_control.steering_target_tdeg;
  command.steering_enable = accepted_control.steering_enable;
  command.steering_source = ECU_SteeringSourceFromSender(sender_id);
  command.steering_flags = steering_rate_mode ?
      SAFETY_STEERING_FLAG_RATE_MODE : 0U;

  result = SECURE_SafetySubmitActuatorCommand(&command);
  if (result == SAFETY_RESULT_OK)
  {
    current_control = accepted_control;
  }
  return result;
}

void ECU_DataModelControlLost(void)
{
  memset(&current_control, 0, sizeof(current_control));
  authority_mode = ECU_CONTROL_IDLE;
  authority_sender = 0xFFU;
  (void)SECURE_SafetyDisarmOutputs();
}

void ECU_DataModelSetAuthority(ECU_ControlMode mode, uint8_t sender_id)
{
  authority_mode = mode;
  authority_sender = sender_id;
}

void ECU_DataModelUpdateStatus(void)
{
  SpeedSensorMeasurement speed = {0};
  J1939Data j1939 = {0};
  uint32_t mask;
  uint32_t safety_status;

  (void)SECURE_SafetyGetAdcSnapshot(&adc_snapshot);
  (void)SECURE_SafetyGetActuatorSnapshot(&actuator_snapshot);
  SpeedSensor_GetMeasurement(&speed);
  J1939_Poll();
  J1939_GetData(&j1939);

  ++status_payload.sequence_id;
  status_payload.timestamp_ms = HAL_GetTick();
  status_payload.steering_raw = 0U;
  status_payload.steering_cdeg = 0;
  status_payload.steering_mv = 0U;
  status_payload.water_mv = ECU_AdcToMillivolts(adc_snapshot.slow_adc[ADC_WATER_LEVEL]);
  status_payload.oil_temperature_mv =
      ECU_AdcToMillivolts(adc_snapshot.slow_adc[ADC_OIL_TEMPERATURE]);
  status_payload.oil_level_mv =
      ECU_AdcToMillivolts(adc_snapshot.slow_adc[ADC_OIL_LEVEL]);
  status_payload.reserved_mv = 0U;
  status_payload.engine_signal_raw = adc_snapshot.slow_adc[ADC_ENGINE_SIGNAL];
  status_payload.engine_signal_mv =
      ECU_AdcToMillivolts(status_payload.engine_signal_raw);
  status_payload.speed_frequency_centi_hz = speed.frequency_centi_hz;
  status_payload.vehicle_speed_centi_kph = speed.speed_centi_kph;
  status_payload.speed_signal_present = speed.signal_present ? 1U : 0U;
  status_payload.engine_running =
      (status_payload.engine_signal_mv >= ECU_ENGINE_RUN_THRESHOLD_MV) ? 1U : 0U;

  mask = actuator_snapshot.applied_relay_mask;
  safety_status = actuator_snapshot.status;
  status_payload.pump_enabled =
      ((mask & (SAFETY_RELAY_PUMP_1 | SAFETY_RELAY_PUMP_2)) != 0U) ? 1U : 0U;
  status_payload.pump_select = ECU_RelayIsOn(SAFETY_RELAY_PUMP_2) ? 1U : 0U;
  status_payload.headlamp_front_on = ECU_RelayIsOn(SAFETY_RELAY_FRONT_MAIN);
  status_payload.headlamp_rear_on = ECU_RelayIsOn(SAFETY_RELAY_REAR_MAIN);
  status_payload.led_front_on = ECU_RelayIsOn(SAFETY_RELAY_FRONT_LED);
  status_payload.led_rear_on = ECU_RelayIsOn(SAFETY_RELAY_REAR_LED);
  status_payload.buzzer_reverse_on = ECU_RelayIsOn(SAFETY_RELAY_REVERSE_ALARM);
  status_payload.buzzer_main_on = ECU_RelayIsOn(SAFETY_RELAY_HORN);
  status_payload.indicator1_on = 0U;
  status_payload.indicator2_on = 0U;
  status_payload.indicator3_on = 0U;
  status_payload.vib_original_on = ECU_RelayIsOn(SAFETY_RELAY_VIB_ENABLE_OVERRIDE);
  status_payload.vib_strong_on = ECU_RelayIsOn(SAFETY_RELAY_VIB_HIGH_LEVEL);
  status_payload.vib_weak_on = ECU_RelayIsOn(SAFETY_RELAY_VIB_LOW_LEVEL);
  status_payload.vib_front_selected = ECU_RelayIsOn(SAFETY_RELAY_VIB_FRONT_LEVEL);
  status_payload.vib_rear_selected = ECU_RelayIsOn(SAFETY_RELAY_VIB_REAR_LEVEL);
  status_payload.engine_start_request_on = ECU_RelayIsOn(SAFETY_RELAY_ENGINE_START);
  status_payload.speed_mode_high = ECU_RelayIsOn(SAFETY_RELAY_TRAVEL_SPEED_LEVEL);
  status_payload.engine_speed_level = actuator_snapshot.engine_speed_level;
  status_payload.power_latch_on = 0U;
  status_payload.parking_brake_on = ECU_RelayIsOn(SAFETY_RELAY_PARK_BRAKE_OVERRIDE);
  status_payload.emergency_stop_on =
      ((safety_status & (SAFETY_STATUS_ESTOP_ACTIVE |
                         SAFETY_STATUS_NETWORK_ESTOP_LATCHED)) != 0U) ? 1U : 0U;
  status_payload.main_power_relay_on = 0U;
  status_payload.turn_signal_right_on = ECU_RelayIsOn(SAFETY_RELAY_RIGHT_TURN);
  status_payload.turn_signal_left_on = ECU_RelayIsOn(SAFETY_RELAY_LEFT_TURN);

  if ((safety_status & SAFETY_STATUS_ESTOP_ACTIVE) != 0U)
  {
    status_payload.control_mode = ECU_CONTROL_PHYSICAL_ESTOP;
    status_payload.active_sender_id = 0xFFU;
  }
  else if ((safety_status & SAFETY_STATUS_NETWORK_ESTOP_LATCHED) != 0U)
  {
    status_payload.control_mode = ECU_CONTROL_EMERGENCY;
    status_payload.active_sender_id =
        (authority_mode == ECU_CONTROL_EMERGENCY) ? authority_sender : 0xFFU;
  }
  else
  {
    status_payload.control_mode = (uint8_t)authority_mode;
    status_payload.active_sender_id = authority_sender;
  }

  status_payload.j1939_engine_rpm = j1939.engine_rpm;
  status_payload.j1939_engine_torque_percent = j1939.engine_torque_percent;
  status_payload.j1939_engine_load_percent = j1939.engine_load_percent;
  status_payload.j1939_coolant_temp_cdeg = j1939.coolant_temp_cdeg;
  status_payload.j1939_oil_pressure_kpa = j1939.oil_pressure_kpa;
  status_payload.j1939_accelerator_percent = j1939.accelerator_pedal_percent;
  status_payload.j1939_fuel_temp_cdeg = j1939.fuel_temp_cdeg;
  status_payload.j1939_fuel_pressure_kpa = j1939.fuel_pressure_kpa;
  status_payload.j1939_ambient_temp_cdeg = j1939.ambient_temp_cdeg;
  status_payload.j1939_dtc_count = j1939.dtc_count;
  status_payload.j1939_data_valid = j1939.eec1_valid ? 1U : 0U;
  status_payload.valve_requested_target_ma =
      actuator_snapshot.valve_requested_target_ma;
  status_payload.valve_applied_target_ma =
      actuator_snapshot.valve_applied_target_ma;
  status_payload.forward_current_ma = actuator_snapshot.forward_current_ma;
  status_payload.reverse_current_ma = actuator_snapshot.reverse_current_ma;
  status_payload.forward_duty_permille =
      actuator_snapshot.forward_duty_permille;
  status_payload.reverse_duty_permille =
      actuator_snapshot.reverse_duty_permille;
  status_payload.valve_state = actuator_snapshot.valve_state;
  status_payload.valve_config_dirty = actuator_snapshot.valve_config_dirty;
  status_payload.valve_active_revision =
      actuator_snapshot.valve_active_revision;
  status_payload.valve_persisted_generation =
      actuator_snapshot.valve_persisted_generation;

}

bool ECU_DataModelUpdateSteeringStatus(void)
{
  SAFETY_SteeringSnapshot snapshot = {0};

  /* Version and capacity are scalar NSC inputs. Treat every snapshot field as
     Secure-owned output and validate its echoed layout before publishing. */
  if ((SECURE_SafetyGetSteeringSnapshot(SAFETY_STEERING_API_VERSION,
                                        &snapshot, sizeof(snapshot)) !=
       SAFETY_RESULT_OK) ||
      (snapshot.api_version != SAFETY_STEERING_API_VERSION) ||
      (snapshot.size != sizeof(snapshot)))
  {
    /* Never publish stale or layout-ambiguous steering feedback. The legacy
       100-byte status remains available to mixed/unsupported peers. */
    memset(&steering_status_payload, 0, sizeof(steering_status_payload));
    return false;
  }

  /* sequence_id identifies the accepted Secure command sampled here; the V2
     frame header supplies a separate monotonically increasing TX sequence. */
  steering_status_payload.sequence_id = snapshot.command_sequence;
  steering_status_payload.timestamp_ms = snapshot.timestamp_ms;
  steering_status_payload.requested_velocity_tdeg_per_s =
      snapshot.requested_velocity_tdeg_per_s;
  steering_status_payload.applied_velocity_tdeg_per_s =
      snapshot.applied_velocity_tdeg_per_s;
  steering_status_payload.speed_command_permille =
      snapshot.speed_command_permille;
  steering_status_payload.rx_age_ms = ECU_SaturateU32ToU16(
      snapshot.rx_age_ms);
  steering_status_payload.motor_fault_code =
      snapshot.motor_fault_code;
  steering_status_payload.motor_speed_feedback_raw =
      snapshot.motor_speed_feedback_raw;
  steering_status_payload.status_flags = snapshot.status_flags;
  steering_status_payload.fault_flags = snapshot.fault_flags;
  steering_status_payload.tx_frames = snapshot.tx_frames;
  steering_status_payload.rx_frames = snapshot.rx_frames;
  steering_status_payload.tx_errors = snapshot.tx_errors;
  steering_status_payload.rx_errors = snapshot.rx_errors;
  steering_status_payload.bus_off_events = snapshot.bus_off_events;
  steering_status_payload.state = snapshot.state;
  steering_status_payload.bus_state = snapshot.bus_state;
  steering_status_payload.command_enable = snapshot.command_enable;
  steering_status_payload.motor_enable_confirmed =
      snapshot.motor_enable_confirmed;
  return true;
}

const ECU_StatusPayloadV2 *ECU_DataModelGetStatus(void)
{
  return &status_payload;
}

const ECU_SteeringStatusPayloadV2 *ECU_DataModelGetSteeringStatus(void)
{
  return &steering_status_payload;
}

const ECU_ControlPayloadV2 *ECU_DataModelGetControl(void)
{
  return &current_control;
}

const SAFETY_AdcSnapshot *ECU_DataModelGetAdcSnapshot(void)
{
  return &adc_snapshot;
}

const SAFETY_ActuatorSnapshot *ECU_DataModelGetActuatorSnapshot(void)
{
  return &actuator_snapshot;
}

bool ECU_DataModelControlIsNeutral(const ECU_ControlPayloadV2 *control)
{
  ECU_ControlPayloadV2 zero = {0};

  return (control != NULL) &&
         (memcmp(control, &zero, sizeof(zero)) == 0);
}
