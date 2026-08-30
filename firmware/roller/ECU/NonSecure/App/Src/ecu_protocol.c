#include "ecu_protocol.h"

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(ECU_V2Header) == ECU_V2_HEADER_SIZE,
               "V2 frame header ABI changed");
_Static_assert(sizeof(ECU_ControlPayloadV1) == 31U,
               "V1 control payload ABI changed");
_Static_assert(sizeof(ECU_ControlDatagramV1) ==
                   ECU_V1_CONTROL_PAYLOAD_SIZE,
               "V1 control datagram ABI changed");
_Static_assert(sizeof(ECU_StatusPayloadV1) == ECU_V1_STATUS_PAYLOAD_SIZE,
               "V1 status payload ABI changed");
_Static_assert(sizeof(ECU_ControlPayloadV2) == 32U,
               "V2 control payload ABI changed");
_Static_assert(sizeof(ECU_ControlDatagramV2) == 36U,
               "V2 control datagram ABI changed");
_Static_assert(sizeof(ECU_StatusPayloadV2) == 100U,
               "V2 status payload ABI changed");
_Static_assert(sizeof(ECU_SteeringStatusPayloadV2) == 52U,
               "V2 steering status payload ABI changed");
_Static_assert(sizeof(ECU_DiagnosticPayloadV2) == 86U,
               "V2 diagnostic payload ABI changed");
_Static_assert(sizeof(ECU_TelemetrySubscribePayload) == 8U,
               "V2 subscription payload ABI changed");
_Static_assert(sizeof(SAFETY_ValveConfig) == 48U,
               "Valve configuration ABI changed");
_Static_assert(sizeof(SAFETY_ValveConfigSnapshot) == 64U,
               "Valve configuration snapshot ABI changed");
_Static_assert(sizeof(ECU_ValveConfigReplyPayload) == 72U,
               "V2 valve configuration reply ABI changed");
_Static_assert(sizeof(ECU_OperationAckPayload) == 8U,
               "V2 operation acknowledgement ABI changed");
_Static_assert(sizeof(SAFETY_ValveTelemetrySample) == 20U,
               "Valve telemetry sample ABI changed");
_Static_assert(sizeof(SAFETY_OtaManifest) == 128U,
               "OTA manifest ABI changed");
_Static_assert(sizeof(SAFETY_OtaBeginRequest) == 192U,
               "OTA begin ABI changed");
_Static_assert(sizeof(SAFETY_OtaChunk) == 532U,
               "OTA chunk ABI changed");
_Static_assert(sizeof(SAFETY_OtaStatus) == 36U,
               "OTA status ABI changed");
_Static_assert(sizeof(ECU_OtaStatusPayload) == 44U,
               "OTA status wire ABI changed");
#if defined(ECU_FACTORY_PROVISIONING)
_Static_assert(sizeof(SAFETY_FactoryProvisionRequest) == 32U,
               "ATECC factory request ABI changed");
_Static_assert(sizeof(ECU_FactoryAteccStatusPayload) == 239U,
               "ATECC factory status ABI changed");
#endif

uint32_t ECU_ProtocolCrc32c(const void *data, size_t length)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFUL;
  size_t index;
  uint32_t bit;

  for (index = 0U; index < length; ++index)
  {
    crc ^= bytes[index];
    for (bit = 0U; bit < 8U; ++bit)
    {
      crc = (crc >> 1U) ^
            ((crc & 1U) ? 0x82F63B78UL : 0U);
    }
  }
  return crc ^ 0xFFFFFFFFUL;
}

static uint8_t ECU_ProtocolXorParity(const uint8_t *data, size_t length)
{
  uint8_t parity = 0U;
  size_t index;

  for (index = 0U; index < length; ++index)
  {
    parity ^= data[index];
  }
  return parity;
}

size_t ECU_ProtocolEncodeV1(const void *payload, uint16_t payload_size,
                            uint8_t *frame, size_t frame_capacity)
{
  const uint8_t *payload_bytes = (const uint8_t *)payload;
  size_t frame_size = (size_t)payload_size + ECU_V1_FRAME_OVERHEAD;

  if ((frame == NULL) ||
      ((payload_size != 0U) && (payload == NULL)) ||
      (frame_size > frame_capacity))
  {
    return 0U;
  }
  frame[0] = ECU_V1_FRAME_START;
  frame[1] = (uint8_t)(payload_size >> 8U);
  frame[2] = (uint8_t)payload_size;
  if (payload_size != 0U)
  {
    memcpy(&frame[3], payload, payload_size);
  }
  frame[3U + payload_size] =
      ECU_ProtocolXorParity(payload_bytes, payload_size);
  frame[4U + payload_size] = ECU_V1_FRAME_STOP;
  return frame_size;
}

