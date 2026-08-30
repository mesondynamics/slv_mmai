#ifndef VEHICLE_CAN_H
#define VEHICLE_CAN_H

#include <stdbool.h>
#include <stdint.h>

#include "fdcan.h"
#include "safety_api.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t VehicleCan_Init(uint32_t now_ms);
void VehicleCan_Process(uint32_t now_ms);
bool VehicleCan_IsHealthy(void);
int32_t VehicleCan_GetSnapshot(SAFETY_J1939Snapshot *snapshot,
                               uint32_t now_ms);
void VehicleCan_HandleRxFifo0(FDCAN_HandleTypeDef *hfdcan,
                              uint32_t rx_fifo0_its);
void VehicleCan_HandleErrorStatus(FDCAN_HandleTypeDef *hfdcan,
                                  uint32_t error_status_its);
void VehicleCan_HandleError(FDCAN_HandleTypeDef *hfdcan);

#ifdef __cplusplus
}
#endif

#endif /* VEHICLE_CAN_H */
