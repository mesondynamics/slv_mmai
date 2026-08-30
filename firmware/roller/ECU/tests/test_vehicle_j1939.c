#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vehicle_j1939.h"

_Static_assert(SAFETY_J1939_API_VERSION == 1UL,
               "J1939 snapshot API version changed");
_Static_assert(SAFETY_J1939_DATA_TIMEOUT_MS == 1000UL,
               "J1939 freshness boundary changed");
_Static_assert(sizeof(SAFETY_J1939Snapshot) == 84U,
               "J1939 snapshot ABI changed");
_Static_assert(offsetof(SAFETY_J1939Snapshot, api_version) == 0U,
               "J1939 api_version ABI offset changed");
_Static_assert(offsetof(SAFETY_J1939Snapshot, valid_mask) == 20U,
               "J1939 valid_mask ABI offset changed");
_Static_assert(offsetof(SAFETY_J1939Snapshot, eec1_age_ms) == 40U,
               "J1939 age table ABI offset changed");
_Static_assert(offsetof(SAFETY_J1939Snapshot, engine_rpm) == 64U,
               "J1939 decoded data ABI offset changed");
_Static_assert(offsetof(SAFETY_J1939Snapshot, bus_state) == 81U,
               "J1939 bus_state ABI offset changed");
_Static_assert(offsetof(SAFETY_J1939Snapshot, reserved) == 82U,
               "J1939 reserved tail ABI offset changed");

#define ALL_VALID_MASK                                                   \
  (SAFETY_J1939_VALID_EEC1 | SAFETY_J1939_VALID_EEC2 |                 \
   SAFETY_J1939_VALID_ET1 | SAFETY_J1939_VALID_EFL_P1 |                \
   SAFETY_J1939_VALID_AMB | SAFETY_J1939_VALID_DM1)

static uint32_t make_id(uint32_t pgn, uint8_t priority, uint8_t source)
{
  return (((uint32_t)priority & 0x7UL) << 26U) |
         ((pgn & 0xFFFFUL) << 8U) | source;
}

static void assert_unseen_ages(const SAFETY_J1939Snapshot *snapshot)
{
  assert(snapshot->eec1_age_ms == UINT32_MAX);
  assert(snapshot->eec2_age_ms == UINT32_MAX);
  assert(snapshot->et1_age_ms == UINT32_MAX);
  assert(snapshot->eflp1_age_ms == UINT32_MAX);
  assert(snapshot->amb_age_ms == UINT32_MAX);
  assert(snapshot->dm1_age_ms == UINT32_MAX);
}

static void test_initialization_and_null_snapshot(void)
{
  VehicleJ1939 state;
  SAFETY_J1939Snapshot snapshot;

  memset(&snapshot, 0xA5, sizeof(snapshot));
  VehicleJ1939_GetSnapshot(NULL, 1234U, &snapshot);
  assert(snapshot.api_version == SAFETY_J1939_API_VERSION);
  assert(snapshot.size == sizeof(snapshot));
  assert(snapshot.timestamp_ms == 0U);
  assert(snapshot.status_flags == SAFETY_J1939_STATUS_INIT_FAULT);
  assert(snapshot.valid_mask == 0U);
  assert(snapshot.bus_state == SAFETY_J1939_BUS_STOPPED);
  assert_unseen_ages(&snapshot);
  assert(snapshot.reserved[0] == 0U);
  assert(snapshot.reserved[1] == 0U);

  VehicleJ1939_Init(&state);
  VehicleJ1939_GetSnapshot(&state, 77U, &snapshot);
  assert(snapshot.api_version == SAFETY_J1939_API_VERSION);
  assert(snapshot.size == sizeof(snapshot));
  assert(snapshot.timestamp_ms == 77U);
  assert(snapshot.status_flags == SAFETY_J1939_STATUS_INIT_FAULT);
  assert(snapshot.valid_mask == 0U);
  assert(snapshot.bus_state == SAFETY_J1939_BUS_STOPPED);
  assert_unseen_ages(&snapshot);

  VehicleJ1939_SetReady(&state);
  VehicleJ1939_GetSnapshot(&state, 78U, &snapshot);
  assert(snapshot.status_flags == SAFETY_J1939_STATUS_READY);
  assert(snapshot.bus_state == SAFETY_J1939_BUS_ACTIVE);

  VehicleJ1939_Init(NULL);
  VehicleJ1939_SetReady(NULL);
  VehicleJ1939_SetInitFault(NULL);
  VehicleJ1939_RecordRxError(NULL, 1U);
  VehicleJ1939_RecordDrop(NULL, 1U);
  VehicleJ1939_RecordBusState(NULL, SAFETY_J1939_BUS_ACTIVE);
  VehicleJ1939_GetSnapshot(&state, 79U, NULL);
}

