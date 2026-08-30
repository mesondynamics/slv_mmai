#include "vehicle_j1939.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum
{
  VEHICLE_J1939_INDEX_EEC1 = 0,
  VEHICLE_J1939_INDEX_EEC2,
  VEHICLE_J1939_INDEX_ET1,
  VEHICLE_J1939_INDEX_EFL_P1,
  VEHICLE_J1939_INDEX_AMB,
  VEHICLE_J1939_INDEX_DM1,
  VEHICLE_J1939_INDEX_COUNT
};

_Static_assert(VEHICLE_J1939_INDEX_COUNT == 6,
               "J1939 timestamp table must cover six PGNs");
_Static_assert(sizeof(SAFETY_J1939Snapshot) == 84U,
               "J1939 snapshot ABI changed");

static uint8_t VehicleJ1939_ClampPercent(int16_t value)
{
  if (value < 0)
  {
    return 0U;
  }
  if (value > 100)
  {
    return 100U;
  }
  return (uint8_t)value;
}

static uint32_t VehicleJ1939_ExtractPgn(uint32_t identifier)
{
  uint8_t pf = (uint8_t)(identifier >> 16U);
  uint8_t ps = (uint8_t)(identifier >> 8U);

  return (pf < 240U) ? ((uint32_t)pf << 8U) :
                       (((uint32_t)pf << 8U) | ps);
}

static int16_t VehicleJ1939_SaturateI32ToI16(int32_t value)
{
  if (value > INT16_MAX)
  {
    return INT16_MAX;
  }
  if (value < INT16_MIN)
  {
    return INT16_MIN;
  }
  return (int16_t)value;
}

static uint32_t VehicleJ1939_Age(const VehicleJ1939 *state,
                                 uint32_t mask, uint32_t index,
                                 uint32_t now_ms)
{
  if ((state->received_mask & mask) == 0U)
  {
    return UINT32_MAX;
  }
  return now_ms - state->last_tick_ms[index];
}

void VehicleJ1939_Init(VehicleJ1939 *state)
{
  if (state == NULL)
  {
    return;
  }
  memset(state, 0, sizeof(*state));
  state->sampled.api_version = SAFETY_J1939_API_VERSION;
  state->sampled.size = sizeof(SAFETY_J1939Snapshot);
  state->sampled.status_flags = SAFETY_J1939_STATUS_INIT_FAULT;
  state->sampled.bus_state = SAFETY_J1939_BUS_STOPPED;
}

void VehicleJ1939_SetReady(VehicleJ1939 *state)
{
  if (state == NULL)
  {
    return;
  }
  state->sampled.status_flags &= ~SAFETY_J1939_STATUS_INIT_FAULT;
  state->sampled.status_flags |= SAFETY_J1939_STATUS_READY;
  VehicleJ1939_RecordBusState(state, SAFETY_J1939_BUS_ACTIVE);
}

void VehicleJ1939_SetInitFault(VehicleJ1939 *state)
{
  if (state == NULL)
  {
    return;
  }
  state->sampled.status_flags &= ~SAFETY_J1939_STATUS_READY;
  state->sampled.status_flags |= SAFETY_J1939_STATUS_INIT_FAULT;
  VehicleJ1939_RecordBusState(state, SAFETY_J1939_BUS_STOPPED);
}

