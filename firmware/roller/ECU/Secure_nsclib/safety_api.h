#ifndef SAFETY_API_H
#define SAFETY_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAFETY_SLOW_ADC_COUNT          9U
#define SAFETY_CURRENT_ADC_COUNT       2U
#define SAFETY_PWM_PERIOD_COUNTS       6250U
#define SAFETY_PWM_MAX_COMPARE         (SAFETY_PWM_PERIOD_COUNTS - 1U)
#define SAFETY_ACTUATOR_API_VERSION    3UL
#define SAFETY_COMMAND_TIMEOUT_MS      300UL
#define SAFETY_ENGINE_START_MAX_MS     3000UL
#define SAFETY_STEERING_VELOCITY_MAX_TDEG_PER_S 6000
#define SAFETY_STEERING_MANUAL_MAX_MS  2000UL
#define SAFETY_STEERING_FLAG_RATE_MODE (1U << 0)
#define SAFETY_STEERING_ALLOWED_FLAGS  SAFETY_STEERING_FLAG_RATE_MODE
#define SAFETY_STEERING_API_VERSION    1UL
#define SAFETY_J1939_API_VERSION       1UL
#define SAFETY_J1939_DATA_TIMEOUT_MS   1000UL
#define SAFETY_VALVE_CONFIG_VERSION    1UL
#define SAFETY_VALVE_TARGET_MAX_MA     2000
#define SAFETY_VALVE_TARGET_DEADBAND_MA 50
#define SAFETY_VALVE_TELEMETRY_BATCH_MAX 16U
#define SAFETY_SECURITY_API_VERSION      2UL
#define SAFETY_OTA_API_VERSION           1UL
#define SAFETY_OTA_MANIFEST_MAGIC        0x31544F52UL /* "ROT1" */
#define SAFETY_OTA_MANIFEST_SCHEMA       1UL
#define SAFETY_OTA_CHUNK_SIZE            512U

#define SAFETY_ARM_TOKEN               0x41524D21UL /* "ARM!" */
#define SAFETY_CLEAR_FAULT_TOKEN       0x434C5246UL /* "CLRF" */

enum
{
  SAFETY_STATUS_READY              = (1UL << 0),
  SAFETY_STATUS_ESTOP_ACTIVE       = (1UL << 1),
  SAFETY_STATUS_FAULT_LATCHED      = (1UL << 2),
  SAFETY_STATUS_OUTPUTS_ARMED      = (1UL << 3),
  SAFETY_STATUS_ADC_RUNNING        = (1UL << 4),
  SAFETY_STATUS_GTZC_VIOLATION     = (1UL << 5),
  SAFETY_STATUS_INTERNAL_ERROR     = (1UL << 6),
  SAFETY_STATUS_COMMAND_TIMEOUT    = (1UL << 7),
  SAFETY_STATUS_TPIC_ERROR         = (1UL << 8),
  SAFETY_STATUS_COMMAND_REJECTED   = (1UL << 9),
  SAFETY_STATUS_START_LIMIT        = (1UL << 10),
  SAFETY_STATUS_SW_I2C_BUS_FAULT   = (1UL << 11),
  SAFETY_STATUS_VALVE_OVERCURRENT  = (1UL << 12),
  SAFETY_STATUS_VALVE_OPEN_LOAD    = (1UL << 13),
  SAFETY_STATUS_VALVE_SENSOR_FAULT = (1UL << 14),
  SAFETY_STATUS_VALVE_DIRECTION_FAULT = (1UL << 15),
  SAFETY_STATUS_VALVE_CONFIG_DEFAULTED = (1UL << 16),
  SAFETY_STATUS_VALVE_CALIBRATING  = (1UL << 17),
  SAFETY_STATUS_VALVE_LIMITED      = (1UL << 18),
  SAFETY_STATUS_ATECC_MISSING      = (1UL << 19),
  SAFETY_STATUS_ATECC_UNPAIRED     = (1UL << 20),
  SAFETY_STATUS_ATECC_AUTH_FAILED  = (1UL << 21),
  SAFETY_STATUS_ATECC_AUTHENTICATED = (1UL << 22),
  SAFETY_STATUS_OTA_ACTIVE         = (1UL << 23),
  SAFETY_STATUS_OTA_READY          = (1UL << 24),
  /* Positive indication of a test-swap state. Older Secure firmware leaves
     this reserved bit clear, which is intentionally backward-compatible. */
  SAFETY_STATUS_OTA_UNCONFIRMED    = (1UL << 25),
  SAFETY_STATUS_STEERING_CAN_FAULT = (1UL << 26),
  SAFETY_STATUS_VEHICLE_CAN_FAULT  = (1UL << 27)
};

