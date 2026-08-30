#include "fdcan.h"

#include <stddef.h>

#include "steering_can.h"
#include "vehicle_can.h"

/* HAL exposes one callback symbol per event for all FDCAN instances. Keep the
   dispatch exact and centralized so vehicle CAN1 can never consume steering
   CAN2 frames, error state, or counters (and vice versa). */
void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan,
                               uint32_t rx_fifo0_its)
{
  if ((hfdcan != NULL) && (hfdcan->Instance == FDCAN1))
  {
    VehicleCan_HandleRxFifo0(hfdcan, rx_fifo0_its);
  }
  else if ((hfdcan != NULL) && (hfdcan->Instance == FDCAN2))
  {
    SteeringCan_HandleRxFifo0(hfdcan, rx_fifo0_its);
  }
}

void HAL_FDCAN_ErrorStatusCallback(FDCAN_HandleTypeDef *hfdcan,
                                   uint32_t error_status_its)
{
  if ((hfdcan != NULL) && (hfdcan->Instance == FDCAN1))
  {
    VehicleCan_HandleErrorStatus(hfdcan, error_status_its);
  }
  else if ((hfdcan != NULL) && (hfdcan->Instance == FDCAN2))
  {
    SteeringCan_HandleErrorStatus(hfdcan, error_status_its);
  }
}

void HAL_FDCAN_ErrorCallback(FDCAN_HandleTypeDef *hfdcan)
{
  if ((hfdcan != NULL) && (hfdcan->Instance == FDCAN1))
  {
    VehicleCan_HandleError(hfdcan);
  }
  else if ((hfdcan != NULL) && (hfdcan->Instance == FDCAN2))
  {
    SteeringCan_HandleError(hfdcan);
  }
}
