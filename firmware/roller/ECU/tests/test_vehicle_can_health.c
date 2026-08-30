#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "vehicle_can.h"

FDCAN_GlobalTypeDef test_fdcan1_instance;
FDCAN_HandleTypeDef hfdcan1 = {
  .Instance = &test_fdcan1_instance,
  .ErrorCode = HAL_FDCAN_ERROR_NONE
};
uint32_t test_primask;

static FDCAN_ProtocolStatusTypeDef test_protocol_status;
static HAL_StatusTypeDef test_protocol_result = HAL_OK;
static uint32_t test_filter_call_count;
static uint32_t test_filter_failure_call;

HAL_StatusTypeDef HAL_FDCAN_ConfigFilter(
    FDCAN_HandleTypeDef *hfdcan, const FDCAN_FilterTypeDef *filter)
{
  (void)hfdcan;
  (void)filter;
  ++test_filter_call_count;
  if ((test_filter_failure_call != 0U) &&
      (test_filter_call_count == test_filter_failure_call))
  {
    return HAL_ERROR;
  }
  return HAL_OK;
}

HAL_StatusTypeDef HAL_FDCAN_ConfigGlobalFilter(
    FDCAN_HandleTypeDef *hfdcan, uint32_t nonmatching_standard,
    uint32_t nonmatching_extended, uint32_t reject_standard_remote,
    uint32_t reject_extended_remote)
{
  (void)hfdcan;
  (void)nonmatching_standard;
  (void)nonmatching_extended;
  (void)reject_standard_remote;
  (void)reject_extended_remote;
  return HAL_OK;
}

HAL_StatusTypeDef HAL_FDCAN_ConfigInterruptLines(
    FDCAN_HandleTypeDef *hfdcan, uint32_t interrupt_list,
    uint32_t interrupt_line)
{
  (void)hfdcan;
  (void)interrupt_list;
  (void)interrupt_line;
  return HAL_OK;
}

HAL_StatusTypeDef HAL_FDCAN_ActivateNotification(
    FDCAN_HandleTypeDef *hfdcan, uint32_t active_it,
    uint32_t buffer_indexes)
{
  (void)hfdcan;
  (void)active_it;
  (void)buffer_indexes;
  return HAL_OK;
}

HAL_StatusTypeDef HAL_FDCAN_Start(FDCAN_HandleTypeDef *hfdcan)
{
  (void)hfdcan;
  return HAL_OK;
}

HAL_StatusTypeDef HAL_FDCAN_GetProtocolStatus(
    const FDCAN_HandleTypeDef *hfdcan,
    FDCAN_ProtocolStatusTypeDef *protocol_status)
{
  (void)hfdcan;
  if (test_protocol_result != HAL_OK)
  {
    return test_protocol_result;
  }
  *protocol_status = test_protocol_status;
  return HAL_OK;
}

uint32_t HAL_FDCAN_GetRxFifoFillLevel(
    const FDCAN_HandleTypeDef *hfdcan, uint32_t rx_fifo)
{
  (void)hfdcan;
  (void)rx_fifo;
  return 0U;
}

HAL_StatusTypeDef HAL_FDCAN_GetRxMessage(
    FDCAN_HandleTypeDef *hfdcan, uint32_t rx_location,
    FDCAN_RxHeaderTypeDef *rx_header, uint8_t *rx_data)
{
  (void)hfdcan;
  (void)rx_location;
  (void)rx_header;
  (void)rx_data;
  return HAL_ERROR;
}

static void Test_ResetHal(void)
{
  memset(&test_fdcan1_instance, 0, sizeof(test_fdcan1_instance));
  memset(&test_protocol_status, 0, sizeof(test_protocol_status));
  hfdcan1.Instance = &test_fdcan1_instance;
  hfdcan1.ErrorCode = HAL_FDCAN_ERROR_NONE;
  test_primask = 0U;
  test_protocol_result = HAL_OK;
  test_filter_call_count = 0U;
  test_filter_failure_call = 0U;
}

int main(void)
{
  SAFETY_J1939Snapshot snapshot;

  Test_ResetHal();
  assert(!VehicleCan_IsHealthy());

  test_filter_failure_call = 1U;
  assert(VehicleCan_Init(0U) == SAFETY_RESULT_INTERNAL_ERROR);
  assert(!VehicleCan_IsHealthy());

  Test_ResetHal();
  assert(VehicleCan_Init(0U) == SAFETY_RESULT_OK);
  assert(VehicleCan_IsHealthy());

  test_protocol_status.Warning = 1U;
  VehicleCan_Process(10U);
  assert(!VehicleCan_IsHealthy());
  assert(VehicleCan_GetSnapshot(&snapshot, 10U) == SAFETY_RESULT_OK);
  assert(snapshot.bus_state == SAFETY_J1939_BUS_WARNING);

  test_protocol_status.Warning = 0U;
  VehicleCan_Process(19U);
  assert(!VehicleCan_IsHealthy());
  VehicleCan_Process(20U);
  assert(VehicleCan_IsHealthy());

  VehicleCan_HandleErrorStatus(&hfdcan1, FDCAN_IT_ARB_PROTOCOL_ERROR);
  VehicleCan_Process(21U);
  assert(!VehicleCan_IsHealthy());
  VehicleCan_Process(30U);
  assert(VehicleCan_IsHealthy());

  test_fdcan1_instance.CCCR = FDCAN_CCCR_INIT;
  VehicleCan_HandleErrorStatus(&hfdcan1, FDCAN_IT_BUS_OFF);
  VehicleCan_Process(31U);
  assert(!VehicleCan_IsHealthy());
  assert((test_fdcan1_instance.CCCR & FDCAN_CCCR_INIT) == 0U);
  assert(VehicleCan_GetSnapshot(&snapshot, 31U) == SAFETY_RESULT_OK);
  assert(snapshot.bus_state == SAFETY_J1939_BUS_OFF);
  VehicleCan_Process(40U);
  assert(VehicleCan_IsHealthy());

  hfdcan1.ErrorCode = HAL_FDCAN_ERROR_RAM_ACCESS;
  VehicleCan_HandleError(&hfdcan1);
  VehicleCan_Process(41U);
  assert(!VehicleCan_IsHealthy());
  assert(hfdcan1.ErrorCode == HAL_FDCAN_ERROR_NONE);
  VehicleCan_Process(50U);
  assert(VehicleCan_IsHealthy());

  test_protocol_result = HAL_ERROR;
  VehicleCan_Process(60U);
  assert(!VehicleCan_IsHealthy());
  test_protocol_result = HAL_OK;
  VehicleCan_Process(70U);
  assert(VehicleCan_IsHealthy());

  return 0;
}