enum
{
  SAFETY_J1939_VALID_EEC1   = (1UL << 0),
  SAFETY_J1939_VALID_EEC2   = (1UL << 1),
  SAFETY_J1939_VALID_ET1    = (1UL << 2),
  SAFETY_J1939_VALID_EFL_P1 = (1UL << 3),
  SAFETY_J1939_VALID_AMB    = (1UL << 4),
  SAFETY_J1939_VALID_DM1    = (1UL << 5)
};

enum
{
  SAFETY_J1939_STATUS_READY       = (1UL << 0),
  SAFETY_J1939_STATUS_INIT_FAULT  = (1UL << 1),
  SAFETY_J1939_STATUS_RX_ERROR    = (1UL << 2),
  SAFETY_J1939_STATUS_RX_DROPPED  = (1UL << 3),
  SAFETY_J1939_STATUS_BUS_WARNING = (1UL << 4),
  SAFETY_J1939_STATUS_BUS_PASSIVE = (1UL << 5),
  SAFETY_J1939_STATUS_BUS_OFF     = (1UL << 6)
};

typedef enum
{
  SAFETY_J1939_BUS_STOPPED = 0,
  SAFETY_J1939_BUS_ACTIVE,
  SAFETY_J1939_BUS_WARNING,
  SAFETY_J1939_BUS_PASSIVE,
  SAFETY_J1939_BUS_OFF
} SAFETY_J1939BusState;

typedef enum
{
  SAFETY_STEERING_SOURCE_REMOTE = 1,
  SAFETY_STEERING_SOURCE_OPERATOR = 2,
  SAFETY_STEERING_SOURCE_AUTONOMOUS = 3
} SAFETY_SteeringSource;

typedef enum
{
  SAFETY_STEERING_STATE_DISABLED = 0,
  SAFETY_STEERING_STATE_SAFE_ZERO_PENDING,
  SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING,
  SAFETY_STEERING_STATE_ENABLE_PENDING,
  SAFETY_STEERING_STATE_ENABLE_WAIT_ACK,
  SAFETY_STEERING_STATE_ACTIVE
} SAFETY_SteeringState;

typedef enum
{
  SAFETY_STEERING_BUS_STOPPED = 0,
  SAFETY_STEERING_BUS_ACTIVE,
  SAFETY_STEERING_BUS_WARNING,
  SAFETY_STEERING_BUS_PASSIVE,
  SAFETY_STEERING_BUS_OFF
} SAFETY_SteeringBusState;

enum
{
  SAFETY_STEERING_STATUS_READY          = (1UL << 0),
  SAFETY_STEERING_STATUS_ENABLE_REQUEST = (1UL << 1),
  SAFETY_STEERING_STATUS_ENABLE_CONFIRMED = (1UL << 2),
  SAFETY_STEERING_STATUS_RATE_MODE      = (1UL << 3),
  SAFETY_STEERING_STATUS_TX_PENDING     = (1UL << 4),
  SAFETY_STEERING_STATUS_REARM_REQUIRED = (1UL << 5),
  SAFETY_STEERING_STATUS_MANUAL_LIMIT   = (1UL << 6),
  SAFETY_STEERING_STATUS_SOURCE_SWITCH  = (1UL << 7),
  SAFETY_STEERING_STATUS_COMMAND_REJECTED = (1UL << 8)
};