bool ECU_ProtocolDecodeV1(const uint8_t *frame, size_t frame_length,
                          void *payload, uint16_t expected_payload_size)
{
  uint16_t declared_size;

  if ((frame == NULL) ||
      ((expected_payload_size != 0U) && (payload == NULL)) ||
      (frame_length !=
       ((size_t)expected_payload_size + ECU_V1_FRAME_OVERHEAD)) ||
      (frame[0] != ECU_V1_FRAME_START))
  {
    return false;
  }
  declared_size = ((uint16_t)frame[1] << 8U) | frame[2];
  if ((declared_size != expected_payload_size) ||
      (frame[4U + declared_size] != ECU_V1_FRAME_STOP) ||
      (ECU_ProtocolXorParity(&frame[3], declared_size) !=
       frame[3U + declared_size]))
  {
    return false;
  }
  if (declared_size != 0U)
  {
    memcpy(payload, &frame[3], declared_size);
  }
  return true;
}

bool ECU_ProtocolControlV1ValuesValid(const ECU_ControlDatagramV1 *datagram)
{
  const ECU_ControlPayloadV1 *control;
  size_t index;

  if ((datagram == NULL) || (datagram->flags != 0U))
  {
    return false;
  }
  control = &datagram->control;

  const uint8_t boolean_fields[] = {
    control->pump_enable, control->pump_select,
    control->headlamp_front_on, control->headlamp_rear_on,
    control->led_front_on, control->led_rear_on,
    control->buzzer_reverse_on, control->buzzer_main_on,
    control->indicator1_on, control->indicator2_on,
    control->indicator3_on, control->vib_original_on,
    control->vib_strong_on, control->vib_weak_on,
    control->vib_front_selected, control->vib_rear_selected,
    control->engine_start_request, control->power_latch_on,
    control->speed_mode_high, control->steering_enable,
    control->parking_brake_on, control->emergency_stop_on,
    control->main_power_relay_on, control->turn_signal_right_on,
    control->turn_signal_left_on
  };

  for (index = 0U; index < (sizeof(boolean_fields) /
                            sizeof(boolean_fields[0])); ++index)
  {
    if (boolean_fields[index] > 1U)
    {
      return false;
    }
  }

  /* V1 has no physical-unit valve request and this PCB has no steering-angle
     feedback. Silently translating either legacy motion request would create
     uncontrolled movement, so the complete non-emergency frame is rejected.
     Fields without an actuator on this PCB are rejected for the same reason:
     a peer must never infer successful actuation from a refreshed session. */
  if ((control->steering_target_tdeg != 0) ||
      (control->steering_speed_tdeg_per_s != 0U) ||
      (control->steering_enable != 0U) ||
      (control->throttle_percent != 0) ||
      (control->indicator1_on != 0U) ||
      (control->indicator2_on != 0U) ||
      (control->indicator3_on != 0U) ||
      (control->power_latch_on != 0U) ||
      (control->main_power_relay_on != 0U) ||
      (control->engine_speed_level < -3) ||
      (control->engine_speed_level > 3) ||
      ((control->vib_strong_on != 0U) &&
       (control->vib_weak_on != 0U)))
  {
    return false;
  }
  return true;
}

bool ECU_ProtocolConvertControlV1(const ECU_ControlDatagramV1 *source,
                                  ECU_ControlDatagramV2 *destination)
{
  const ECU_ControlPayloadV1 *input;
  ECU_ControlPayloadV2 *output;

  if ((source == NULL) || (destination == NULL) ||
      !ECU_ProtocolControlV1ValuesValid(source))
  {
    return false;
  }
  memset(destination, 0, sizeof(*destination));
  destination->sender_id = source->sender_id;
  destination->priority = source->priority;
  input = &source->control;
  output = &destination->control;
  output->pump_enable = input->pump_enable;
  output->pump_select = input->pump_select;
  output->headlamp_front_on = input->headlamp_front_on;
  output->headlamp_rear_on = input->headlamp_rear_on;
  output->led_front_on = input->led_front_on;
  output->led_rear_on = input->led_rear_on;
  output->buzzer_reverse_on = input->buzzer_reverse_on;
  output->buzzer_main_on = input->buzzer_main_on;
  output->vib_original_on = input->vib_original_on;
  output->vib_strong_on = input->vib_strong_on;
  output->vib_weak_on = input->vib_weak_on;
  output->vib_front_selected = input->vib_front_selected;
  output->vib_rear_selected = input->vib_rear_selected;
  output->engine_start_request = input->engine_start_request;
  output->speed_mode_high = input->speed_mode_high;
  output->engine_speed_level = input->engine_speed_level;
  output->parking_brake_on = input->parking_brake_on;
  output->emergency_stop_request = input->emergency_stop_on;
  output->turn_signal_right_on = input->turn_signal_right_on;
  output->turn_signal_left_on = input->turn_signal_left_on;

  /* The destination was zero-initialized: steering remains disabled and the
     physical-unit valve current target remains exactly zero. */
  return ECU_ProtocolControlValuesValid(0U, output);
}

