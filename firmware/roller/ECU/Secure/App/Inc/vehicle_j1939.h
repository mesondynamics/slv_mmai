#ifndef VEHICLE_J1939_H
#define VEHICLE_J1939_H

#include <stdbool.h>
#include <stdint.h>

#include "safety_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VEHICLE_J1939_PGN_EEC1    0xF004UL
#define VEHICLE_J1939_PGN_EEC2    0xF003UL
#define VEHICLE_J1939_PGN_ET1     0xFEEEUL
#define VEHICLE_J1939_PGN_EFL_P1  0xFEEFUL
#define VEHICLE_J1939_PGN_AMB     0xFEF5UL
#define VEHICLE_J1939_PGN_DM1     0xFECAUL

typedef struct
{
  SAFETY_J1939Snapshot sampled;
  uint32_t received_mask;
  uint32_t last_tick_ms[6];
} VehicleJ1939;

void VehicleJ1939_Init(VehicleJ1939 *state);
void VehicleJ1939_SetReady(VehicleJ1939 *state);
void VehicleJ1939_SetInitFault(VehicleJ1939 *state);
bool VehicleJ1939_HandleFrame(VehicleJ1939 *state, uint32_t extended_id,
                              const uint8_t data[8], uint32_t now_ms);
void VehicleJ1939_RecordRxError(VehicleJ1939 *state, uint32_t count);
void VehicleJ1939_RecordDrop(VehicleJ1939 *state, uint32_t count);
void VehicleJ1939_RecordBusState(VehicleJ1939 *state, uint8_t bus_state);
void VehicleJ1939_GetSnapshot(const VehicleJ1939 *state, uint32_t now_ms,
                              SAFETY_J1939Snapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* VEHICLE_J1939_H */