enum
{
  SAFETY_STEERING_FAULT_INIT     = (1UL << 0),
  SAFETY_STEERING_FAULT_TX       = (1UL << 1),
  SAFETY_STEERING_FAULT_PROTOCOL = (1UL << 2),
  SAFETY_STEERING_FAULT_BUS_OFF  = (1UL << 3),
  SAFETY_STEERING_FAULT_RAM      = (1UL << 4),
  SAFETY_STEERING_FAULT_MOTOR_DTC = (1UL << 5)
};

enum
{
  SAFETY_SECURITY_ATECC_PRESENT       = (1UL << 0),
  SAFETY_SECURITY_CONFIG_LOCKED       = (1UL << 1),
  SAFETY_SECURITY_DATA_LOCKED         = (1UL << 2),
  SAFETY_SECURITY_PAIRING_PRESENT     = (1UL << 3),
  SAFETY_SECURITY_AUTHENTICATED       = (1UL << 4),
  SAFETY_SECURITY_QUARANTINE          = (1UL << 5),
  SAFETY_SECURITY_READ_ONLY_PROBE     = (1UL << 6)
};

typedef enum
{
  SAFETY_VALVE_STATE_OFF = 0,
  SAFETY_VALVE_STATE_FORWARD,
  SAFETY_VALVE_STATE_REVERSE,
  SAFETY_VALVE_STATE_REVERSING_TO_FORWARD,
  SAFETY_VALVE_STATE_REVERSING_TO_REVERSE,
  SAFETY_VALVE_STATE_FAULT
} SAFETY_ValveState;

typedef struct
{
  uint16_t kp_permille_per_amp;
  uint16_t ki_permille_per_amp_second;
  uint16_t filter_cutoff_hz;
  uint16_t rise_slew_ma_per_s;
  uint16_t fall_slew_ma_per_s;
  uint16_t max_duty_permille;
  int32_t current_gain_ppm;
  int16_t current_offset_ma;
  uint16_t reserved;
} SAFETY_ValveChannelConfig;

typedef struct
{
  uint32_t version;
  uint32_t size;
  SAFETY_ValveChannelConfig forward;
  SAFETY_ValveChannelConfig reverse;
} SAFETY_ValveConfig;

typedef struct
{
  uint32_t active_revision;
  uint32_t persisted_generation;
  uint32_t persisted_crc32c;
  uint8_t persisted_valid;
  uint8_t dirty;
  uint8_t using_defaults;
  uint8_t reserved;
  SAFETY_ValveConfig config;
} SAFETY_ValveConfigSnapshot;

typedef struct
{
  uint32_t timestamp_us;
  int16_t requested_target_ma;
  int16_t applied_target_ma;
  uint16_t forward_current_ma;
  uint16_t reverse_current_ma;
  uint16_t forward_duty_permille;
  uint16_t reverse_duty_permille;
  uint16_t state;
  uint16_t fault_flags;
} SAFETY_ValveTelemetrySample;

typedef struct
{
  uint32_t first_sequence;
  uint32_t dropped_samples;
  uint16_t sample_count;
  uint16_t sample_period_us;
  SAFETY_ValveTelemetrySample samples[SAFETY_VALVE_TELEMETRY_BATCH_MAX];
} SAFETY_ValveTelemetryBatch;

/* TPIC6A595 logical output bit assignment. Bits 5 and 10..13 are not routed
   and are forced low by the Secure service. */
