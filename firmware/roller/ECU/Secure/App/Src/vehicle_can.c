#include "vehicle_can.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "vehicle_j1939.h"

#define VEHICLE_CAN_FILTER_MASK          0x03FFFF00UL
#define VEHICLE_CAN_RX_RING_SIZE         8U
#define VEHICLE_CAN_RX_RING_MASK         (VEHICLE_CAN_RX_RING_SIZE - 1U)
#define VEHICLE_CAN_ISR_FRAME_BUDGET     3U
#define VEHICLE_CAN_PROCESS_FRAME_BUDGET 3U
#define VEHICLE_CAN_STATUS_POLL_MS       10UL

typedef struct
{
  uint32_t extended_id;
  uint8_t data[8];
} VehicleCanRxItem;

static VehicleJ1939 vehicle_j1939;
static VehicleCanRxItem vehicle_rx_ring[VEHICLE_CAN_RX_RING_SIZE];
static volatile uint32_t vehicle_rx_write_sequence;
static volatile uint32_t vehicle_rx_read_sequence;
static volatile uint32_t vehicle_irq_error_status;
static volatile uint32_t vehicle_irq_hal_errors;
static volatile uint32_t vehicle_irq_rx_errors;
static volatile uint32_t vehicle_irq_rx_dropped;
static uint32_t vehicle_last_status_poll_ms;
static uint8_t vehicle_can_initialized;
static uint8_t vehicle_can_health_fault;

_Static_assert((VEHICLE_CAN_RX_RING_SIZE &
                (VEHICLE_CAN_RX_RING_SIZE - 1U)) == 0U,
               "Vehicle CAN ring size must be a power of two");
_Static_assert(VEHICLE_CAN_ISR_FRAME_BUDGET == 3U,
               "FDCAN1 ISR budget must match the three-entry FIFO0");

static uint32_t VehicleCan_EnterCritical(void)
{
  uint32_t primask = __get_PRIMASK();

  __disable_irq();
  __DMB();
  return primask;
}

static void VehicleCan_ExitCritical(uint32_t primask)
{
  __DMB();
  if (primask == 0U)
  {
    __enable_irq();
  }
}