static void test_six_pgn_golden_vectors(void)
{
  static const uint8_t eec1[8] = {
    0x00U, 0x00U, 200U, 225U, 0x34U, 0x12U, 0x00U, 0x00U
  };
  static const uint8_t eec2[8] = {
    0x00U, 200U, 87U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  };
  static const uint8_t et1[8] = {
    100U, 20U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  };
  static const uint8_t eflp1[8] = {
    0x00U, 0x00U, 55U, 111U, 0x00U, 0x00U, 0x00U, 0x00U
  };
  static const uint8_t amb[8] = {
    0x00U, 0x00U, 0x00U, 0x10U, 0x27U, 0x00U, 0x00U, 0x00U
  };
  static const uint8_t dm1[8] = {
    0x40U, 0x00U, 0x12U, 0x34U, 0x56U, 0x78U, 0x9AU, 0xBCU
  };
  VehicleJ1939 state;
  SAFETY_J1939Snapshot snapshot;

  VehicleJ1939_Init(&state);
  VehicleJ1939_SetReady(&state);
  state.sampled.dtc_count = 9U;

  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_EEC1, 3U, 0x44U), eec1, 100U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_EEC2, 0U, 0x00U), eec2, 200U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_ET1, 7U, 0xFFU), et1, 300U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_EFL_P1, 2U, 0x80U), eflp1, 400U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_AMB, 6U, 0x01U), amb, 500U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_DM1, 1U, 0xA5U), dm1, 600U));

  VehicleJ1939_GetSnapshot(&state, 700U, &snapshot);
  assert(snapshot.api_version == SAFETY_J1939_API_VERSION);
  assert(snapshot.size == sizeof(snapshot));
  assert(snapshot.sequence == 6U);
  assert(snapshot.timestamp_ms == 700U);
  assert(snapshot.status_flags == SAFETY_J1939_STATUS_READY);
  assert(snapshot.valid_mask == ALL_VALID_MASK);
  assert(snapshot.rx_frames == 6U);
  assert(snapshot.rx_errors == 0U);
  assert(snapshot.rx_dropped == 0U);
  assert(snapshot.bus_off_events == 0U);
  assert(snapshot.eec1_age_ms == 600U);
  assert(snapshot.eec2_age_ms == 500U);
  assert(snapshot.et1_age_ms == 400U);
  assert(snapshot.eflp1_age_ms == 300U);
  assert(snapshot.amb_age_ms == 200U);
  assert(snapshot.dm1_age_ms == 100U);
  assert(snapshot.engine_rpm == 0x1234U);
  assert(snapshot.driver_demand_percent == 75U);
  assert(snapshot.engine_torque_percent == 100U);
  assert(snapshot.accelerator_pedal_percent == 80U);
  assert(snapshot.engine_load_percent == 87U);
  assert(snapshot.coolant_temp_cdeg == 6000);
  assert(snapshot.fuel_temp_cdeg == -2000);
  assert(snapshot.oil_pressure_kpa == 444U);
  assert(snapshot.fuel_pressure_kpa == 220U);
  assert(snapshot.ambient_temp_cdeg == 3900);
  assert(snapshot.dtc_count == 0U);
  assert(snapshot.bus_state == SAFETY_J1939_BUS_ACTIVE);
  assert(snapshot.reserved[0] == 0U);
  assert(snapshot.reserved[1] == 0U);
}

static void test_priority_and_source_are_ignored(void)
{
  static const uint8_t first[8] = {
    0U, 0U, 130U, 140U, 0x01U, 0x02U, 0U, 0U
  };
  static const uint8_t second[8] = {
    0U, 0U, 150U, 160U, 0x03U, 0x04U, 0U, 0U
  };
  VehicleJ1939 state;
  SAFETY_J1939Snapshot snapshot;

  VehicleJ1939_Init(&state);
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_EEC1, 0U, 0x00U), first, 10U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_EEC1, 7U, 0xFFU), second, 20U));
  VehicleJ1939_GetSnapshot(&state, 20U, &snapshot);

  assert(snapshot.sequence == 2U);
  assert(snapshot.rx_frames == 2U);
  assert(snapshot.valid_mask == SAFETY_J1939_VALID_EEC1);
  assert(snapshot.eec1_age_ms == 0U);
  assert(snapshot.engine_rpm == 0x0403U);
  assert(snapshot.driver_demand_percent == 25U);
  assert(snapshot.engine_torque_percent == 35U);
}

