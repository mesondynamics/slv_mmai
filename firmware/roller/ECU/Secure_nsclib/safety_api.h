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
#define SAFETY_ACTUATOR_API_VERSION    2UL
#define SAFETY_COMMAND_TIMEOUT_MS      300UL
#define SAFETY_ENGINE_START_MAX_MS     3000UL
#define SAFETY_VALVE_CONFIG_VERSION    1UL
#define SAFETY_VALVE_TARGET_MAX_MA     2000
#define SAFETY_VALVE_TARGET_DEADBAND_MA 50
#define SAFETY_VALVE_TELEMETRY_BATCH_MAX 16U

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
  SAFETY_STATUS_VALVE_LIMITED      = (1UL << 18)
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
  SAFETY_RESULT_RATE_LIMITED        = -15
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
  uint8_t reserved[5];
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

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_API_H */