enum
{
  SAFETY_RELAY_VIB_FRONT_LEVEL       = (1UL << 0),  /* K8  */
  SAFETY_RELAY_VIB_HIGH_OVERRIDE     = (1UL << 1),  /* K2  */
  SAFETY_RELAY_HORN                  = (1UL << 2),  /* K21 */
  SAFETY_RELAY_VIB_ENABLE_OVERRIDE   = (1UL << 3),  /* K1  */
  SAFETY_RELAY_VIB_FRONT_OVERRIDE    = (1UL << 4),  /* K7  */
  SAFETY_RELAY_PUMP_1                = (1UL << 6),  /* K26 */
  SAFETY_RELAY_FRONT_LED             = (1UL << 7),  /* K15 */
  SAFETY_RELAY_VIB_REAR_OVERRIDE     = (1UL << 8),  /* K9  */
  SAFETY_RELAY_VIB_HIGH_LEVEL        = (1UL << 9),  /* K4  */
  SAFETY_RELAY_REAR_MAIN             = (1UL << 14), /* K23 */
  SAFETY_RELAY_REAR_LED              = (1UL << 15), /* K16 */
  SAFETY_RELAY_ENGINE_SPEED_OVERRIDE = (1UL << 16), /* K3; temporary engine-speed takeover */
  SAFETY_RELAY_VIB_LOW_LEVEL         = (1UL << 17), /* K6  */
  SAFETY_RELAY_VIB_LOW_OVERRIDE      = (1UL << 18), /* K5  */
  SAFETY_RELAY_VIB_REAR_LEVEL        = (1UL << 19), /* K10 */
  SAFETY_RELAY_REVERSE_ALARM         = (1UL << 20), /* K17 */
  SAFETY_RELAY_PARK_BRAKE_OVERRIDE   = (1UL << 21), /* K11 */
  SAFETY_RELAY_ESTOP_RUN_PERMIT      = (1UL << 22), /* K12; 1=run circuit */
  SAFETY_RELAY_LEFT_TURN             = (1UL << 23), /* K18 */
  SAFETY_RELAY_ENGINE_SPEED_DOWN     = (1UL << 24), /* K25; PGND/engine-low pulse */
  SAFETY_RELAY_TRAVEL_SPEED_LEVEL    = (1UL << 25), /* K14; GND=rabbit */
  SAFETY_RELAY_TRAVEL_SPEED_OVERRIDE = (1UL << 26), /* K13; turtle/rabbit takeover */
  SAFETY_RELAY_ENGINE_SPEED_UP       = (1UL << 27), /* K24; +BAT/engine-high pulse */
  SAFETY_RELAY_RIGHT_TURN            = (1UL << 28), /* K19 */
  SAFETY_RELAY_FRONT_MAIN            = (1UL << 29), /* K22 */
  SAFETY_RELAY_PUMP_2                = (1UL << 30), /* K27 */
  SAFETY_RELAY_ENGINE_START          = (1UL << 31)  /* K20 */
};

#define SAFETY_RELAY_RESERVED_MASK \
  ((1UL << 5) | (1UL << 10) | (1UL << 11) | (1UL << 12) | (1UL << 13))

typedef enum
{
  SAFETY_RESULT_OK                  = 0,
  SAFETY_RESULT_BAD_ARGUMENT        = -1,
  SAFETY_RESULT_NOT_READY           = -2,
  SAFETY_RESULT_ESTOP_ACTIVE        = -3,
  SAFETY_RESULT_FAULT_LATCHED       = -4,
  SAFETY_RESULT_NOT_ARMED           = -5,
  SAFETY_RESULT_RANGE               = -6,
  SAFETY_RESULT_DIRECTION           = -7,
  SAFETY_RESULT_STALE_SEQUENCE      = -8,
  SAFETY_RESULT_INTERNAL_ERROR      = -9,
  SAFETY_RESULT_UNSUPPORTED_VERSION = -10,
  SAFETY_RESULT_CONFLICT            = -11,
  SAFETY_RESULT_TPIC_ERROR          = -12,
  SAFETY_RESULT_BUSY                = -13,
  SAFETY_RESULT_NOT_PERSISTED       = -14,
  SAFETY_RESULT_RATE_LIMITED        = -15,
  SAFETY_RESULT_AUTHENTICATION      = -16,
  SAFETY_RESULT_INTEGRITY           = -17,
  SAFETY_RESULT_STORAGE             = -18,
  SAFETY_RESULT_ROLLBACK            = -19
} SAFETY_Result;

typedef struct
{
  uint32_t status;
  uint32_t slow_sequence;
  uint32_t current_sequence;
  uint16_t slow_adc[SAFETY_SLOW_ADC_COUNT];
  uint16_t current_adc[SAFETY_CURRENT_ADC_COUNT];
  uint16_t reserved;
} SAFETY_AdcSnapshot;

/* High-level boundary between NonSecure communications and the Secure
   actuator owner. No raw TPIC mask is accepted across this boundary. */
