#ifndef ECU_PROTOCOL_H
#define ECU_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "safety_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ECU_V2_MAGIC                 0x32554345UL /* bytes "ECU2" */
#define ECU_V2_VERSION               2U
#define ECU_V2_HEADER_SIZE           24U
#define ECU_V2_MAX_FRAME_SIZE        768U

#define ECU_V1_FRAME_START           0xA5U
#define ECU_V1_FRAME_STOP            0x5AU
#define ECU_V1_FRAME_OVERHEAD        5U
#define ECU_V1_CONTROL_PAYLOAD_SIZE  39U
#define ECU_V1_STATUS_PAYLOAD_SIZE   77U

#define ECU_CONTROL_FLAG_CLEAR_FAULT    (1U << 0)
#define ECU_CONTROL_FLAG_RELEASE        (1U << 1)
#define ECU_CONTROL_FLAG_STEERING_RATE  (1U << 2)
#define ECU_CONTROL_FLAG_ESTOP_RESET    (1U << 3)
#define ECU_CONTROL_ALLOWED_FLAGS       (ECU_CONTROL_FLAG_CLEAR_FAULT | \
                                         ECU_CONTROL_FLAG_RELEASE | \
                                         ECU_CONTROL_FLAG_STEERING_RATE | \
                                         ECU_CONTROL_FLAG_ESTOP_RESET)

typedef enum
{
  ECU_MESSAGE_CONTROL_COMMAND = 0x01,
  ECU_MESSAGE_STATUS = 0x02,
  ECU_MESSAGE_DIAGNOSTIC = 0x03,
  ECU_MESSAGE_VALVE_TELEMETRY = 0x04,
  ECU_MESSAGE_SECURITY_STATUS = 0x05,
  ECU_MESSAGE_STEERING_STATUS = 0x06,
  ECU_MESSAGE_VALVE_CONFIG_GET = 0x10,
  ECU_MESSAGE_VALVE_CONFIG_APPLY = 0x11,
  ECU_MESSAGE_VALVE_CONFIG_SAVE = 0x12,
  ECU_MESSAGE_VALVE_CONFIG_RELOAD = 0x13,
  ECU_MESSAGE_VALVE_CONFIG_REPLY = 0x14,
  ECU_MESSAGE_TELEMETRY_SUBSCRIBE = 0x15,
  ECU_MESSAGE_OPERATION_ACK = 0x16,
  ECU_MESSAGE_TELEMETRY_UNSUBSCRIBE = 0x17,
  ECU_MESSAGE_OTA_STATUS = 0x40,
  ECU_MESSAGE_OTA_BEGIN = 0x41,
  ECU_MESSAGE_OTA_CHUNK = 0x42,
  ECU_MESSAGE_OTA_FINISH = 0x43,
#if defined(ECU_FACTORY_PROVISIONING)
  ECU_MESSAGE_FACTORY_ATECC_STATUS = 0x30,
  ECU_MESSAGE_FACTORY_ATECC_PROVISION = 0x31
#endif
} ECU_MessageType;

typedef struct __attribute__((packed))
{
  uint32_t magic;
  uint8_t version;
  uint8_t message_type;
  uint16_t header_size;
  uint16_t payload_size;
  uint16_t flags;
  uint32_t sequence;
  uint32_t timestamp_ms;
  uint32_t crc32c;
} ECU_V2Header;

typedef struct __attribute__((packed))
{
  uint8_t pump_enable;
  uint8_t pump_select;
  uint8_t headlamp_front_on;
  uint8_t headlamp_rear_on;
  uint8_t led_front_on;
  uint8_t led_rear_on;
  uint8_t buzzer_reverse_on;
  uint8_t buzzer_main_on;
  uint8_t indicator1_on;
  uint8_t indicator2_on;
  uint8_t indicator3_on;
  uint8_t vib_original_on;
  uint8_t vib_strong_on;
  uint8_t vib_weak_on;
  uint8_t vib_front_selected;
  uint8_t vib_rear_selected;
  uint8_t engine_start_request;
  uint8_t power_latch_on;
  uint8_t speed_mode_high;
  int8_t engine_speed_level;
  int16_t steering_target_tdeg;
  uint16_t steering_speed_tdeg_per_s;
  uint8_t steering_enable;
  int16_t valve_current_target_ma;
  uint8_t parking_brake_on;
  uint8_t emergency_stop_request;
  uint8_t main_power_relay_on;
  uint8_t turn_signal_right_on;
  uint8_t turn_signal_left_on;
} ECU_ControlPayloadV2;

