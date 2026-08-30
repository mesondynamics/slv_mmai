#ifndef STEERING_CAN_H
#define STEERING_CAN_H

#include <stdbool.h>
#include <stdint.h>

#include "fdcan.h"
#include "safety_api.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t SteeringCan_Init(uint32_t now_ms);
int32_t SteeringCan_SetCommand(int16_t velocity_tdeg_per_s,
                               uint8_t enable, uint8_t source,
                               uint8_t flags, uint32_t now_ms);
void SteeringCan_RequestSafe(uint32_t now_ms);
void SteeringCan_RecordCommandRejected(uint32_t now_ms);
void SteeringCan_Process(uint32_t now_ms);
bool SteeringCan_ConsumeActiveFault(void);
bool SteeringCan_CanClearFault(void);
void SteeringCan_ClearFaults(void);
void SteeringCan_GetSnapshot(SAFETY_SteeringSnapshot *snapshot,
                             uint32_t now_ms,
                             uint32_t command_sequence);
void SteeringCan_HandleRxFifo0(FDCAN_HandleTypeDef *hfdcan,
                               uint32_t rx_fifo0_its);
void SteeringCan_HandleErrorStatus(FDCAN_HandleTypeDef *hfdcan,
                                   uint32_t error_status_its);
void SteeringCan_HandleError(FDCAN_HandleTypeDef *hfdcan);

#ifdef __cplusplus
}
#endif

#endif /* STEERING_CAN_H */