static void test_identifier_boundary_rejection_is_side_effect_free(void)
{
  static const uint8_t data[8] = {
    0U, 0U, 130U, 140U, 0x01U, 0x02U, 0U, 0U
  };
  VehicleJ1939 state;
  VehicleJ1939 before;
  uint32_t base_id = make_id(VEHICLE_J1939_PGN_EEC1, 3U, 0x55U);

  VehicleJ1939_Init(&state);
  VehicleJ1939_SetReady(&state);
  before = state;

  assert(!VehicleJ1939_HandleFrame(&state, base_id | (1UL << 25U),
                                   data, 1U));
  assert(memcmp(&state, &before, sizeof(state)) == 0);
  assert(!VehicleJ1939_HandleFrame(&state, base_id | (1UL << 24U),
                                   data, 2U));
  assert(memcmp(&state, &before, sizeof(state)) == 0);
  assert(!VehicleJ1939_HandleFrame(
      &state, make_id(0xFE00UL, 3U, 0x55U), data, 3U));
  assert(memcmp(&state, &before, sizeof(state)) == 0);
  assert(!VehicleJ1939_HandleFrame(&state, 0x20000000UL, data, 4U));
  assert(memcmp(&state, &before, sizeof(state)) == 0);
  assert(!VehicleJ1939_HandleFrame(NULL, base_id, data, 5U));
  assert(!VehicleJ1939_HandleFrame(&state, base_id, NULL, 6U));
  assert(memcmp(&state, &before, sizeof(state)) == 0);
}

static void test_sentinel_values_preserve_previous_samples(void)
{
  static const uint8_t eec1_value[8] = {
    0U, 0U, 160U, 170U, 0x22U, 0x11U, 0U, 0U
  };
  static const uint8_t eec2_value[8] = {
    0U, 200U, 90U, 0U, 0U, 0U, 0U, 0U
  };
  static const uint8_t et1_value[8] = {
    80U, 70U, 0U, 0U, 0U, 0U, 0U, 0U
  };
  static const uint8_t eflp1_value[8] = {
    0U, 0U, 25U, 50U, 0U, 0U, 0U, 0U
  };
  static const uint8_t amb_value[8] = {
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U
  };
  static const uint8_t eec1_sentinel[8] = {
    0U, 0U, 251U, 255U, 0xFFU, 0xFFU, 0U, 0U
  };
  static const uint8_t eec2_sentinel[8] = {
    0U, 251U, 255U, 0U, 0U, 0U, 0U, 0U
  };
  static const uint8_t et1_sentinel[8] = {
    251U, 255U, 0U, 0U, 0U, 0U, 0U, 0U
  };
  static const uint8_t eflp1_sentinel[8] = {
    0U, 0U, 251U, 255U, 0U, 0U, 0U, 0U
  };
  static const uint8_t amb_sentinel[8] = {
    0U, 0U, 0U, 0x00U, 0xFBU, 0U, 0U, 0U
  };
  VehicleJ1939 state;
  SAFETY_J1939Snapshot before;
  SAFETY_J1939Snapshot after;

  VehicleJ1939_Init(&state);
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_EEC1, 3U, 1U), eec1_value, 1U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_EEC2, 3U, 1U), eec2_value, 2U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_ET1, 3U, 1U), et1_value, 3U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_EFL_P1, 3U, 1U), eflp1_value, 4U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_AMB, 3U, 1U), amb_value, 5U));
  VehicleJ1939_GetSnapshot(&state, 5U, &before);

  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_EEC1, 3U, 1U), eec1_sentinel, 10U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_EEC2, 3U, 1U), eec2_sentinel, 11U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_ET1, 3U, 1U), et1_sentinel, 12U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_EFL_P1, 3U, 1U), eflp1_sentinel,
      13U));
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_AMB, 3U, 1U), amb_sentinel, 14U));
  VehicleJ1939_GetSnapshot(&state, 14U, &after);

  assert(after.engine_rpm == before.engine_rpm);
  assert(after.driver_demand_percent == before.driver_demand_percent);
  assert(after.engine_torque_percent == before.engine_torque_percent);
  assert(after.accelerator_pedal_percent ==
         before.accelerator_pedal_percent);
  assert(after.engine_load_percent == before.engine_load_percent);
  assert(after.coolant_temp_cdeg == before.coolant_temp_cdeg);
  assert(after.fuel_temp_cdeg == before.fuel_temp_cdeg);
  assert(after.oil_pressure_kpa == before.oil_pressure_kpa);
  assert(after.fuel_pressure_kpa == before.fuel_pressure_kpa);
  assert(after.ambient_temp_cdeg == before.ambient_temp_cdeg);
  assert(after.sequence == 10U);
  assert(after.rx_frames == 10U);
  assert(after.valid_mask ==
         (ALL_VALID_MASK & ~SAFETY_J1939_VALID_DM1));
  assert(after.eec1_age_ms == 4U);
  assert(after.eec2_age_ms == 3U);
  assert(after.et1_age_ms == 2U);
  assert(after.eflp1_age_ms == 1U);
  assert(after.amb_age_ms == 0U);
}