typedef struct
{
  uint32_t api_version;
  uint32_t sequence;
  int16_t valve_current_target_ma;
  uint16_t reserved_current;
  uint8_t pump_enable;
  uint8_t pump_select;
  uint8_t headlamp_front_on;
  uint8_t headlamp_rear_on;
  uint8_t led_front_on;
  uint8_t led_rear_on;
  uint8_t buzzer_reverse_on;
  uint8_t buzzer_main_on;
  uint8_t vib_strong_on;
  uint8_t vib_weak_on;
  uint8_t vib_front_selected;
  uint8_t vib_rear_selected;
  uint8_t engine_start_request;
  uint8_t speed_mode_high;
  int8_t engine_speed_level;
  uint8_t parking_brake_on;
  /* Physical output semantic, deliberately distinct from the legacy UDP
     emergency_stop_on field: one energizes K12 and closes the run-permit
     circuit; zero is the fail-safe/open state. */
  uint8_t run_permit_on;
  uint8_t turn_signal_right_on;
  uint8_t turn_signal_left_on;
  uint8_t steering_enable;
  uint8_t steering_source;
  uint8_t steering_flags;
  int16_t steering_velocity_tdeg_per_s;
} SAFETY_ActuatorCommand;

typedef struct
{
  uint32_t status;
  uint32_t requested_relay_mask;
  uint32_t applied_relay_mask;
  uint32_t last_command_sequence;
  uint32_t secure_uptime_ms;
  uint32_t command_age_ms;
  int16_t valve_requested_target_ma;
  int16_t valve_applied_target_ma;
  uint16_t forward_current_ma;
  uint16_t reverse_current_ma;
  uint16_t forward_duty_permille;
  uint16_t reverse_duty_permille;
  uint16_t engine_start_remaining_ms;
  int8_t engine_speed_level;
  int8_t engine_speed_remaining;
  uint8_t engine_speed_state;
  uint8_t physical_estop_active;
  uint8_t software_i2c_bus_ok;
  uint8_t valve_state;
  uint8_t valve_config_dirty;
  uint32_t valve_active_revision;
  uint32_t valve_persisted_generation;
  uint32_t valve_fault_flags;
} SAFETY_ActuatorSnapshot;

/* Versioned separately from SAFETY_ActuatorSnapshot so a mixed Secure/
   NonSecure development image can never overrun the frozen 60-byte v1
   actuator object. The dedicated NSC call receives the requested version and
   output capacity as scalar arguments before validating this object. */
typedef struct
{
  uint32_t api_version;
  uint32_t size;
  uint32_t status_flags;
  uint32_t fault_flags;
  uint32_t command_sequence;
  uint32_t timestamp_ms;
  uint32_t command_age_ms;
  uint32_t rx_age_ms;
  uint32_t tx_frames;
  uint32_t rx_frames;
  uint32_t tx_errors;
  uint32_t rx_errors;
  uint32_t tx_deferred;
  uint32_t bus_off_events;
  int16_t requested_velocity_tdeg_per_s;
  int16_t applied_velocity_tdeg_per_s;
  int16_t speed_command_permille;
  int16_t motor_speed_feedback_raw;
  uint16_t motor_fault_code;
  uint8_t source;
  uint8_t state;
  uint8_t bus_state;
  uint8_t command_enable;
  uint8_t motor_enable_confirmed;
  uint8_t reserved[1];
} SAFETY_SteeringSnapshot;

/* Versioned read-only vehicle-bus boundary. Raw CAN frames and transmit
   primitives never cross into NonSecure; only the six reviewed J1939 PGNs
   are decoded in Secure and copied through the dedicated NSC service. */
