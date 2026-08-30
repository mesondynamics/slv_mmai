#include "steering_can.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "fdcan.h"
#include "steering_control.h"

#define STEERING_CAN_RX_RING_SIZE       8U
#define STEERING_CAN_RX_RING_MASK       (STEERING_CAN_RX_RING_SIZE - 1U)
#define STEERING_CAN_STATUS_POLL_MS     10UL
#define STEERING_CAN_TX_BUFFER_MASK     0x7UL

typedef struct
{
  uint32_t extended_id;
  uint8_t length;
  uint8_t data[8];
} SteeringCanRxItem;

static SteeringControl steering_control;
static SteeringCanRxItem steering_rx_ring[STEERING_CAN_RX_RING_SIZE];
static volatile uint32_t steering_rx_write_sequence;
static volatile uint32_t steering_rx_read_sequence;
static volatile uint32_t steering_irq_error_status;
static volatile uint32_t steering_irq_hal_errors;
static volatile uint32_t steering_irq_rx_errors;
static volatile uint32_t steering_safe_requested;
static uint32_t steering_last_status_poll_ms;
static uint8_t steering_can_initialized;

static uint32_t SteeringCan_EnterCritical(void)
{
  uint32_t primask = __get_PRIMASK();

  __disable_irq();
  __DMB();
  return primask;
}

static void SteeringCan_ExitCritical(uint32_t primask)
{
  __DMB();
  if (primask == 0U)
  {
    __enable_irq();
  }
}

