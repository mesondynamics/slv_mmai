#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "j1939.h"
#include "safety_api.h"
#include "secure_nsc.h"

static SAFETY_J1939Snapshot stub_snapshot;
static int32_t stub_result;

int32_t SECURE_SafetyGetJ1939Snapshot(
    uint32_t requested_version, SAFETY_J1939Snapshot *snapshot,
    uint32_t snapshot_capacity)
{
  assert(requested_version == SAFETY_J1939_API_VERSION);
  assert(snapshot != NULL);
  assert(snapshot_capacity == sizeof(*snapshot));
  if (stub_result == SAFETY_RESULT_OK)
  {
    *snapshot = stub_snapshot;
  }
  return stub_result;
}

static void FillValidSnapshot(void)
{
  memset(&stub_snapshot, 0, sizeof(stub_snapshot));
  stub_snapshot.api_version = SAFETY_J1939_API_VERSION;
  stub_snapshot.size = sizeof(stub_snapshot);
  stub_snapshot.timestamp_ms = 1234U;
  stub_snapshot.valid_mask = SAFETY_J1939_VALID_EEC1 |
                             SAFETY_J1939_VALID_EEC2 |
                             SAFETY_J1939_VALID_ET1 |
                             SAFETY_J1939_VALID_EFL_P1 |
                             SAFETY_J1939_VALID_AMB |
                             SAFETY_J1939_VALID_DM1;
  stub_snapshot.eec1_age_ms = 1U;
  stub_snapshot.eec2_age_ms = 2U;
  stub_snapshot.et1_age_ms = 3U;
  stub_snapshot.eflp1_age_ms = 4U;
  stub_snapshot.amb_age_ms = 5U;
  stub_snapshot.dm1_age_ms = 6U;
  stub_snapshot.engine_rpm = 1500U;
  stub_snapshot.engine_torque_percent = 45U;
  stub_snapshot.driver_demand_percent = 46U;
  stub_snapshot.accelerator_pedal_percent = 47U;
  stub_snapshot.engine_load_percent = 48U;
  stub_snapshot.coolant_temp_cdeg = 8100;
  stub_snapshot.fuel_temp_cdeg = 3200;
  stub_snapshot.oil_pressure_kpa = 404U;
  stub_snapshot.fuel_pressure_kpa = 212U;
  stub_snapshot.ambient_temp_cdeg = -1200;
  stub_snapshot.dtc_count = 3U;
}

static void AssertAllValid(const J1939Data *data)
{
  assert(data->eec1_valid);
  assert(data->eec2_valid);
  assert(data->et1_valid);
  assert(data->eflp1_valid);
  assert(data->amb_valid);
  assert(data->dm1_valid);
}

static void AssertAllInvalid(const J1939Data *data)
{
  assert(!data->eec1_valid);
  assert(!data->eec2_valid);
  assert(!data->et1_valid);
  assert(!data->eflp1_valid);
  assert(!data->amb_valid);
  assert(!data->dm1_valid);
}

int main(void)
{
  J1939Data data;

  FillValidSnapshot();
  stub_result = SAFETY_RESULT_OK;
  assert(J1939_Init());
  memset(&data, 0, sizeof(data));
  J1939_GetData(&data);
  AssertAllValid(&data);
  assert(data.engine_rpm == 1500U);
  assert(data.engine_torque_percent == 45U);
  assert(data.driver_demand_percent == 46U);
  assert(data.accelerator_pedal_percent == 47U);
  assert(data.engine_load_percent == 48U);
  assert(data.coolant_temp_cdeg == 8100);
  assert(data.fuel_temp_cdeg == 3200);
  assert(data.oil_pressure_kpa == 404U);
  assert(data.fuel_pressure_kpa == 212U);
  assert(data.ambient_temp_cdeg == -1200);
  assert(data.dtc_count == 3U);
  assert(data.eec1_timestamp == 1233U);
  assert(data.dm1_timestamp == 1228U);

  stub_result = SAFETY_RESULT_INTERNAL_ERROR;
  J1939_Poll();
  J1939_GetData(&data);
  AssertAllInvalid(&data);

  FillValidSnapshot();
  stub_snapshot.api_version = SAFETY_J1939_API_VERSION + 1U;
  stub_result = SAFETY_RESULT_OK;
  J1939_Poll();
  J1939_GetData(&data);
  AssertAllInvalid(&data);

  FillValidSnapshot();
  stub_snapshot.valid_mask = SAFETY_J1939_VALID_EEC1;
  J1939_Poll();
  J1939_GetData(&data);
  assert(data.eec1_valid);
  assert(data.eec1_timestamp == 1233U);
  assert(!data.eec2_valid);
  assert(data.eec2_timestamp == 0U);
  J1939_GetData(NULL);
  return 0;
}