typedef struct
{
  uint32_t api_version;
  uint32_t size;
  uint32_t sequence;
  uint32_t timestamp_ms;
  uint32_t status_flags;
  uint32_t valid_mask;
  uint32_t rx_frames;
  uint32_t rx_errors;
  uint32_t rx_dropped;
  uint32_t bus_off_events;
  uint32_t eec1_age_ms;
  uint32_t eec2_age_ms;
  uint32_t et1_age_ms;
  uint32_t eflp1_age_ms;
  uint32_t amb_age_ms;
  uint32_t dm1_age_ms;
  uint16_t engine_rpm;
  uint16_t oil_pressure_kpa;
  uint16_t fuel_pressure_kpa;
  int16_t coolant_temp_cdeg;
  int16_t fuel_temp_cdeg;
  int16_t ambient_temp_cdeg;
  uint8_t engine_torque_percent;
  uint8_t driver_demand_percent;
  uint8_t accelerator_pedal_percent;
  uint8_t engine_load_percent;
  uint8_t dtc_count;
  uint8_t bus_state;
  uint8_t reserved[2];
} SAFETY_J1939Snapshot;

typedef struct
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
} SAFETY_SecurityStatus;

enum
{
  SAFETY_OTA_FLAG_ENCRYPTED_IMAGES = (1UL << 0),
  SAFETY_OTA_FLAG_TEST_SWAP = (1UL << 1)
};

typedef enum
{
  SAFETY_OTA_STATE_IDLE = 0,
  SAFETY_OTA_STATE_RECEIVING,
  SAFETY_OTA_STATE_READY,
  SAFETY_OTA_STATE_ERROR
} SAFETY_OtaState;

/* The ECDSA signature in SAFETY_OtaBeginRequest covers the SHA-256 digest of
 * every byte in this canonical 128-byte manifest.  A separate OEMiROT root
 * authenticates each encrypted MCUboot image again before execution. */
typedef struct
{
  uint32_t magic;
  uint32_t schema;
  uint32_t layout_version;
  uint32_t update_sequence;
  uint32_t version_major;
  uint32_t version_minor;
  uint32_t version_revision;
  uint32_t version_build;
  uint32_t security_counter;
  uint32_t flags;
  uint32_t secure_image_size;
  uint32_t nonsecure_image_size;
  uint8_t secure_sha256[32];
  uint8_t nonsecure_sha256[32];
  uint32_t reserved[4];
} SAFETY_OtaManifest;

typedef struct
{
  SAFETY_OtaManifest manifest;
  uint8_t signature[64];
} SAFETY_OtaBeginRequest;

typedef struct
{
  uint32_t update_sequence;
  uint8_t image_index;
  uint8_t reserved0[3];
  uint32_t offset;
  uint16_t data_size;
  uint16_t reserved1;
  uint32_t data_crc32c;
  uint8_t data[SAFETY_OTA_CHUNK_SIZE];
} SAFETY_OtaChunk;

typedef struct
{
  uint32_t api_version;
  int32_t result;
  uint32_t state;
  uint32_t update_sequence;
  uint32_t accepted_sequence;
  uint32_t secure_received;
  uint32_t nonsecure_received;
  uint32_t secure_image_size;
  uint32_t nonsecure_image_size;
} SAFETY_OtaStatus;

#if defined(ECU_FACTORY_PROVISIONING)
#define SAFETY_FACTORY_PROVISION_TOKEN 0x4B434F4CUL /* "LOCK" */

enum
{
  SAFETY_FACTORY_PHASE_PROBED = (1UL << 0),
  SAFETY_FACTORY_PHASE_JOURNALED = (1UL << 1),
  SAFETY_FACTORY_PHASE_CONFIG_LOCKED = (1UL << 2),
  SAFETY_FACTORY_PHASE_DATA_LOCKED = (1UL << 3),
  SAFETY_FACTORY_PHASE_SLOT_LOCKED = (1UL << 4),
  SAFETY_FACTORY_PHASE_MANIFEST_SAVED = (1UL << 5),
  SAFETY_FACTORY_PHASE_AUTHENTICATED = (1UL << 6)
};

typedef struct
{
  uint32_t authorization_token;
  uint32_t expected_initial_config_crc32c;
  uint32_t expected_mcu_uid[3];
  uint8_t expected_atecc_serial[9];
  uint8_t reserved[3];
} SAFETY_FactoryProvisionRequest;

typedef struct
{
  int32_t result;
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
} SAFETY_FactoryStatus;
#endif

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_API_H */