bool ECU_ProtocolBuildStatusV1(const ECU_StatusPayloadV2 *source,
                               ECU_StatusPayloadV1 *destination)
{
  if ((source == NULL) || (destination == NULL))
  {
    return false;
  }
  memset(destination, 0, sizeof(*destination));
  destination->sequence_id = source->sequence_id;
  destination->timestamp_ms = source->timestamp_ms;
  destination->steering_raw = source->steering_raw;
  destination->steering_cdeg = source->steering_cdeg;
  destination->steering_mv = source->steering_mv;
  destination->water_mv = source->water_mv;
  destination->oil_temperature_mv = source->oil_temperature_mv;
  destination->oil_level_mv = source->oil_level_mv;
  destination->reserved_mv = source->reserved_mv;
  destination->engine_signal_mv = source->engine_signal_mv;
  destination->engine_signal_raw = source->engine_signal_raw;
  destination->speed_frequency_centi_hz =
      source->speed_frequency_centi_hz;
  destination->vehicle_speed_centi_kph = source->vehicle_speed_centi_kph;
  destination->speed_signal_present = source->speed_signal_present;
  destination->engine_running = source->engine_running;
  destination->pump_enabled = source->pump_enabled;
  destination->pump_select = source->pump_select;
  destination->headlamp_front_on = source->headlamp_front_on;
  destination->headlamp_rear_on = source->headlamp_rear_on;
  destination->led_front_on = source->led_front_on;
  destination->led_rear_on = source->led_rear_on;
  destination->buzzer_reverse_on = source->buzzer_reverse_on;
  destination->buzzer_main_on = source->buzzer_main_on;
  destination->indicator1_on = source->indicator1_on;
  destination->indicator2_on = source->indicator2_on;
  destination->indicator3_on = source->indicator3_on;
  destination->vib_original_on = source->vib_original_on;
  destination->vib_strong_on = source->vib_strong_on;
  destination->vib_weak_on = source->vib_weak_on;
  destination->vib_front_selected = source->vib_front_selected;
  destination->vib_rear_selected = source->vib_rear_selected;
  destination->engine_start_request_on = source->engine_start_request_on;
  destination->speed_mode_high = source->speed_mode_high;
  destination->engine_speed_level = source->engine_speed_level;
  destination->power_latch_on = source->power_latch_on;
  destination->throttle_percent = 0;
  destination->parking_brake_on = source->parking_brake_on;
  destination->emergency_stop_on = source->emergency_stop_on;
  destination->main_power_relay_on = source->main_power_relay_on;
  destination->turn_signal_right_on = source->turn_signal_right_on;
  destination->turn_signal_left_on = source->turn_signal_left_on;
  destination->control_mode = source->control_mode;
  destination->active_sender_id = source->active_sender_id;
  destination->j1939_engine_rpm = source->j1939_engine_rpm;
  destination->j1939_engine_torque_percent =
      source->j1939_engine_torque_percent;
  destination->j1939_engine_load_percent =
      source->j1939_engine_load_percent;
  destination->j1939_coolant_temp_cdeg =
      source->j1939_coolant_temp_cdeg;
  destination->j1939_oil_pressure_kpa = source->j1939_oil_pressure_kpa;
  destination->j1939_accelerator_percent =
      source->j1939_accelerator_percent;
  destination->j1939_fuel_temp_cdeg = source->j1939_fuel_temp_cdeg;
  destination->j1939_fuel_pressure_kpa = source->j1939_fuel_pressure_kpa;
  destination->j1939_ambient_temp_cdeg = source->j1939_ambient_temp_cdeg;
  destination->j1939_dtc_count = source->j1939_dtc_count;
  destination->j1939_data_valid = source->j1939_data_valid;
  return true;
}

size_t ECU_ProtocolEncodeV2(uint8_t message_type, uint16_t flags,
                            uint32_t sequence, uint32_t timestamp_ms,
                            const void *payload, uint16_t payload_size,
                            uint8_t *frame, size_t frame_capacity)
{
  ECU_V2Header header = {
    .magic = ECU_V2_MAGIC,
    .version = ECU_V2_VERSION,
    .message_type = message_type,
    .header_size = ECU_V2_HEADER_SIZE,
    .payload_size = payload_size,
    .flags = flags,
    .sequence = sequence,
    .timestamp_ms = timestamp_ms,
    .crc32c = 0U
  };
  size_t frame_size = sizeof(header) + payload_size;
  uint32_t crc;

  if ((frame == NULL) || (frame_size > frame_capacity) ||
      (frame_size > ECU_V2_MAX_FRAME_SIZE) ||
      ((payload_size != 0U) && (payload == NULL)))
  {
    return 0U;
  }
  memcpy(frame, &header, sizeof(header));
  if (payload_size != 0U)
  {
    memcpy(frame + sizeof(header), payload, payload_size);
  }
  crc = ECU_ProtocolCrc32c(frame, frame_size);
  memcpy(frame + offsetof(ECU_V2Header, crc32c), &crc, sizeof(crc));
  return frame_size;
}