typedef struct __attribute__((packed))
{
  uint8_t sender_id;
  uint8_t priority;
  uint16_t reserved;
  ECU_ControlPayloadV2 control;
} ECU_ControlDatagramV2;

/* Frozen compatibility ABI used by the previous ECU. The two-byte frame
   length is big-endian; every multi-byte field in these packed payloads is
   little-endian on the wire. A V1 control datagram has no timestamp. */
typedef struct __attribute__((packed))
{
  uint8_t pump_enable;
  uint8_t pump_select;
  uint8_t headlamp_front_on;
  uint8_t headlamp_rear_on;
  uint8_t led_front_on;
  uint8_t led_rear_on;
  uint8_t buzzer_reverse_on;
  uint8_t buzzer_main_on;
  uint8_t indicator1_on;
  uint8_t indicator2_on;
  uint8_t indicator3_on;
  uint8_t vib_original_on;
  uint8_t vib_strong_on;
  uint8_t vib_weak_on;
  uint8_t vib_front_selected;
  uint8_t vib_rear_selected;
  uint8_t engine_start_request;
  uint8_t power_latch_on;
  uint8_t speed_mode_high;
  int8_t engine_speed_level;
  int16_t steering_target_tdeg;
  uint16_t steering_speed_tdeg_per_s;
  uint8_t steering_enable;
  int8_t throttle_percent;
  uint8_t parking_brake_on;
  uint8_t emergency_stop_on;
  uint8_t main_power_relay_on;
  uint8_t turn_signal_right_on;
  uint8_t turn_signal_left_on;
} ECU_ControlPayloadV1;

typedef struct __attribute__((packed))
{
  uint8_t sender_id;
  uint8_t priority;
  uint16_t flags;
  uint32_t sequence;
  ECU_ControlPayloadV1 control;
} ECU_ControlDatagramV1;

typedef struct __attribute__((packed))
{
  uint32_t sequence_id;
  uint32_t timestamp_ms;
  uint16_t steering_raw;
  int16_t steering_cdeg;
  uint16_t steering_mv;
  uint16_t water_mv;
  uint16_t oil_temperature_mv;
  uint16_t oil_level_mv;
  uint16_t reserved_mv;
  uint16_t engine_signal_mv;
  uint16_t engine_signal_raw;
  uint16_t speed_frequency_centi_hz;
  uint16_t vehicle_speed_centi_kph;
  uint8_t speed_signal_present;
  uint8_t engine_running;
  uint8_t pump_enabled;
  uint8_t pump_select;
  uint8_t headlamp_front_on;
  uint8_t headlamp_rear_on;
  uint8_t led_front_on;
  uint8_t led_rear_on;
  uint8_t buzzer_reverse_on;
  uint8_t buzzer_main_on;
  uint8_t indicator1_on;
  uint8_t indicator2_on;
  uint8_t indicator3_on;
  uint8_t vib_original_on;
  uint8_t vib_strong_on;
  uint8_t vib_weak_on;
  uint8_t vib_front_selected;
  uint8_t vib_rear_selected;
  uint8_t engine_start_request_on;
  uint8_t speed_mode_high;
  int8_t engine_speed_level;
  uint8_t power_latch_on;
  uint8_t parking_brake_on;
  uint8_t emergency_stop_on;
  uint8_t main_power_relay_on;
  uint8_t turn_signal_right_on;
  uint8_t turn_signal_left_on;
  uint8_t control_mode;
  uint8_t active_sender_id;
  uint16_t j1939_engine_rpm;
  uint8_t j1939_engine_torque_percent;
  uint8_t j1939_engine_load_percent;
  int16_t j1939_coolant_temp_cdeg;
  uint16_t j1939_oil_pressure_kpa;
  uint8_t j1939_accelerator_percent;
  int16_t j1939_fuel_temp_cdeg;
  uint16_t j1939_fuel_pressure_kpa;
  int16_t j1939_ambient_temp_cdeg;
  uint8_t j1939_dtc_count;
  uint8_t j1939_data_valid;
  int16_t valve_requested_target_ma;
  int16_t valve_applied_target_ma;
  uint16_t forward_current_ma;
  uint16_t reverse_current_ma;
  uint16_t forward_duty_permille;
  uint16_t reverse_duty_permille;
  uint8_t valve_state;
  uint8_t valve_config_dirty;
  uint16_t reserved;
  uint32_t valve_active_revision;
  uint32_t valve_persisted_generation;
} ECU_StatusPayloadV2;