static void test_exact_timeout_boundary(void)
{
  static const uint8_t eec1[8] = {
    0U, 0U, 125U, 125U, 0x01U, 0U, 0U, 0U
  };
  VehicleJ1939 state;
  SAFETY_J1939Snapshot snapshot;

  VehicleJ1939_Init(&state);
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_EEC1, 3U, 1U), eec1, 100U));

  VehicleJ1939_GetSnapshot(&state, 1100U, &snapshot);
  assert(snapshot.eec1_age_ms == 1000U);
  assert(snapshot.valid_mask == SAFETY_J1939_VALID_EEC1);

  VehicleJ1939_GetSnapshot(&state, 1101U, &snapshot);
  assert(snapshot.eec1_age_ms == 1001U);
  assert(snapshot.valid_mask == 0U);
  assert(snapshot.engine_rpm == 1U);
}

static void test_time_and_counter_uint32_wrap(void)
{
  static const uint8_t eec1[8] = {
    0U, 0U, 125U, 125U, 0x02U, 0U, 0U, 0U
  };
  VehicleJ1939 state;
  SAFETY_J1939Snapshot snapshot;
  const uint32_t frame_tick = UINT32_MAX - 999U;

  VehicleJ1939_Init(&state);
  state.sampled.sequence = UINT32_MAX;
  state.sampled.rx_frames = UINT32_MAX;
  assert(VehicleJ1939_HandleFrame(
      &state, make_id(VEHICLE_J1939_PGN_EEC1, 3U, 1U), eec1, frame_tick));
  assert(state.sampled.sequence == 0U);
  assert(state.sampled.rx_frames == 0U);

  VehicleJ1939_GetSnapshot(&state, 0U, &snapshot);
  assert(snapshot.eec1_age_ms == 1000U);
  assert(snapshot.valid_mask == SAFETY_J1939_VALID_EEC1);

  VehicleJ1939_GetSnapshot(&state, 1U, &snapshot);
  assert(snapshot.eec1_age_ms == 1001U);
  assert(snapshot.valid_mask == 0U);
}

static void test_ambient_saturation(void)
{
  static const uint8_t minimum[8] = {
    0U, 0U, 0U, 0x00U, 0x00U, 0U, 0U, 0U
  };
  static const uint8_t maximum_valid[8] = {
    0U, 0U, 0U, 0xFFU, 0xFAU, 0U, 0U, 0U
  };
  VehicleJ1939 state;
  SAFETY_J1939Snapshot snapshot;
  uint32_t id = make_id(VEHICLE_J1939_PGN_AMB, 3U, 1U);

  VehicleJ1939_Init(&state);
  assert(VehicleJ1939_HandleFrame(&state, id, minimum, 1U));
  VehicleJ1939_GetSnapshot(&state, 1U, &snapshot);
  assert(snapshot.ambient_temp_cdeg == -27300);

  assert(VehicleJ1939_HandleFrame(&state, id, maximum_valid, 2U));
  VehicleJ1939_GetSnapshot(&state, 2U, &snapshot);
  assert(snapshot.ambient_temp_cdeg == INT16_MAX);
}