static bool SteeringCan_TimeReached(uint32_t now_ms, uint32_t deadline_ms)
{
  return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint8_t SteeringCan_BusState(
    const FDCAN_ProtocolStatusTypeDef *protocol)
{
  if (protocol->BusOff != 0U)
  {
    return SAFETY_STEERING_BUS_OFF;
  }
  if (protocol->ErrorPassive != 0U)
  {
    return SAFETY_STEERING_BUS_PASSIVE;
  }
  if (protocol->Warning != 0U)
  {
    return SAFETY_STEERING_BUS_WARNING;
  }
  return SAFETY_STEERING_BUS_ACTIVE;
}

static void SteeringCan_AbortPending(void)
{
  uint32_t pending = hfdcan2.Instance->TXBRP &
                     STEERING_CAN_TX_BUFFER_MASK;

  if (pending != 0U)
  {
    (void)HAL_FDCAN_AbortTxRequest(&hfdcan2, pending);
  }
}

static void SteeringCan_RecoverBusOff(void)
{
  SteeringCan_AbortPending();
  /* Bosch M_CAN enters INIT on bus-off. Clearing INIT starts the ISO 11898-1
     recovery sequence; there is no wait loop and no stale Tx request remains. */
  CLEAR_BIT(hfdcan2.Instance->CCCR, FDCAN_CCCR_INIT);
}

static void SteeringCan_ProcessIrqErrors(uint32_t now_ms)
{
  uint32_t status;
  uint32_t hal_errors;
  uint32_t rx_errors;
  uint32_t primask = SteeringCan_EnterCritical();

  status = steering_irq_error_status;
  hal_errors = steering_irq_hal_errors;
  rx_errors = steering_irq_rx_errors;
  steering_irq_error_status = 0U;
  steering_irq_hal_errors = 0U;
  steering_irq_rx_errors = 0U;
  SteeringCan_ExitCritical(primask);

  if (rx_errors != 0U)
  {
    steering_control.rx_errors += rx_errors;
  }
  if ((status & FDCAN_IT_BUS_OFF) != 0U)
  {
    SteeringControl_RecordBusState(&steering_control,
                                   SAFETY_STEERING_BUS_OFF, 0U, 0U,
                                   now_ms);
    SteeringCan_RecoverBusOff();
  }
  else if ((status & (FDCAN_IT_ERROR_WARNING |
                      FDCAN_IT_ERROR_PASSIVE)) != 0U)
  {
    uint8_t bus_state = ((status & FDCAN_IT_ERROR_PASSIVE) != 0U) ?
        SAFETY_STEERING_BUS_PASSIVE : SAFETY_STEERING_BUS_WARNING;
    SteeringControl_RecordBusState(&steering_control, bus_state, 0U, 0U,
                                   now_ms);
  }
  if ((hal_errors & HAL_FDCAN_ERROR_RAM_ACCESS) != 0U)
  {
    SteeringControl_RecordProtocolError(&steering_control,
                                        SAFETY_STEERING_FAULT_RAM,
                                        now_ms);
  }
  if ((hal_errors & (HAL_FDCAN_ERROR_PROTOCOL_ARBT |
                     HAL_FDCAN_ERROR_PROTOCOL_DATA |
                     HAL_FDCAN_ERROR_RAM_WDG |
                     HAL_FDCAN_ERROR_RESERVED_AREA)) != 0U)
  {
    SteeringControl_RecordProtocolError(&steering_control,
                                        SAFETY_STEERING_FAULT_PROTOCOL,
                                        now_ms);
  }
}

static void SteeringCan_ProcessRx(uint32_t now_ms)
{
  while (steering_rx_read_sequence != steering_rx_write_sequence)
  {
    uint32_t sequence = steering_rx_read_sequence;
    const SteeringCanRxItem *item =
        &steering_rx_ring[sequence & STEERING_CAN_RX_RING_MASK];

    (void)SteeringControl_HandleRxFrame(&steering_control,
                                        item->extended_id, item->data,
                                        item->length, now_ms);
    __DMB();
    steering_rx_read_sequence = sequence + 1U;
  }
}

static void SteeringCan_PollStatus(uint32_t now_ms)
{
  FDCAN_ProtocolStatusTypeDef protocol = {0};
  FDCAN_ErrorCountersTypeDef counters = {0};
  uint8_t bus_state;

  if (!SteeringCan_TimeReached(now_ms, steering_last_status_poll_ms +
                               STEERING_CAN_STATUS_POLL_MS))
  {
    return;
  }
  steering_last_status_poll_ms = now_ms;
  if ((HAL_FDCAN_GetProtocolStatus(&hfdcan2, &protocol) != HAL_OK) ||
      (HAL_FDCAN_GetErrorCounters(&hfdcan2, &counters) != HAL_OK))
  {
    SteeringControl_RecordTxError(&steering_control, now_ms);
    return;
  }
  bus_state = SteeringCan_BusState(&protocol);
  SteeringControl_RecordBusState(&steering_control, bus_state,
                                 counters.TxErrorCnt,
                                 counters.RxErrorCnt, now_ms);
  if (bus_state == SAFETY_STEERING_BUS_OFF)
  {
    SteeringCan_RecoverBusOff();
  }
}

static void SteeringCan_TryTransmit(uint32_t now_ms)
{
  SteeringControlFrame frame;
  FDCAN_TxHeaderTypeDef header = {0};

  if (steering_control.bus_state == SAFETY_STEERING_BUS_OFF)
  {
    return;
  }
  if (!SteeringControl_GetPendingFrame(&steering_control, now_ms, &frame))
  {
    return;
  }
  if (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan2) == 0U)
  {
    SteeringControl_RecordTxDeferred(&steering_control, now_ms);
    return;
  }

  header.Identifier = frame.extended_id;
  header.IdType = FDCAN_EXTENDED_ID;
  header.TxFrameType = FDCAN_DATA_FRAME;
  header.DataLength = FDCAN_DLC_BYTES_8;
  header.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
  header.BitRateSwitch = FDCAN_BRS_OFF;
  header.FDFormat = FDCAN_CLASSIC_CAN;
  header.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
  header.MessageMarker = 0U;
  if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan2, &header, frame.data) != HAL_OK)
  {
    SteeringControl_RecordTxError(&steering_control, now_ms);
    return;
  }
  SteeringControl_ConfirmTransmit(&steering_control, frame.kind, now_ms);
}

