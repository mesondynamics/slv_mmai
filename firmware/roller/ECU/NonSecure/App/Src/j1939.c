#include "j1939.h"

#include <string.h>

#include "safety_api.h"
#include "secure_nsc.h"

static J1939Data vehicle_data;

_Static_assert(J1939_DATA_TIMEOUT_MS == SAFETY_J1939_DATA_TIMEOUT_MS,
               "NonSecure J1939 timeout must match the Secure owner");

static void J1939_ClearValidity(void)
{
  vehicle_data.eec1_valid = false;
  vehicle_data.eec2_valid = false;
  vehicle_data.et1_valid = false;
  vehicle_data.eflp1_valid = false;
  vehicle_data.amb_valid = false;
  vehicle_data.dm1_valid = false;
}

static void J1939_ApplySnapshot(const SAFETY_J1939Snapshot *snapshot)
{
  uint32_t valid;

  if ((snapshot == NULL) ||
      (snapshot->api_version != SAFETY_J1939_API_VERSION) ||
      (snapshot->size != sizeof(*snapshot)))
  {
    J1939_ClearValidity();
    return;
  }

  valid = snapshot->valid_mask;
  vehicle_data.engine_rpm = snapshot->engine_rpm;
  vehicle_data.engine_torque_percent = snapshot->engine_torque_percent;
  vehicle_data.driver_demand_percent = snapshot->driver_demand_percent;
  vehicle_data.accelerator_pedal_percent =
      snapshot->accelerator_pedal_percent;
  vehicle_data.engine_load_percent = snapshot->engine_load_percent;
  vehicle_data.coolant_temp_cdeg = snapshot->coolant_temp_cdeg;
  vehicle_data.fuel_temp_cdeg = snapshot->fuel_temp_cdeg;
  vehicle_data.oil_pressure_kpa = snapshot->oil_pressure_kpa;
  vehicle_data.fuel_pressure_kpa = snapshot->fuel_pressure_kpa;
  vehicle_data.ambient_temp_cdeg = snapshot->ambient_temp_cdeg;
  vehicle_data.dtc_count = snapshot->dtc_count;

  vehicle_data.eec1_valid =
      (valid & SAFETY_J1939_VALID_EEC1) != 0U;
  vehicle_data.eec2_valid =
      (valid & SAFETY_J1939_VALID_EEC2) != 0U;
  vehicle_data.et1_valid =
      (valid & SAFETY_J1939_VALID_ET1) != 0U;
  vehicle_data.eflp1_valid =
      (valid & SAFETY_J1939_VALID_EFL_P1) != 0U;
  vehicle_data.amb_valid =
      (valid & SAFETY_J1939_VALID_AMB) != 0U;
  vehicle_data.dm1_valid =
      (valid & SAFETY_J1939_VALID_DM1) != 0U;

  /* Preserve the legacy local data-model shape. These timestamps are in the
     Secure monotonic-time domain and are diagnostic only; freshness is
     already decided atomically by Secure in valid_mask. */
  vehicle_data.eec1_timestamp =
      vehicle_data.eec1_valid ?
      snapshot->timestamp_ms - snapshot->eec1_age_ms : 0U;
  vehicle_data.eec2_timestamp =
      vehicle_data.eec2_valid ?
      snapshot->timestamp_ms - snapshot->eec2_age_ms : 0U;
  vehicle_data.et1_timestamp =
      vehicle_data.et1_valid ?
      snapshot->timestamp_ms - snapshot->et1_age_ms : 0U;
  vehicle_data.eflp1_timestamp =
      vehicle_data.eflp1_valid ?
      snapshot->timestamp_ms - snapshot->eflp1_age_ms : 0U;
  vehicle_data.amb_timestamp =
      vehicle_data.amb_valid ?
      snapshot->timestamp_ms - snapshot->amb_age_ms : 0U;
  vehicle_data.dm1_timestamp =
      vehicle_data.dm1_valid ?
      snapshot->timestamp_ms - snapshot->dm1_age_ms : 0U;
}

static bool J1939_Refresh(void)
{
  SAFETY_J1939Snapshot snapshot = {0};
  int32_t result;

  result = SECURE_SafetyGetJ1939Snapshot(
      SAFETY_J1939_API_VERSION, &snapshot, sizeof(snapshot));
  if (result != SAFETY_RESULT_OK)
  {
    /* Never continue publishing cached vehicle data as valid after the
       Secure boundary reports an error or ABI mismatch. */
    J1939_ClearValidity();
    return false;
  }
  J1939_ApplySnapshot(&snapshot);
  return true;
}

bool J1939_Init(void)
{
  memset(&vehicle_data, 0, sizeof(vehicle_data));
  return J1939_Refresh();
}

void J1939_Poll(void)
{
  (void)J1939_Refresh();
}

void J1939_GetData(J1939Data *data)
{
  if (data != NULL)
  {
    *data = vehicle_data;
  }
}
