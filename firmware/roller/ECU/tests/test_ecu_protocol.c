#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ecu_protocol.h"

int main(void)
{
  ECU_ControlDatagramV2 source = {0};
  ECU_ControlDatagramV2 decoded = {0};
  ECU_V2Header header = {0};
  uint8_t frame[ECU_V2_HEADER_SIZE + sizeof(source)];
  static const uint8_t crc_vector[] = "123456789";
  size_t length;

  assert(sizeof(ECU_V2Header) == 24U);
  assert(sizeof(ECU_ControlPayloadV1) == 31U);
  assert(sizeof(ECU_ControlDatagramV1) == 39U);
  assert(sizeof(ECU_StatusPayloadV1) == 77U);
  assert(sizeof(ECU_ControlDatagramV2) == 36U);
  assert(sizeof(ECU_StatusPayloadV2) == 100U);
  assert(sizeof(ECU_SteeringStatusPayloadV2) == 52U);
  assert(sizeof(ECU_DiagnosticPayloadV2) == 86U);
  assert(sizeof(ECU_ValveConfigReplyPayload) == 72U);
  assert(SAFETY_ACTUATOR_API_VERSION == 3UL);
  assert(SAFETY_STEERING_API_VERSION == 1UL);
  assert(sizeof(SAFETY_ActuatorCommand) == 36U);
  assert(sizeof(SAFETY_ActuatorSnapshot) == 60U);
  assert(sizeof(SAFETY_SteeringSnapshot) == 72U);
  assert(offsetof(SAFETY_ActuatorCommand, steering_enable) == 31U);
  assert(offsetof(SAFETY_ActuatorCommand, steering_source) == 32U);
  assert(offsetof(SAFETY_ActuatorCommand, steering_flags) == 33U);
  assert(offsetof(SAFETY_ActuatorCommand,
                  steering_velocity_tdeg_per_s) == 34U);
  assert(ECU_CONTROL_FLAG_STEERING_RATE == (1U << 2));
  assert(ECU_MESSAGE_STEERING_STATUS == 0x06);
  assert(offsetof(ECU_ControlPayloadV2, steering_target_tdeg) == 20U);
  assert(offsetof(ECU_ControlPayloadV2, steering_speed_tdeg_per_s) == 22U);
  assert(offsetof(ECU_ControlPayloadV2, steering_enable) == 24U);
  assert(offsetof(ECU_ControlDatagramV2, control) == 4U);
  assert(offsetof(ECU_SteeringStatusPayloadV2,
                  requested_velocity_tdeg_per_s) == 8U);
  assert(offsetof(ECU_SteeringStatusPayloadV2,
                  speed_command_permille) == 12U);
  assert(offsetof(ECU_SteeringStatusPayloadV2,
                  motor_speed_feedback_raw) == 18U);
  assert(offsetof(ECU_SteeringStatusPayloadV2, status_flags) == 20U);
  assert(offsetof(ECU_SteeringStatusPayloadV2, tx_frames) == 28U);
  assert(offsetof(ECU_SteeringStatusPayloadV2, state) == 48U);
  assert(ECU_ProtocolCrc32c(crc_vector, sizeof(crc_vector) - 1U) ==
         0xE3069283UL);

  {
    /* Generated independently by the previous release's ccu_protocol.py.
       This freezes A5 framing, BE length, LE packed fields, XOR and stop byte. */
    static const uint8_t legacy_golden[] = {
      0xA5, 0x00, 0x27, 0x02, 0x02, 0x00, 0x00, 0x78, 0x56, 0x34,
      0x12, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x01,
      0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
      0x01, 0x00, 0xF7, 0x5A
    };
    ECU_ControlDatagramV1 legacy = {0};
    ECU_ControlDatagramV2 mapped = {0};
    uint8_t encoded[sizeof(legacy_golden)] = {0};

    assert(sizeof(legacy_golden) == 44U);
    assert(ECU_ProtocolDecodeV1(legacy_golden, sizeof(legacy_golden),
                                &legacy, sizeof(legacy)));
    assert(legacy.sender_id == 2U);
    assert(legacy.priority == 2U);
    assert(legacy.flags == 0U);
    assert(legacy.sequence == 0x12345678UL);
    assert(legacy.control.pump_enable == 1U);
    assert(legacy.control.headlamp_front_on == 1U);
    assert(legacy.control.buzzer_main_on == 1U);
    assert(legacy.control.vib_weak_on == 1U);
    assert(legacy.control.vib_rear_selected == 1U);
    assert(legacy.control.engine_start_request == 1U);
    assert(legacy.control.speed_mode_high == 1U);
    assert(legacy.control.engine_speed_level == -2);
    assert(legacy.control.throttle_percent == 0);
    assert(legacy.control.parking_brake_on == 1U);
    assert(legacy.control.turn_signal_right_on == 1U);
    assert(ECU_ProtocolControlV1ValuesValid(&legacy));
    assert(ECU_ProtocolConvertControlV1(&legacy, &mapped));
    assert(mapped.sender_id == legacy.sender_id);
    assert(mapped.priority == legacy.priority);
    assert(mapped.reserved == 0U);
    assert(mapped.control.pump_enable == 1U);
    assert(mapped.control.engine_speed_level == -2);
    assert(mapped.control.valve_current_target_ma == 0);
    assert(mapped.control.steering_target_tdeg == 0);
    assert(mapped.control.steering_speed_tdeg_per_s == 0U);
    assert(mapped.control.steering_enable == 0U);
    assert(ECU_ProtocolEncodeV1(&legacy, sizeof(legacy), encoded,
                                sizeof(encoded)) == sizeof(encoded));
    assert(memcmp(encoded, legacy_golden, sizeof(encoded)) == 0);

    encoded[3U] ^= 0x01U;
    assert(!ECU_ProtocolDecodeV1(encoded, sizeof(encoded), &legacy,
                                 sizeof(legacy)));
    encoded[3U] ^= 0x01U;
    encoded[sizeof(encoded) - 1U] = 0U;
    assert(!ECU_ProtocolDecodeV1(encoded, sizeof(encoded), &legacy,
                                 sizeof(legacy)));
    encoded[sizeof(encoded) - 1U] = ECU_V1_FRAME_STOP;
    assert(!ECU_ProtocolDecodeV1(encoded, sizeof(encoded) - 1U, &legacy,
                                 sizeof(legacy)));
    assert(ECU_ProtocolEncodeV1(&legacy, sizeof(legacy), encoded,
                                sizeof(encoded) - 1U) == 0U);
  }

  {
    ECU_ControlDatagramV1 legacy = {0};
    ECU_ControlDatagramV2 mapped = {0};

    legacy.sender_id = 1U;
    legacy.priority = 1U;
    assert(ECU_ProtocolControlV1ValuesValid(&legacy));
    legacy.control.steering_target_tdeg = 1;
    assert(!ECU_ProtocolControlV1ValuesValid(&legacy));
    assert(!ECU_ProtocolConvertControlV1(&legacy, &mapped));
    legacy.control.steering_target_tdeg = 0;
    legacy.control.steering_speed_tdeg_per_s = 1U;
    assert(!ECU_ProtocolControlV1ValuesValid(&legacy));
    legacy.control.steering_speed_tdeg_per_s = 0U;
    legacy.control.steering_enable = 1U;
    assert(!ECU_ProtocolControlV1ValuesValid(&legacy));
    legacy.control.steering_enable = 0U;
    legacy.control.throttle_percent = 1;
    assert(!ECU_ProtocolControlV1ValuesValid(&legacy));
    legacy.control.throttle_percent = 0;
    legacy.control.indicator1_on = 1U;
    assert(!ECU_ProtocolControlV1ValuesValid(&legacy));
    legacy.control.indicator1_on = 0U;
    legacy.control.power_latch_on = 1U;
    assert(!ECU_ProtocolControlV1ValuesValid(&legacy));
    legacy.control.power_latch_on = 0U;
    legacy.control.main_power_relay_on = 1U;
    assert(!ECU_ProtocolControlV1ValuesValid(&legacy));
    legacy.control.main_power_relay_on = 0U;
    legacy.flags = 1U;
    assert(!ECU_ProtocolControlV1ValuesValid(&legacy));
    legacy.flags = 0U;
    legacy.control.pump_enable = 2U;
    assert(!ECU_ProtocolControlV1ValuesValid(&legacy));
  }

  {
    ECU_StatusPayloadV2 modern = {0};
    ECU_StatusPayloadV1 legacy = {0};
    ECU_StatusPayloadV1 decoded_status = {0};
    uint8_t status_frame[ECU_V1_STATUS_PAYLOAD_SIZE +
                         ECU_V1_FRAME_OVERHEAD];

    modern.sequence_id = 0x01020304UL;
    modern.timestamp_ms = 0xA0B0C0D0UL;
    modern.water_mv = 1234U;
    modern.engine_speed_level = -3;
    modern.parking_brake_on = 1U;
    modern.control_mode = 3U;
    modern.active_sender_id = 3U;
    modern.j1939_engine_rpm = 16000U;
    modern.valve_requested_target_ma = 1500;
    assert(ECU_ProtocolBuildStatusV1(&modern, &legacy));
    assert(legacy.sequence_id == modern.sequence_id);
    assert(legacy.timestamp_ms == modern.timestamp_ms);
    assert(legacy.water_mv == modern.water_mv);
    assert(legacy.engine_speed_level == -3);
    assert(legacy.parking_brake_on == 1U);
    assert(legacy.control_mode == 3U);
    assert(legacy.active_sender_id == 3U);
    assert(legacy.j1939_engine_rpm == 16000U);
    assert(legacy.throttle_percent == 0);
    assert(ECU_ProtocolEncodeV1(&legacy, sizeof(legacy), status_frame,
                                sizeof(status_frame)) ==
           sizeof(status_frame));
    assert(status_frame[0] == ECU_V1_FRAME_START);
    assert(status_frame[1] == 0U);
    assert(status_frame[2] == ECU_V1_STATUS_PAYLOAD_SIZE);
    assert(ECU_ProtocolDecodeV1(status_frame, sizeof(status_frame),
                                &decoded_status, sizeof(decoded_status)));
    assert(memcmp(&legacy, &decoded_status, sizeof(legacy)) == 0);
    assert(!ECU_ProtocolBuildStatusV1(NULL, &legacy));
    assert(!ECU_ProtocolBuildStatusV1(&modern, NULL));
  }

  source.sender_id = 2U;
  source.priority = 2U;
  source.control.pump_enable = 1U;
  source.control.valve_current_target_ma = -1370;
  source.control.engine_speed_level = 1;
  source.control.emergency_stop_request = 1U;

  length = ECU_ProtocolEncodeV2(
      ECU_MESSAGE_CONTROL_COMMAND, ECU_CONTROL_FLAG_CLEAR_FAULT,
      0x12345678UL, 0x76543210UL, &source, sizeof(source), frame,
      sizeof(frame));
  assert(length == sizeof(frame));
  assert(frame[0] == 0x45U && frame[1] == 0x43U &&
         frame[2] == 0x55U && frame[3] == 0x32U);
  assert(ECU_ProtocolDecodeV2(frame, length, &header, &decoded,
                              sizeof(decoded)));
  assert(header.message_type == ECU_MESSAGE_CONTROL_COMMAND);
  assert(header.flags == ECU_CONTROL_FLAG_CLEAR_FAULT);
  assert(header.sequence == 0x12345678UL);
  assert(header.timestamp_ms == 0x76543210UL);
  assert(memcmp(&source, &decoded, sizeof(source)) == 0);

  frame[ECU_V2_HEADER_SIZE + 3U] ^= 0x01U;
  assert(!ECU_ProtocolDecodeV2(frame, length, &header, &decoded,
                               sizeof(decoded)));
  frame[ECU_V2_HEADER_SIZE + 3U] ^= 0x01U;
  assert(!ECU_ProtocolDecodeV2(frame, length - 1U, &header, &decoded,
                               sizeof(decoded)));
  assert(!ECU_ProtocolDecodeV2(frame, length, &header, &decoded,
                               sizeof(decoded) - 1U));
  assert(ECU_ProtocolEncodeV2(ECU_MESSAGE_CONTROL_COMMAND, 0U, 1U, 1U,
                              &source, sizeof(source), frame,
                              sizeof(frame) - 1U) == 0U);

  {
    ECU_SteeringStatusPayloadV2 sampled = {0};
    ECU_SteeringStatusPayloadV2 received = {0};
    uint8_t steering_frame[ECU_V2_HEADER_SIZE + sizeof(sampled)];

    /* The payload sequence correlates the sampled Secure command while the
       enclosing header sequence independently orders network broadcasts. */
    sampled.sequence_id = 0x11223344UL;
    sampled.timestamp_ms = 0x55667788UL;
    sampled.speed_command_permille = -500;
    sampled.motor_speed_feedback_raw = -123;
    length = ECU_ProtocolEncodeV2(
        ECU_MESSAGE_STEERING_STATUS, 0U, 0xAABBCCDDUL, 0x12345678UL,
        &sampled, sizeof(sampled), steering_frame, sizeof(steering_frame));
    assert(length == sizeof(steering_frame));
    assert(ECU_ProtocolDecodeV2(steering_frame, length, &header, &received,
                                sizeof(received)));
    assert(header.sequence == 0xAABBCCDDUL);
    assert(header.timestamp_ms == 0x12345678UL);
    assert(received.sequence_id == 0x11223344UL);
    assert(received.timestamp_ms == 0x55667788UL);
    assert(received.speed_command_permille == -500);
    assert(received.motor_speed_feedback_raw == -123);
  }

  {
    ECU_ControlPayloadV2 control = {0};

    control.pump_enable = 1U;
    control.steering_target_tdeg = 300;
    control.steering_speed_tdeg_per_s = 6000U;
    control.steering_enable = 1U;
    assert(ECU_ProtocolControlValuesValid(0U, &control));
    ECU_ProtocolNormalizeControl(0U, &control);
    assert(control.pump_enable == 1U);
    assert(control.steering_target_tdeg == 0);
    assert(control.steering_speed_tdeg_per_s == 0U);
    assert(control.steering_enable == 0U);

    control.steering_target_tdeg = -6000;
    control.steering_speed_tdeg_per_s = 0U;
    control.steering_enable = 1U;
    assert(ECU_ProtocolControlValuesValid(
        ECU_CONTROL_FLAG_STEERING_RATE, &control));
    ECU_ProtocolNormalizeControl(ECU_CONTROL_FLAG_STEERING_RATE, &control);
    assert(control.steering_target_tdeg == -6000);
    assert(control.steering_enable == 1U);

    control.steering_enable = 0U;
    ECU_ProtocolNormalizeControl(ECU_CONTROL_FLAG_STEERING_RATE, &control);
    assert(control.steering_target_tdeg == 0);
    assert(control.steering_speed_tdeg_per_s == 0U);

    control.steering_target_tdeg = -6001;
    assert(!ECU_ProtocolControlValuesValid(
        ECU_CONTROL_FLAG_STEERING_RATE, &control));
    control.steering_target_tdeg = 6000;
    control.steering_speed_tdeg_per_s = 1U;
    assert(!ECU_ProtocolControlValuesValid(
        ECU_CONTROL_FLAG_STEERING_RATE, &control));
    control.steering_speed_tdeg_per_s = 0U;
    assert(!ECU_ProtocolControlValuesValid((1U << 15), &control));
    control.steering_enable = 2U;
    assert(!ECU_ProtocolControlValuesValid(
        ECU_CONTROL_FLAG_STEERING_RATE, &control));
    assert(!ECU_ProtocolControlValuesValid(
        ECU_CONTROL_FLAG_STEERING_RATE, NULL));
  }
  return 0;
}