int32_t SteeringCan_Init(uint32_t now_ms)
{
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

  memset(steering_rx_ring, 0, sizeof(steering_rx_ring));
  steering_rx_write_sequence = 0U;
  steering_rx_read_sequence = 0U;
  steering_irq_error_status = 0U;
  steering_irq_hal_errors = 0U;
  steering_irq_rx_errors = 0U;
  steering_safe_requested = 0U;
  steering_last_status_poll_ms = now_ms;
  steering_can_initialized = 0U;
  SteeringControl_Init(&steering_control, now_ms);

  filter.IdType = FDCAN_EXTENDED_ID;
  filter.FilterIndex = 0U;
  filter.FilterType = FDCAN_FILTER_MASK;
  filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
  filter.FilterID1 = STEERING_CONTROL_SDO_RESPONSE_EXT_ID;
  filter.FilterID2 = 0x1FFFFFFFUL;
  if (HAL_FDCAN_ConfigFilter(&hfdcan2, &filter) != HAL_OK)
  {
    steering_control.fault_flags |= SAFETY_STEERING_FAULT_INIT;
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  filter.FilterIndex = 1U;
  filter.FilterID1 = STEERING_CONTROL_HEARTBEAT_EXT_ID;
  if ((HAL_FDCAN_ConfigFilter(&hfdcan2, &filter) != HAL_OK) ||
      (HAL_FDCAN_ConfigGlobalFilter(&hfdcan2,
                                    FDCAN_REJECT, FDCAN_REJECT,
                                    FDCAN_REJECT_REMOTE,
                                    FDCAN_REJECT_REMOTE) != HAL_OK) ||
      (HAL_FDCAN_ConfigInterruptLines(
           &hfdcan2,
           FDCAN_IT_LIST_RX_FIFO0 | FDCAN_IT_LIST_BIT_LINE_ERROR |
           FDCAN_IT_LIST_PROTOCOL_ERROR | FDCAN_IT_RAM_ACCESS_FAILURE,
           FDCAN_INTERRUPT_LINE0) != HAL_OK) ||
      (HAL_FDCAN_ActivateNotification(&hfdcan2, notifications, 0U) != HAL_OK) ||
      (HAL_FDCAN_Start(&hfdcan2) != HAL_OK))
  {
    steering_control.fault_flags |= SAFETY_STEERING_FAULT_INIT;
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  steering_can_initialized = 1U;
  steering_control.bus_state = SAFETY_STEERING_BUS_ACTIVE;

  /* Start the boot zero transaction before ATECC authentication. The second
     bounded attempt intentionally observes the pending SDO and cannot queue
     disable until zero is acknowledged or times out. Auto-retry is disabled. */
  SteeringCan_TryTransmit(now_ms);
  SteeringCan_TryTransmit(now_ms);
  return SAFETY_RESULT_OK;
}

int32_t SteeringCan_SetCommand(int16_t velocity_tdeg_per_s,
                               uint8_t enable, uint8_t source,
                               uint8_t flags, uint32_t now_ms)
{
  if (steering_can_initialized == 0U)
  {
    return SAFETY_RESULT_NOT_READY;
  }
  if (steering_safe_requested != 0U)
  {
    return SAFETY_RESULT_NOT_READY;
  }
  return SteeringControl_SetCommand(&steering_control,
                                    velocity_tdeg_per_s, enable,
                                    source, flags, now_ms);
}

void SteeringCan_RequestSafe(uint32_t now_ms)
{
  /* May be called by ADC/EXTI/exception paths. Do not touch the FDCAN HAL or
     the state-machine object here; the 1 ms owner consumes this atomic word. */
  steering_safe_requested = 1U;
  __DMB();
  (void)now_ms;
}

void SteeringCan_RecordCommandRejected(uint32_t now_ms)
{
  SteeringControl_RecordCommandRejected(&steering_control, now_ms);
}

void SteeringCan_Process(uint32_t now_ms)
{
  uint32_t safe_requested;
  uint32_t primask;

  if (steering_can_initialized == 0U)
  {
    return;
  }
  primask = SteeringCan_EnterCritical();
  safe_requested = steering_safe_requested;
  steering_safe_requested = 0U;
  SteeringCan_ExitCritical(primask);
  if (safe_requested != 0U)
  {
    SteeringControl_RequestSafe(&steering_control, now_ms);
  }
  SteeringCan_ProcessIrqErrors(now_ms);
  SteeringCan_ProcessRx(now_ms);
  SteeringCan_PollStatus(now_ms);
  SteeringControl_Step(&steering_control, now_ms);
  SteeringCan_TryTransmit(now_ms);
}

bool SteeringCan_ConsumeActiveFault(void)
{
  return SteeringControl_ConsumeActiveFault(&steering_control);
}

bool SteeringCan_CanClearFault(void)
{
  return (steering_can_initialized != 0U) &&
         SteeringControl_CanClearFault(&steering_control);
}

void SteeringCan_ClearFaults(void)
{
  SteeringControl_ClearFaults(&steering_control);
}

void SteeringCan_GetSnapshot(SAFETY_SteeringSnapshot *snapshot,
                             uint32_t now_ms,
                             uint32_t command_sequence)
{
  if (snapshot == NULL)
  {
    return;
  }
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->api_version = SAFETY_STEERING_API_VERSION;
  snapshot->size = sizeof(*snapshot);
  snapshot->status_flags = steering_control.status_flags;
  snapshot->fault_flags = steering_control.fault_flags;
  snapshot->command_sequence = command_sequence;
  snapshot->timestamp_ms = now_ms;
  snapshot->command_age_ms = (steering_control.owner_valid != 0U) ?
      now_ms - steering_control.command_tick_ms : UINT32_MAX;
  snapshot->rx_age_ms = (steering_control.rx_valid != 0U) ?
      now_ms - steering_control.last_rx_tick_ms : UINT32_MAX;
  snapshot->tx_frames = steering_control.tx_frames;
  snapshot->rx_frames = steering_control.rx_frames;
  snapshot->tx_errors = steering_control.tx_errors;
  snapshot->rx_errors = steering_control.rx_errors;
  snapshot->tx_deferred = steering_control.tx_deferred;
  snapshot->bus_off_events = steering_control.bus_off_events;
  snapshot->requested_velocity_tdeg_per_s =
      steering_control.requested_velocity_tdeg_per_s;
  snapshot->applied_velocity_tdeg_per_s =
      steering_control.applied_velocity_tdeg_per_s;
  snapshot->speed_command_permille =
      steering_control.speed_command_permille;
  snapshot->motor_speed_feedback_raw =
      steering_control.motor_speed_feedback_raw;
  snapshot->motor_fault_code = steering_control.motor_fault_code;
  snapshot->source = steering_control.source;
  snapshot->state = steering_control.state;
  snapshot->bus_state = steering_control.bus_state;
  snapshot->command_enable = steering_control.enable_request;
  snapshot->motor_enable_confirmed =
      steering_control.motor_enable_confirmed;
}

void SteeringCan_HandleRxFifo0(FDCAN_HandleTypeDef *hfdcan,
                               uint32_t rx_fifo0_its)
{
  uint32_t handled = 0U;

  if ((hfdcan == NULL) || (hfdcan->Instance != FDCAN2))
  {
    return;
  }
  if ((rx_fifo0_its & (FDCAN_IT_RX_FIFO0_FULL |
                       FDCAN_IT_RX_FIFO0_MESSAGE_LOST)) != 0U)
  {
    ++steering_irq_rx_errors;
  }
  while ((HAL_FDCAN_GetRxFifoFillLevel(hfdcan, FDCAN_RX_FIFO0) != 0U) &&
         (handled < STEERING_CAN_RX_RING_SIZE))
  {
    FDCAN_RxHeaderTypeDef header = {0};
    uint8_t data[8] = {0};
    uint32_t write_sequence;
    SteeringCanRxItem *item;

    if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0,
                               &header, data) != HAL_OK)
    {
      ++steering_irq_rx_errors;
      break;
    }
    ++handled;
    if ((header.IdType != FDCAN_EXTENDED_ID) ||
        (header.RxFrameType != FDCAN_DATA_FRAME) ||
        (header.FDFormat != FDCAN_CLASSIC_CAN) ||
        (header.DataLength != FDCAN_DLC_BYTES_8))
    {
      ++steering_irq_rx_errors;
      continue;
    }
    write_sequence = steering_rx_write_sequence;
    if ((write_sequence - steering_rx_read_sequence) >=
        STEERING_CAN_RX_RING_SIZE)
    {
      ++steering_irq_rx_errors;
      continue;
    }
    item = &steering_rx_ring[write_sequence & STEERING_CAN_RX_RING_MASK];
    item->extended_id = header.Identifier;
    /* HAL DLC representation is device-family specific. The strict equality
       above establishes the wire length; never truncate the encoded value. */
    item->length = 8U;
    memcpy(item->data, data, sizeof(item->data));
    __DMB();
    steering_rx_write_sequence = write_sequence + 1U;
  }
}

void SteeringCan_HandleErrorStatus(FDCAN_HandleTypeDef *hfdcan,
                                   uint32_t error_status_its)
{
  if ((hfdcan != NULL) && (hfdcan->Instance == FDCAN2))
  {
    steering_irq_error_status |= error_status_its;
  }
}

void SteeringCan_HandleError(FDCAN_HandleTypeDef *hfdcan)
{
  if ((hfdcan != NULL) && (hfdcan->Instance == FDCAN2))
  {
    steering_irq_hal_errors |= hfdcan->ErrorCode;
    hfdcan->ErrorCode = HAL_FDCAN_ERROR_NONE;
  }
}