typedef struct __attribute__((packed))
{
  uint32_t sequence_id;
  uint32_t timestamp_ms;
  uint16_t steering_raw;
  int16_t steering_cdeg;
  uint16_t steering_mv;
  uint16_t water_mv;
  uint16_t oil_temperature_mv;
  uint16_t oil_level_mv;
  uint16_t reserved_mv;
  uint16_t engine_signal_mv;
  uint16_t engine_signal_raw;
  uint16_t speed_frequency_centi_hz;
  uint16_t vehicle_speed_centi_kph;
  uint8_t speed_signal_present;
  uint8_t engine_running;
  uint8_t pump_enabled;
  uint8_t pump_select;
  uint8_t headlamp_front_on;
  uint8_t headlamp_rear_on;
  uint8_t led_front_on;
  uint8_t led_rear_on;
  uint8_t buzzer_reverse_on;
  uint8_t buzzer_main_on;
  uint8_t indicator1_on;
  uint8_t indicator2_on;
  uint8_t indicator3_on;
  uint8_t vib_original_on;
  uint8_t vib_strong_on;
  uint8_t vib_weak_on;
  uint8_t vib_front_selected;
  uint8_t vib_rear_selected;
  uint8_t engine_start_request_on;
  uint8_t speed_mode_high;
  int8_t engine_speed_level;
  uint8_t power_latch_on;
  int8_t throttle_percent;
  uint8_t parking_brake_on;
  uint8_t emergency_stop_on;
  uint8_t main_power_relay_on;
  uint8_t turn_signal_right_on;
  uint8_t turn_signal_left_on;
  uint8_t control_mode;
  uint8_t active_sender_id;
  uint16_t j1939_engine_rpm;
  uint8_t j1939_engine_torque_percent;
  uint8_t j1939_engine_load_percent;
  int16_t j1939_coolant_temp_cdeg;
  uint16_t j1939_oil_pressure_kpa;
  uint8_t j1939_accelerator_percent;
  int16_t j1939_fuel_temp_cdeg;
  uint16_t j1939_fuel_pressure_kpa;
  int16_t j1939_ambient_temp_cdeg;
  uint8_t j1939_dtc_count;
  uint8_t j1939_data_valid;
} ECU_StatusPayloadV1;

typedef struct __attribute__((packed))
{
  /* Secure command generation sampled by this payload. The enclosing V2
     header sequence remains the independent network-broadcast sequence. */
  uint32_t sequence_id;
  uint32_t timestamp_ms;
  int16_t requested_velocity_tdeg_per_s;
  int16_t applied_velocity_tdeg_per_s;
  int16_t speed_command_permille;
  uint16_t rx_age_ms;
  uint16_t motor_fault_code;
  int16_t motor_speed_feedback_raw;
  uint32_t status_flags;
  uint32_t fault_flags;
  uint32_t tx_frames;
  uint32_t rx_frames;
  uint32_t tx_errors;
  uint32_t rx_errors;
  uint32_t bus_off_events;
  uint8_t state;
  uint8_t bus_state;
  uint8_t command_enable;
  uint8_t motor_enable_confirmed;
} ECU_SteeringStatusPayloadV2;