bool VehicleJ1939_HandleFrame(VehicleJ1939 *state, uint32_t extended_id,
                              const uint8_t data[8], uint32_t now_ms)
{
  uint32_t pgn;
  uint32_t valid_mask;
  uint32_t index;

  if ((state == NULL) || (data == NULL) ||
      (extended_id > 0x1FFFFFFFUL))
  {
    return false;
  }

  pgn = VehicleJ1939_ExtractPgn(extended_id);
  /* The hardware uses the same full PGN mask, but the Secure parser must not
     trust filter configuration as its only boundary. In particular, reject
     reserved/data-page bits that ExtractPgn intentionally does not retain. */
  if ((extended_id & 0x03FFFF00UL) != (pgn << 8U))
  {
    return false;
  }
  switch (pgn)
  {
    case VEHICLE_J1939_PGN_EEC1:
      if (data[2] <= 250U)
      {
        state->sampled.driver_demand_percent =
            VehicleJ1939_ClampPercent((int16_t)data[2] - 125);
      }
      if (data[3] <= 250U)
      {
        state->sampled.engine_torque_percent =
            VehicleJ1939_ClampPercent((int16_t)data[3] - 125);
      }
      {
        uint16_t raw = (uint16_t)data[4] | ((uint16_t)data[5] << 8U);
        if (raw <= 0xFAFFU)
        {
          state->sampled.engine_rpm = raw;
        }
      }
      valid_mask = SAFETY_J1939_VALID_EEC1;
      index = VEHICLE_J1939_INDEX_EEC1;
      break;

    case VEHICLE_J1939_PGN_EEC2:
      if (data[1] <= 250U)
      {
        state->sampled.accelerator_pedal_percent =
            VehicleJ1939_ClampPercent(
                (int16_t)(((uint32_t)data[1] * 40U) / 100U));
      }
      if (data[2] <= 250U)
      {
        state->sampled.engine_load_percent =
            VehicleJ1939_ClampPercent(data[2]);
      }
      valid_mask = SAFETY_J1939_VALID_EEC2;
      index = VEHICLE_J1939_INDEX_EEC2;
      break;

    case VEHICLE_J1939_PGN_ET1:
      if (data[0] <= 250U)
      {
        state->sampled.coolant_temp_cdeg =
            (int16_t)(((int16_t)data[0] - 40) * 100);
      }
      if (data[1] <= 250U)
      {
        state->sampled.fuel_temp_cdeg =
            (int16_t)(((int16_t)data[1] - 40) * 100);
      }
      valid_mask = SAFETY_J1939_VALID_ET1;
      index = VEHICLE_J1939_INDEX_ET1;
      break;

    case VEHICLE_J1939_PGN_EFL_P1:
      if (data[3] <= 250U)
      {
        state->sampled.oil_pressure_kpa = (uint16_t)data[3] * 4U;
      }
      if (data[2] <= 250U)
      {
        state->sampled.fuel_pressure_kpa = (uint16_t)data[2] * 4U;
      }
      valid_mask = SAFETY_J1939_VALID_EFL_P1;
      index = VEHICLE_J1939_INDEX_EFL_P1;
      break;

    case VEHICLE_J1939_PGN_AMB:
      {
        uint16_t raw = (uint16_t)data[3] | ((uint16_t)data[4] << 8U);
        if (raw <= 0xFAFFU)
        {
          int32_t cdeg =
              (((int32_t)raw * 3125L / 100000L) - 273L) * 100L;
          state->sampled.ambient_temp_cdeg =
              VehicleJ1939_SaturateI32ToI16(cdeg);
        }
      }
      valid_mask = SAFETY_J1939_VALID_AMB;
      index = VEHICLE_J1939_INDEX_AMB;
      break;

    case VEHICLE_J1939_PGN_DM1:
      /* Preserve the previous ECU's single-frame behavior. Transport
         protocol/multi-packet DTC decoding is intentionally not invented. */
      state->sampled.dtc_count = 0U;
      valid_mask = SAFETY_J1939_VALID_DM1;
      index = VEHICLE_J1939_INDEX_DM1;
      break;

    default:
      return false;
  }

  state->received_mask |= valid_mask;
  state->last_tick_ms[index] = now_ms;
  ++state->sampled.sequence;
  ++state->sampled.rx_frames;
  return true;
}

void VehicleJ1939_RecordRxError(VehicleJ1939 *state, uint32_t count)
{
  if ((state == NULL) || (count == 0U))
  {
    return;
  }
  state->sampled.rx_errors += count;
  state->sampled.status_flags |= SAFETY_J1939_STATUS_RX_ERROR;
}

void VehicleJ1939_RecordDrop(VehicleJ1939 *state, uint32_t count)
{
  if ((state == NULL) || (count == 0U))
  {
    return;
  }
  state->sampled.rx_dropped += count;
  state->sampled.status_flags |= SAFETY_J1939_STATUS_RX_DROPPED;
}