static bool VehicleCan_TimeReached(uint32_t now_ms, uint32_t deadline_ms)
{
  return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint8_t VehicleCan_BusState(
    const FDCAN_ProtocolStatusTypeDef *protocol)
{
  if (protocol->BusOff != 0U)
  {
    return SAFETY_J1939_BUS_OFF;
  }
  if (protocol->ErrorPassive != 0U)
  {
    return SAFETY_J1939_BUS_PASSIVE;
  }
  if (protocol->Warning != 0U)
  {
    return SAFETY_J1939_BUS_WARNING;
  }
  return SAFETY_J1939_BUS_ACTIVE;
}

static void VehicleCan_RecoverBusOff(void)
{
  /* FDCAN1 is receive-only, so there are no stale transmit buffers to abort.
     Clearing INIT starts the ISO 11898-1 bus-off recovery sequence. */
  CLEAR_BIT(hfdcan1.Instance->CCCR, FDCAN_CCCR_INIT);
}

static void VehicleCan_ProcessIrqErrors(void)
{
  uint32_t error_status;
  uint32_t hal_errors;
  uint32_t rx_errors;
  uint32_t rx_dropped;
  uint32_t primask = VehicleCan_EnterCritical();

  error_status = vehicle_irq_error_status;
  hal_errors = vehicle_irq_hal_errors;
  rx_errors = vehicle_irq_rx_errors;
  rx_dropped = vehicle_irq_rx_dropped;
  vehicle_irq_error_status = 0U;
  vehicle_irq_hal_errors = 0U;
  vehicle_irq_rx_errors = 0U;
  vehicle_irq_rx_dropped = 0U;
  VehicleCan_ExitCritical(primask);

  VehicleJ1939_RecordRxError(&vehicle_j1939, rx_errors);
  VehicleJ1939_RecordDrop(&vehicle_j1939, rx_dropped);
  if ((rx_errors != 0U) || (rx_dropped != 0U) || (error_status != 0U))
  {
    vehicle_can_health_fault = 1U;
  }
  if ((error_status & FDCAN_IT_BUS_OFF) != 0U)
  {
    VehicleJ1939_RecordBusState(&vehicle_j1939, SAFETY_J1939_BUS_OFF);
    VehicleCan_RecoverBusOff();
  }
  else if ((error_status & FDCAN_IT_ERROR_PASSIVE) != 0U)
  {
    VehicleJ1939_RecordBusState(&vehicle_j1939, SAFETY_J1939_BUS_PASSIVE);
  }
  else if ((error_status & FDCAN_IT_ERROR_WARNING) != 0U)
  {
    VehicleJ1939_RecordBusState(&vehicle_j1939, SAFETY_J1939_BUS_WARNING);
  }
  if ((hal_errors & (HAL_FDCAN_ERROR_RAM_ACCESS |
                     HAL_FDCAN_ERROR_PROTOCOL_ARBT |
                     HAL_FDCAN_ERROR_PROTOCOL_DATA |
                     HAL_FDCAN_ERROR_RAM_WDG |
                     HAL_FDCAN_ERROR_RESERVED_AREA)) != 0U)
  {
    VehicleJ1939_RecordRxError(&vehicle_j1939, 1U);
    vehicle_can_health_fault = 1U;
  }
}

static void VehicleCan_ProcessRx(uint32_t now_ms)
{
  uint32_t handled = 0U;

  while ((vehicle_rx_read_sequence != vehicle_rx_write_sequence) &&
         (handled < VEHICLE_CAN_PROCESS_FRAME_BUDGET))
  {
    uint32_t sequence = vehicle_rx_read_sequence;
    const VehicleCanRxItem *item =
        &vehicle_rx_ring[sequence & VEHICLE_CAN_RX_RING_MASK];

    if (!VehicleJ1939_HandleFrame(&vehicle_j1939, item->extended_id,
                                  item->data, now_ms))
    {
      VehicleJ1939_RecordRxError(&vehicle_j1939, 1U);
      vehicle_can_health_fault = 1U;
    }
    __DMB();
    vehicle_rx_read_sequence = sequence + 1U;
    ++handled;
  }
}

static void VehicleCan_PollStatus(uint32_t now_ms)
{
  FDCAN_ProtocolStatusTypeDef protocol = {0};
  uint8_t bus_state;

  if (!VehicleCan_TimeReached(now_ms, vehicle_last_status_poll_ms +
                              VEHICLE_CAN_STATUS_POLL_MS))
  {
    return;
  }
  vehicle_last_status_poll_ms = now_ms;
  if (HAL_FDCAN_GetProtocolStatus(&hfdcan1, &protocol) != HAL_OK)
  {
    VehicleJ1939_RecordRxError(&vehicle_j1939, 1U);
    vehicle_can_health_fault = 1U;
    return;
  }
  bus_state = VehicleCan_BusState(&protocol);
  VehicleJ1939_RecordBusState(&vehicle_j1939, bus_state);
  vehicle_can_health_fault =
      (bus_state == SAFETY_J1939_BUS_ACTIVE) ? 0U : 1U;
  if (bus_state == SAFETY_J1939_BUS_OFF)
  {
    VehicleCan_RecoverBusOff();
  }
}

int32_t VehicleCan_Init(uint32_t now_ms)
{
  static const uint32_t pgns[6] = {
    VEHICLE_J1939_PGN_EEC1,
    VEHICLE_J1939_PGN_EEC2,
    VEHICLE_J1939_PGN_ET1,
    VEHICLE_J1939_PGN_EFL_P1,
    VEHICLE_J1939_PGN_AMB,
    VEHICLE_J1939_PGN_DM1
  };
  FDCAN_FilterTypeDef filter = {0};
  uint32_t notifications = FDCAN_IT_RX_FIFO0_NEW_MESSAGE |
                           FDCAN_IT_RX_FIFO0_FULL |
                           FDCAN_IT_RX_FIFO0_MESSAGE_LOST |
                           FDCAN_IT_ERROR_WARNING |
                           FDCAN_IT_ERROR_PASSIVE |
                           FDCAN_IT_BUS_OFF |
                           FDCAN_IT_ARB_PROTOCOL_ERROR |
                           FDCAN_IT_DATA_PROTOCOL_ERROR |
                           FDCAN_IT_RAM_ACCESS_FAILURE |
                           FDCAN_IT_RAM_WATCHDOG;
  uint32_t index;

  memset(vehicle_rx_ring, 0, sizeof(vehicle_rx_ring));
  vehicle_rx_write_sequence = 0U;
  vehicle_rx_read_sequence = 0U;
  vehicle_irq_error_status = 0U;
  vehicle_irq_hal_errors = 0U;
  vehicle_irq_rx_errors = 0U;
  vehicle_irq_rx_dropped = 0U;
  vehicle_last_status_poll_ms = now_ms;
  vehicle_can_initialized = 0U;
  vehicle_can_health_fault = 1U;
  VehicleJ1939_Init(&vehicle_j1939);

  for (index = 0U; index < 6U; ++index)
  {
    filter.IdType = FDCAN_EXTENDED_ID;
    filter.FilterIndex = index;
    filter.FilterType = FDCAN_FILTER_MASK;
    filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    filter.FilterID1 = pgns[index] << 8U;
    filter.FilterID2 = VEHICLE_CAN_FILTER_MASK;
    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &filter) != HAL_OK)
    {
      VehicleJ1939_SetInitFault(&vehicle_j1939);
      return SAFETY_RESULT_INTERNAL_ERROR;
    }
  }
  if ((HAL_FDCAN_ConfigGlobalFilter(&hfdcan1,
                                    FDCAN_REJECT, FDCAN_REJECT,
                                    FDCAN_REJECT_REMOTE,
                                    FDCAN_REJECT_REMOTE) != HAL_OK) ||
      (HAL_FDCAN_ConfigInterruptLines(
           &hfdcan1,
           FDCAN_IT_LIST_RX_FIFO0 | FDCAN_IT_LIST_BIT_LINE_ERROR |
           FDCAN_IT_LIST_PROTOCOL_ERROR | FDCAN_IT_RAM_ACCESS_FAILURE,
           FDCAN_INTERRUPT_LINE0) != HAL_OK) ||
      (HAL_FDCAN_ActivateNotification(&hfdcan1, notifications, 0U) != HAL_OK) ||
      (HAL_FDCAN_Start(&hfdcan1) != HAL_OK))
  {
    VehicleJ1939_SetInitFault(&vehicle_j1939);
    return SAFETY_RESULT_INTERNAL_ERROR;
  }

  vehicle_can_initialized = 1U;
  vehicle_can_health_fault = 0U;
  VehicleJ1939_SetReady(&vehicle_j1939);
  return SAFETY_RESULT_OK;
}