typedef struct __attribute__((packed))
{
  uint32_t capability_flags;
  uint32_t safety_status;
  uint32_t requested_relay_mask;
  uint32_t applied_relay_mask;
  uint32_t secure_uptime_ms;
  uint32_t command_age_ms;
  uint32_t valid_control_frames;
  uint32_t invalid_control_frames;
  uint32_t rejected_control_frames;
  uint32_t authority_switches;
  uint32_t legacy_v1_frames_rejected;
  uint32_t telemetry_frames_sent;
  uint16_t slow_adc[SAFETY_SLOW_ADC_COUNT];
  uint16_t current_adc[SAFETY_CURRENT_ADC_COUNT];
  uint16_t engine_start_remaining_ms;
  int8_t engine_speed_remaining;
  uint8_t engine_speed_state;
  uint8_t link_up;
  uint8_t active_sender_id;
  uint8_t control_mode;
  uint8_t software_i2c_bus_ok;
  uint32_t valve_fault_flags;
  uint32_t telemetry_dropped_samples;
} ECU_DiagnosticPayloadV2;

typedef struct __attribute__((packed))
{
  uint32_t api_version;
  uint32_t flags;
  int32_t atecc_result;
  uint32_t config_crc32c;
  uint32_t mcu_uid[3];
  uint8_t serial[9];
  uint8_t revision[4];
  uint8_t i2c_address;
  uint8_t config_locked;
  uint8_t data_locked;
  uint8_t device_status;
  uint8_t reserved[3];
  int32_t auth_result;
  uint32_t pairing_generation;
} ECU_SecurityPayloadV2;

typedef struct __attribute__((packed))
{
  uint16_t destination_port;
  uint16_t sample_rate_hz;
  uint32_t ttl_ms;
} ECU_TelemetrySubscribePayload;

typedef struct __attribute__((packed))
{
  int32_t result;
  uint32_t request_sequence;
  SAFETY_ValveConfigSnapshot snapshot;
} ECU_ValveConfigReplyPayload;

typedef struct __attribute__((packed))
{
  int32_t result;
  uint32_t request_sequence;
} ECU_OperationAckPayload;

typedef struct __attribute__((packed))
{
  int32_t result;
  uint32_t request_sequence;
  SAFETY_OtaStatus status;
} ECU_OtaStatusPayload;

#if defined(ECU_FACTORY_PROVISIONING)
typedef struct __attribute__((packed))
{
  int32_t result;
  uint32_t request_sequence;
  uint32_t phase_flags;
  uint32_t config_crc32c;
  uint32_t mcu_uid[3];
  uint16_t slot_locked_mask;
  uint8_t config_locked;
  uint8_t data_locked;
  uint8_t device_status;
  uint8_t private_key_slot;
  uint8_t serial[9];
  uint8_t revision[4];
  uint8_t config[128];
  uint8_t public_key[64];
} ECU_FactoryAteccStatusPayload;
#endif

uint32_t ECU_ProtocolCrc32c(const void *data, size_t length);
size_t ECU_ProtocolEncodeV1(const void *payload, uint16_t payload_size,
                            uint8_t *frame, size_t frame_capacity);
bool ECU_ProtocolDecodeV1(const uint8_t *frame, size_t frame_length,
                          void *payload, uint16_t expected_payload_size);
bool ECU_ProtocolControlV1ValuesValid(const ECU_ControlDatagramV1 *datagram);
bool ECU_ProtocolConvertControlV1(const ECU_ControlDatagramV1 *source,
                                  ECU_ControlDatagramV2 *destination);
bool ECU_ProtocolBuildStatusV1(const ECU_StatusPayloadV2 *source,
                               ECU_StatusPayloadV1 *destination);
size_t ECU_ProtocolEncodeV2(uint8_t message_type, uint16_t flags,
                            uint32_t sequence, uint32_t timestamp_ms,
                            const void *payload, uint16_t payload_size,
                            uint8_t *frame, size_t frame_capacity);
bool ECU_ProtocolDecodeV2(const uint8_t *frame, size_t frame_length,
                          ECU_V2Header *header, void *payload,
                          size_t payload_capacity);
bool ECU_ProtocolControlValuesValid(uint16_t flags,
                                    const ECU_ControlPayloadV2 *control);
void ECU_ProtocolNormalizeControl(uint16_t flags,
                                  ECU_ControlPayloadV2 *control);

#ifdef __cplusplus
}
#endif

#endif /* ECU_PROTOCOL_H */