void VehicleJ1939_RecordBusState(VehicleJ1939 *state, uint8_t bus_state)
{
  uint8_t old_state;

  if ((state == NULL) || (bus_state > SAFETY_J1939_BUS_OFF))
  {
    return;
  }
  old_state = state->sampled.bus_state;
  state->sampled.status_flags &=
      ~(SAFETY_J1939_STATUS_BUS_WARNING |
        SAFETY_J1939_STATUS_BUS_PASSIVE |
        SAFETY_J1939_STATUS_BUS_OFF);
  if (bus_state == SAFETY_J1939_BUS_WARNING)
  {
    state->sampled.status_flags |= SAFETY_J1939_STATUS_BUS_WARNING;
  }
  else if (bus_state == SAFETY_J1939_BUS_PASSIVE)
  {
    state->sampled.status_flags |= SAFETY_J1939_STATUS_BUS_PASSIVE;
  }
  else if (bus_state == SAFETY_J1939_BUS_OFF)
  {
    state->sampled.status_flags |= SAFETY_J1939_STATUS_BUS_OFF;
    if (old_state != SAFETY_J1939_BUS_OFF)
    {
      ++state->sampled.bus_off_events;
    }
  }
  state->sampled.bus_state = bus_state;
}

void VehicleJ1939_GetSnapshot(const VehicleJ1939 *state, uint32_t now_ms,
                              SAFETY_J1939Snapshot *snapshot)
{
  static const uint32_t masks[VEHICLE_J1939_INDEX_COUNT] = {
    SAFETY_J1939_VALID_EEC1,
    SAFETY_J1939_VALID_EEC2,
    SAFETY_J1939_VALID_ET1,
    SAFETY_J1939_VALID_EFL_P1,
    SAFETY_J1939_VALID_AMB,
    SAFETY_J1939_VALID_DM1
  };
  uint32_t ages[VEHICLE_J1939_INDEX_COUNT];
  uint32_t valid_mask = 0U;
  uint32_t index;

  if (snapshot == NULL)
  {
    return;
  }
  memset(snapshot, 0, sizeof(*snapshot));
  if (state == NULL)
  {
    snapshot->api_version = SAFETY_J1939_API_VERSION;
    snapshot->size = sizeof(*snapshot);
    snapshot->status_flags = SAFETY_J1939_STATUS_INIT_FAULT;
    snapshot->bus_state = SAFETY_J1939_BUS_STOPPED;
    snapshot->eec1_age_ms = UINT32_MAX;
    snapshot->eec2_age_ms = UINT32_MAX;
    snapshot->et1_age_ms = UINT32_MAX;
    snapshot->eflp1_age_ms = UINT32_MAX;
    snapshot->amb_age_ms = UINT32_MAX;
    snapshot->dm1_age_ms = UINT32_MAX;
    return;
  }

  *snapshot = state->sampled;
  snapshot->api_version = SAFETY_J1939_API_VERSION;
  snapshot->size = sizeof(*snapshot);
  snapshot->timestamp_ms = now_ms;
  for (index = 0U; index < VEHICLE_J1939_INDEX_COUNT; ++index)
  {
    ages[index] = VehicleJ1939_Age(state, masks[index], index, now_ms);
    if (ages[index] <= SAFETY_J1939_DATA_TIMEOUT_MS)
    {
      valid_mask |= masks[index];
    }
  }
  snapshot->valid_mask = valid_mask;
  snapshot->eec1_age_ms = ages[VEHICLE_J1939_INDEX_EEC1];
  snapshot->eec2_age_ms = ages[VEHICLE_J1939_INDEX_EEC2];
  snapshot->et1_age_ms = ages[VEHICLE_J1939_INDEX_ET1];
  snapshot->eflp1_age_ms = ages[VEHICLE_J1939_INDEX_EFL_P1];
  snapshot->amb_age_ms = ages[VEHICLE_J1939_INDEX_AMB];
  snapshot->dm1_age_ms = ages[VEHICLE_J1939_INDEX_DM1];
  memset(snapshot->reserved, 0, sizeof(snapshot->reserved));
}