static void test_error_counters_and_bus_state(void)
{
  VehicleJ1939 state;
  VehicleJ1939 unchanged;
  SAFETY_J1939Snapshot snapshot;

  VehicleJ1939_Init(&state);
  VehicleJ1939_SetReady(&state);
  VehicleJ1939_RecordRxError(&state, 2U);
  VehicleJ1939_RecordRxError(&state, 3U);
  VehicleJ1939_RecordRxError(&state, 0U);
  VehicleJ1939_RecordDrop(&state, 4U);
  VehicleJ1939_RecordDrop(&state, 0U);

  assert(state.sampled.rx_errors == 5U);
  assert(state.sampled.rx_dropped == 4U);
  assert((state.sampled.status_flags & SAFETY_J1939_STATUS_RX_ERROR) != 0U);
  assert((state.sampled.status_flags & SAFETY_J1939_STATUS_RX_DROPPED) != 0U);

  VehicleJ1939_RecordBusState(&state, SAFETY_J1939_BUS_WARNING);
  assert(state.sampled.bus_state == SAFETY_J1939_BUS_WARNING);
  assert((state.sampled.status_flags & SAFETY_J1939_STATUS_BUS_WARNING) != 0U);
  assert((state.sampled.status_flags & SAFETY_J1939_STATUS_BUS_PASSIVE) == 0U);
  assert((state.sampled.status_flags & SAFETY_J1939_STATUS_BUS_OFF) == 0U);

  VehicleJ1939_RecordBusState(&state, SAFETY_J1939_BUS_PASSIVE);
  assert(state.sampled.bus_state == SAFETY_J1939_BUS_PASSIVE);
  assert((state.sampled.status_flags & SAFETY_J1939_STATUS_BUS_WARNING) == 0U);
  assert((state.sampled.status_flags & SAFETY_J1939_STATUS_BUS_PASSIVE) != 0U);

  VehicleJ1939_RecordBusState(&state, SAFETY_J1939_BUS_OFF);
  assert(state.sampled.bus_state == SAFETY_J1939_BUS_OFF);
  assert(state.sampled.bus_off_events == 1U);
  assert((state.sampled.status_flags & SAFETY_J1939_STATUS_BUS_PASSIVE) == 0U);
  assert((state.sampled.status_flags & SAFETY_J1939_STATUS_BUS_OFF) != 0U);
  VehicleJ1939_RecordBusState(&state, SAFETY_J1939_BUS_OFF);
  assert(state.sampled.bus_off_events == 1U);

  VehicleJ1939_RecordBusState(&state, SAFETY_J1939_BUS_ACTIVE);
  assert(state.sampled.bus_state == SAFETY_J1939_BUS_ACTIVE);
  assert((state.sampled.status_flags &
          (SAFETY_J1939_STATUS_BUS_WARNING |
           SAFETY_J1939_STATUS_BUS_PASSIVE |
           SAFETY_J1939_STATUS_BUS_OFF)) == 0U);
  VehicleJ1939_RecordBusState(&state, SAFETY_J1939_BUS_OFF);
  assert(state.sampled.bus_off_events == 2U);

  unchanged = state;
  VehicleJ1939_RecordBusState(&state, (uint8_t)(SAFETY_J1939_BUS_OFF + 1U));
  assert(memcmp(&state, &unchanged, sizeof(state)) == 0);

  VehicleJ1939_SetInitFault(&state);
  assert((state.sampled.status_flags & SAFETY_J1939_STATUS_READY) == 0U);
  assert((state.sampled.status_flags & SAFETY_J1939_STATUS_INIT_FAULT) != 0U);
  assert((state.sampled.status_flags & SAFETY_J1939_STATUS_BUS_OFF) == 0U);
  assert(state.sampled.bus_state == SAFETY_J1939_BUS_STOPPED);
  assert(state.sampled.rx_errors == 5U);
  assert(state.sampled.rx_dropped == 4U);
  assert(state.sampled.bus_off_events == 2U);

  state.sampled.reserved[0] = 0xA5U;
  state.sampled.reserved[1] = 0x5AU;
  VehicleJ1939_GetSnapshot(&state, 99U, &snapshot);
  assert(snapshot.timestamp_ms == 99U);
  assert(snapshot.rx_errors == 5U);
  assert(snapshot.rx_dropped == 4U);
  assert(snapshot.bus_off_events == 2U);
  assert(snapshot.bus_state == SAFETY_J1939_BUS_STOPPED);
  assert(snapshot.reserved[0] == 0U);
  assert(snapshot.reserved[1] == 0U);
}

int main(void)
{
  test_initialization_and_null_snapshot();
  test_six_pgn_golden_vectors();
  test_priority_and_source_are_ignored();
  test_identifier_boundary_rejection_is_side_effect_free();
  test_sentinel_values_preserve_previous_samples();
  test_exact_timeout_boundary();
  test_time_and_counter_uint32_wrap();
  test_ambient_saturation();
  test_error_counters_and_bus_state();
  return 0;
}