bool ECU_ProtocolDecodeV2(const uint8_t *frame, size_t frame_length,
                          ECU_V2Header *header, void *payload,
                          size_t payload_capacity)
{
  ECU_V2Header decoded;
  uint8_t copy[ECU_V2_MAX_FRAME_SIZE];
  uint32_t expected_crc;
  uint32_t zero = 0U;

  if ((frame == NULL) || (header == NULL) ||
      (frame_length < sizeof(decoded)) ||
      (frame_length > sizeof(copy)))
  {
    return false;
  }
  memcpy(&decoded, frame, sizeof(decoded));
  if ((decoded.magic != ECU_V2_MAGIC) ||
      (decoded.version != ECU_V2_VERSION) ||
      (decoded.header_size != sizeof(decoded)) ||
      ((size_t)decoded.header_size + decoded.payload_size != frame_length) ||
      (decoded.payload_size > payload_capacity) ||
      ((decoded.payload_size != 0U) && (payload == NULL)))
  {
    return false;
  }
  memcpy(copy, frame, frame_length);
  expected_crc = decoded.crc32c;
  memcpy(copy + offsetof(ECU_V2Header, crc32c), &zero, sizeof(zero));
  if (ECU_ProtocolCrc32c(copy, frame_length) != expected_crc)
  {
    return false;
  }
  if (decoded.payload_size != 0U)
  {
    memcpy(payload, frame + sizeof(decoded), decoded.payload_size);
  }
  *header = decoded;
  return true;
}

bool ECU_ProtocolControlValuesValid(uint16_t flags,
                                    const ECU_ControlPayloadV2 *control)
{
  size_t index;

  if ((control == NULL) || ((flags & ~ECU_CONTROL_ALLOWED_FLAGS) != 0U))
  {
    return false;
  }

  const uint8_t boolean_fields[] = {
    control->pump_enable, control->pump_select,
    control->headlamp_front_on, control->headlamp_rear_on,
    control->led_front_on, control->led_rear_on,
    control->buzzer_reverse_on, control->buzzer_main_on,
    control->indicator1_on, control->indicator2_on,
    control->indicator3_on, control->vib_original_on,
    control->vib_strong_on, control->vib_weak_on,
    control->vib_front_selected, control->vib_rear_selected,
    control->engine_start_request, control->power_latch_on,
    control->speed_mode_high, control->steering_enable,
    control->parking_brake_on, control->emergency_stop_request,
    control->main_power_relay_on, control->turn_signal_right_on,
    control->turn_signal_left_on
  };

  for (index = 0U; index < (sizeof(boolean_fields) /
                            sizeof(boolean_fields[0])); ++index)
  {
    if (boolean_fields[index] > 1U)
    {
      return false;
    }
  }
  if ((control->engine_speed_level < -3) ||
      (control->engine_speed_level > 3) ||
      (control->valve_current_target_ma < -SAFETY_VALVE_TARGET_MAX_MA) ||
      (control->valve_current_target_ma > SAFETY_VALVE_TARGET_MAX_MA) ||
      ((control->vib_strong_on != 0U) &&
       (control->vib_weak_on != 0U)))
  {
    return false;
  }

  if ((flags & ECU_CONTROL_FLAG_STEERING_RATE) != 0U)
  {
    return (control->steering_target_tdeg >= -6000) &&
           (control->steering_target_tdeg <= 6000) &&
           (control->steering_speed_tdeg_per_s == 0U);
  }

  /* Preserve the accepted range of pre-rate V2 clients. Their steering fields
     are intentionally ignored by ECU_ProtocolNormalizeControl(). */
  return (control->steering_target_tdeg >= -300) &&
         (control->steering_target_tdeg <= 300) &&
         (control->steering_speed_tdeg_per_s <= 6000U);
}

void ECU_ProtocolNormalizeControl(uint16_t flags,
                                  ECU_ControlPayloadV2 *control)
{
  if (control == NULL)
  {
    return;
  }

  if (((flags & ECU_CONTROL_FLAG_STEERING_RATE) == 0U) ||
      (control->steering_enable == 0U))
  {
    /* A disabled channel has no latent motion request. This also keeps an
       otherwise valid relay/valve command live when only its disabled
       steering target is stale. */
    control->steering_target_tdeg = 0;
    control->steering_speed_tdeg_per_s = 0U;
    control->steering_enable = 0U;
  }
}