void VehicleCan_Process(uint32_t now_ms)
{
  if (vehicle_can_initialized == 0U)
  {
    return;
  }
  /* Poll first so an ACTIVE sample only clears faults that predate this
     service pass. IRQ/parser errors consumed below stay observable until a
     subsequent successful ACTIVE poll confirms recovery. */
  VehicleCan_PollStatus(now_ms);
  VehicleCan_ProcessIrqErrors();
  VehicleCan_ProcessRx(now_ms);
}

bool VehicleCan_IsHealthy(void)
{
  uint32_t primask = VehicleCan_EnterCritical();
  bool healthy = (vehicle_can_initialized != 0U) &&
                 (vehicle_can_health_fault == 0U) &&
                 ((vehicle_j1939.sampled.status_flags &
                   SAFETY_J1939_STATUS_INIT_FAULT) == 0U) &&
                 (vehicle_j1939.sampled.bus_state ==
                  SAFETY_J1939_BUS_ACTIVE);

  VehicleCan_ExitCritical(primask);
  return healthy;
}

int32_t VehicleCan_GetSnapshot(SAFETY_J1939Snapshot *snapshot,
                               uint32_t now_ms)
{
  uint32_t primask;

  if (snapshot == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  primask = VehicleCan_EnterCritical();
  VehicleJ1939_GetSnapshot(&vehicle_j1939, now_ms, snapshot);
  VehicleCan_ExitCritical(primask);
  return SAFETY_RESULT_OK;
}

void VehicleCan_HandleRxFifo0(FDCAN_HandleTypeDef *hfdcan,
                              uint32_t rx_fifo0_its)
{
  uint32_t handled = 0U;

  if ((hfdcan == NULL) || (hfdcan->Instance != FDCAN1))
  {
    return;
  }
  if ((rx_fifo0_its & (FDCAN_IT_RX_FIFO0_FULL |
                       FDCAN_IT_RX_FIFO0_MESSAGE_LOST)) != 0U)
  {
    ++vehicle_irq_rx_dropped;
  }
  while ((HAL_FDCAN_GetRxFifoFillLevel(hfdcan, FDCAN_RX_FIFO0) != 0U) &&
         (handled < VEHICLE_CAN_ISR_FRAME_BUDGET))
  {
    FDCAN_RxHeaderTypeDef header = {0};
    uint8_t data[8] = {0};
    uint32_t write_sequence;
    VehicleCanRxItem *item;

    if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0,
                               &header, data) != HAL_OK)
    {
      ++vehicle_irq_rx_errors;
      break;
    }
    ++handled;
    if ((header.IdType != FDCAN_EXTENDED_ID) ||
        (header.RxFrameType != FDCAN_DATA_FRAME) ||
        (header.FDFormat != FDCAN_CLASSIC_CAN) ||
        (header.DataLength != FDCAN_DLC_BYTES_8))
    {
      ++vehicle_irq_rx_errors;
      continue;
    }
    write_sequence = vehicle_rx_write_sequence;
    if ((write_sequence - vehicle_rx_read_sequence) >=
        VEHICLE_CAN_RX_RING_SIZE)
    {
      ++vehicle_irq_rx_dropped;
      continue;
    }
    item = &vehicle_rx_ring[write_sequence & VEHICLE_CAN_RX_RING_MASK];
    item->extended_id = header.Identifier;
    memcpy(item->data, data, sizeof(item->data));
    __DMB();
    vehicle_rx_write_sequence = write_sequence + 1U;
  }
}

void VehicleCan_HandleErrorStatus(FDCAN_HandleTypeDef *hfdcan,
                                  uint32_t error_status_its)
{
  if ((hfdcan != NULL) && (hfdcan->Instance == FDCAN1))
  {
    vehicle_irq_error_status |= error_status_its;
  }
}

void VehicleCan_HandleError(FDCAN_HandleTypeDef *hfdcan)
{
  if ((hfdcan != NULL) && (hfdcan->Instance == FDCAN1))
  {
    vehicle_irq_hal_errors |= hfdcan->ErrorCode;
    hfdcan->ErrorCode = HAL_FDCAN_ERROR_NONE;
  }
}
