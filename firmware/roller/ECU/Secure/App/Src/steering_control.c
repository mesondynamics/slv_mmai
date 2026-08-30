#include "steering_control.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define STEERING_SDO_COMMAND_DOWNLOAD_RESPONSE 0x60U
#define STEERING_SDO_COMMAND_ABORT             0x80U
#define STEERING_SDO_INDEX_ENABLE              0x200DU
#define STEERING_SDO_INDEX_DISABLE             0x200CU
#define STEERING_SDO_INDEX_SPEED               0x2000U
#define STEERING_SDO_REQUEST_SUBINDEX          0x01U
#define STEERING_SDO_RESPONSE_SUBINDEX         0x00U
#define STEERING_TDEG_PER_SECOND_PER_RPM       60L
#define STEERING_SPEED_COMMAND_MAX_PERMILLE    1000L

static bool SteeringControl_TimeReached(uint32_t now_ms,
                                        uint32_t deadline_ms)
{
  return (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool SteeringControl_IsManualSource(uint8_t source)
{
  return source != (uint8_t)SAFETY_STEERING_SOURCE_AUTONOMOUS;
}

static bool SteeringControl_IsMoving(const SteeringControl *control)
{
  return (control->enable_request != 0U) &&
         (control->requested_velocity_tdeg_per_s != 0);
}

static int16_t SteeringControl_VelocityToSpeedCommandPermille(
    int16_t velocity_tdeg_per_s)
{
  int32_t command;
  const int32_t rated_velocity_tdeg_per_s =
      STEERING_MOTOR_RATED_SPEED_RPM *
      STEERING_TDEG_PER_SECOND_PER_RPM;

  if (velocity_tdeg_per_s == 0)
  {
    return 0;
  }
  /* LK170DD01005-083 manual parameter 0002 specifies 80 rpm rated speed;
     object 0x2000 is a signed per-mille-of-rated-speed command, not RPM.
     One motor rpm is 60 deci-degree/s. The installed motor direction is
     opposite to the vehicle-positive steering-rate convention. */
  command = -((int32_t)velocity_tdeg_per_s *
              STEERING_SPEED_COMMAND_MAX_PERMILLE) /
            rated_velocity_tdeg_per_s;
  if (command == 0)
  {
    command = (velocity_tdeg_per_s > 0) ? -1 : 1;
  }
  if (command > STEERING_SPEED_COMMAND_MAX_PERMILLE)
  {
    command = STEERING_SPEED_COMMAND_MAX_PERMILLE;
  }
  else if (command < -STEERING_SPEED_COMMAND_MAX_PERMILLE)
  {
    command = -STEERING_SPEED_COMMAND_MAX_PERMILLE;
  }
  return (int16_t)command;
}

static int16_t SteeringControl_SpeedCommandToAppliedVelocity(
    int16_t speed_command_permille)
{
  int32_t velocity =
      -((int32_t)speed_command_permille *
        STEERING_MOTOR_RATED_SPEED_RPM *
        STEERING_TDEG_PER_SECOND_PER_RPM) /
      STEERING_SPEED_COMMAND_MAX_PERMILLE;

  return (int16_t)velocity;
}

static void SteeringControl_UpdateStatus(SteeringControl *control)
{
  uint32_t status = SAFETY_STEERING_STATUS_READY;

  if (control->enable_request != 0U)
  {
    status |= SAFETY_STEERING_STATUS_ENABLE_REQUEST;
    status |= SAFETY_STEERING_STATUS_RATE_MODE;
  }
  if (control->motor_enable_confirmed != 0U)
  {
    status |= SAFETY_STEERING_STATUS_ENABLE_CONFIRMED;
  }
  if ((control->state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING) ||
      (control->state == SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING) ||
      (control->state == SAFETY_STEERING_STATE_ENABLE_PENDING) ||
      (control->state == SAFETY_STEERING_STATE_ENABLE_WAIT_ACK) ||
      (control->pending_response != STEERING_CONTROL_RESPONSE_NONE))
  {
    status |= SAFETY_STEERING_STATUS_TX_PENDING;
  }
  status |= control->status_flags &
            (SAFETY_STEERING_STATUS_REARM_REQUIRED |
             SAFETY_STEERING_STATUS_MANUAL_LIMIT |
             SAFETY_STEERING_STATUS_SOURCE_SWITCH |
             SAFETY_STEERING_STATUS_COMMAND_REJECTED);
  control->status_flags = status;
}

static void SteeringControl_StartSafeSequenceInternal(
    SteeringControl *control, uint32_t now_ms,
    bool force_confirmation_when_disabled)
{
  bool safe_transaction_active;
  bool already_disabled;

  safe_transaction_active =
      ((control->state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING) &&
       (control->pending_response == STEERING_CONTROL_RESPONSE_NONE)) ||
      ((control->state == SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING) &&
       ((control->pending_response == STEERING_CONTROL_RESPONSE_NONE) ||
        (control->pending_response == STEERING_CONTROL_RESPONSE_SAFE_ZERO) ||
        (control->pending_response == STEERING_CONTROL_RESPONSE_DISABLE)));
  already_disabled =
      (control->state == SAFETY_STEERING_STATE_DISABLED) &&
      (control->pending_response == STEERING_CONTROL_RESPONSE_NONE) &&
      (control->motor_enable_confirmed == 0U);

  control->enable_request = 0U;
  control->requested_velocity_tdeg_per_s = 0;
  control->applied_velocity_tdeg_per_s = 0;
  control->speed_command_permille = 0;
  control->manual_nonzero_active = 0U;
  /* Once a safety request is accepted, any earlier enable acknowledgement is
     no longer evidence that motion is permitted. A fresh, transaction-bound
     enable acknowledgement is required before another non-zero command. */
  control->motor_enable_confirmed = 0U;
  if (!safe_transaction_active &&
      (!already_disabled || force_confirmation_when_disabled))
  {
    control->pending_response = STEERING_CONTROL_RESPONSE_NONE;
    control->state = SAFETY_STEERING_STATE_SAFE_ZERO_PENDING;
    control->next_tx_tick_ms = now_ms;
  }
  /* Repeated E-stop/fault requests are intentionally idempotent while the
     zero-then-disable transaction is already in flight. Preserve its matched
     response kind, original deadline and retransmit schedule so a valid ACK
     cannot be orphaned by a 1 ms safety poll. */
  SteeringControl_UpdateStatus(control);
}

static void SteeringControl_StartSafeSequence(SteeringControl *control,
                                              uint32_t now_ms)
{
  /* Internal state-machine interlocks (rearm, source handover, watchdogs and
     rejected commands) require a fresh zero/disable proof even if the last
     known state was disabled. */
  SteeringControl_StartSafeSequenceInternal(control, now_ms, true);
}

static void SteeringControl_RequireFaultSafeConfirmation(
    SteeringControl *control, uint32_t now_ms)
{
  control->fault_safe_confirmation_required = 1U;
  /* A newly observed transport, bus or motor fault must be followed by a
     transaction-bound zero/disable acknowledgement even when the state
     machine was already disabled. Repeated reports remain idempotent while
     that proof is in flight. This is deliberately distinct from the 1 ms
     periodic RequestSafe path, which must not restart a completed proof. */
  SteeringControl_StartSafeSequenceInternal(control, now_ms, true);
}

static void SteeringControl_ApplyDisabledCommand(SteeringControl *control,
                                                 uint32_t now_ms)
{
  bool safe_transaction_active;
  bool already_disabled;

  safe_transaction_active =
      ((control->state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING) &&
       (control->pending_response == STEERING_CONTROL_RESPONSE_NONE)) ||
      ((control->state == SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING) &&
       ((control->pending_response == STEERING_CONTROL_RESPONSE_NONE) ||
        (control->pending_response == STEERING_CONTROL_RESPONSE_SAFE_ZERO) ||
        (control->pending_response == STEERING_CONTROL_RESPONSE_DISABLE)));
  already_disabled =
      (control->state == SAFETY_STEERING_STATE_DISABLED) &&
      (control->pending_response == STEERING_CONTROL_RESPONSE_NONE) &&
      (control->motor_enable_confirmed == 0U);

  if (!safe_transaction_active && !already_disabled)
  {
    SteeringControl_StartSafeSequence(control, now_ms);
    return;
  }

  /* A 20 ms network owner repeatedly submits its selected command. Once a
     zero/disable transaction is in progress, identical disabled commands are
     idempotent: preserve the response kind, original deadline and next frame.
     Likewise, an already disabled motor waits for the normal 250 ms refresh. */
  control->enable_request = 0U;
  control->requested_velocity_tdeg_per_s = 0;
  control->applied_velocity_tdeg_per_s = 0;
  control->speed_command_permille = 0;
  control->manual_nonzero_active = 0U;
  control->motor_enable_confirmed = 0U;
  SteeringControl_UpdateStatus(control);
}

void SteeringControl_Init(SteeringControl *control, uint32_t now_ms)
{
  if (control == NULL)
  {
    return;
  }
  memset(control, 0, sizeof(*control));
  control->state = SAFETY_STEERING_STATE_SAFE_ZERO_PENDING;
  control->bus_state = SAFETY_STEERING_BUS_STOPPED;
  control->command_tick_ms = now_ms;
  control->last_rx_tick_ms = now_ms;
  control->last_response_tick_ms = now_ms;
  control->next_tx_tick_ms = now_ms;
  control->safe_refresh_tick_ms = now_ms +
                                  STEERING_CONTROL_SAFE_REFRESH_MS;
  SteeringControl_UpdateStatus(control);
}

bool SteeringControl_CommandValuesValid(int16_t velocity_tdeg_per_s,
                                        uint8_t enable, uint8_t flags,
                                        uint8_t run_permit_on)
{
  if ((enable > 1U) ||
      ((flags & (uint8_t)~SAFETY_STEERING_ALLOWED_FLAGS) != 0U) ||
      (velocity_tdeg_per_s < -SAFETY_STEERING_VELOCITY_MAX_TDEG_PER_S) ||
      (velocity_tdeg_per_s > SAFETY_STEERING_VELOCITY_MAX_TDEG_PER_S))
  {
    return false;
  }
  if ((enable == 0U) && (velocity_tdeg_per_s != 0))
  {
    return false;
  }
  if ((enable != 0U) &&
      ((flags & SAFETY_STEERING_FLAG_RATE_MODE) == 0U))
  {
    return false;
  }
  if ((enable != 0U) && (run_permit_on == 0U))
  {
    return false;
  }
  return true;
}

int32_t SteeringControl_SetCommand(SteeringControl *control,
                                   int16_t velocity_tdeg_per_s,
                                   uint8_t enable, uint8_t source,
                                   uint8_t flags, uint32_t now_ms)
{
  bool fully_disabled;
  bool moving_command;
  bool source_changed;

  if (control == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  if ((source < (uint8_t)SAFETY_STEERING_SOURCE_REMOTE) ||
      (source > (uint8_t)SAFETY_STEERING_SOURCE_AUTONOMOUS))
  {
    return SAFETY_RESULT_RANGE;
  }
  if (!SteeringControl_CommandValuesValid(velocity_tdeg_per_s, enable,
                                          flags, 1U))
  {
    return SAFETY_RESULT_RANGE;
  }

  /* Rearm requires an explicit fully disabled zero command. An enabled-zero
     command is useful for holding an already healthy motor, but must never
     clear a manual-limit, source-handover or transport-fault interlock. */
  fully_disabled = (enable == 0U) && (velocity_tdeg_per_s == 0);
  moving_command = (enable != 0U) && (velocity_tdeg_per_s != 0);
  source_changed = (control->owner_valid != 0U) &&
                   (control->source != source);
  control->command_tick_ms = now_ms;

  if (source_changed)
  {
    control->source = source;
    control->status_flags |= SAFETY_STEERING_STATUS_SOURCE_SWITCH |
                             SAFETY_STEERING_STATUS_REARM_REQUIRED;
    control->rearm_required = 1U;
    SteeringControl_StartSafeSequence(control, now_ms);
    if (!fully_disabled)
    {
      return SAFETY_RESULT_RATE_LIMITED;
    }
  }
  else
  {
    control->source = source;
  }
  control->owner_valid = 1U;

  if (fully_disabled)
  {
    control->rearm_required = 0U;
    control->manual_nonzero_active = 0U;
    control->status_flags &= ~(SAFETY_STEERING_STATUS_REARM_REQUIRED |
                               SAFETY_STEERING_STATUS_MANUAL_LIMIT |
                               SAFETY_STEERING_STATUS_SOURCE_SWITCH |
                               SAFETY_STEERING_STATUS_COMMAND_REJECTED);
  }
  else if (control->rearm_required != 0U)
  {
    SteeringControl_StartSafeSequence(control, now_ms);
    return SAFETY_RESULT_RATE_LIMITED;
  }

  if (enable == 0U)
  {
    SteeringControl_ApplyDisabledCommand(control, now_ms);
    return SAFETY_RESULT_OK;
  }
  if (control->fault_flags != 0U)
  {
    SteeringControl_StartSafeSequence(control, now_ms);
    return SAFETY_RESULT_NOT_READY;
  }

  control->enable_request = 1U;
  control->requested_velocity_tdeg_per_s = velocity_tdeg_per_s;
  control->speed_command_permille =
      SteeringControl_VelocityToSpeedCommandPermille(
          velocity_tdeg_per_s);

  if (!moving_command)
  {
    control->manual_nonzero_active = 0U;
  }
  if (SteeringControl_IsManualSource(source) && moving_command &&
      (control->manual_nonzero_active == 0U))
  {
    control->manual_nonzero_active = 1U;
    control->manual_nonzero_start_ms = now_ms;
  }
  if (control->state == SAFETY_STEERING_STATE_DISABLED)
  {
    control->state = SAFETY_STEERING_STATE_ENABLE_PENDING;
    control->next_tx_tick_ms = now_ms;
  }
  else if ((control->state == SAFETY_STEERING_STATE_ACTIVE) &&
           (control->motor_enable_confirmed != 0U))
  {
    control->next_tx_tick_ms = now_ms;
  }
  /* A boot/fault zero-disable pair already in progress is never bypassed.
     ConfirmTransmit() transitions to ENABLE_PENDING after the disable frame. */
  SteeringControl_UpdateStatus(control);
  return SAFETY_RESULT_OK;
}

void SteeringControl_RequestSafe(SteeringControl *control, uint32_t now_ms)
{
  if (control == NULL)
  {
    return;
  }
  /* This asynchronous request may be raised every 1 ms while an E-stop or
     global fault remains asserted. It is idempotent both during an in-flight
     proof and after a successfully acknowledged disabled state. */
  SteeringControl_StartSafeSequenceInternal(control, now_ms, false);
}

void SteeringControl_RecordCommandRejected(SteeringControl *control,
                                           uint32_t now_ms)
{
  if (control == NULL)
  {
    return;
  }
  control->status_flags |= SAFETY_STEERING_STATUS_COMMAND_REJECTED |
                           SAFETY_STEERING_STATUS_REARM_REQUIRED;
  control->rearm_required = 1U;
  SteeringControl_StartSafeSequence(control, now_ms);
}

void SteeringControl_Step(SteeringControl *control, uint32_t now_ms)
{
  SteeringControlPendingResponse timed_out_response;

  if (control == NULL)
  {
    return;
  }
  if (SteeringControl_IsMoving(control) &&
      SteeringControl_IsManualSource(control->source) &&
      (control->manual_nonzero_active != 0U) &&
      ((uint32_t)(now_ms - control->manual_nonzero_start_ms) >=
       SAFETY_STEERING_MANUAL_MAX_MS))
  {
    control->status_flags |= SAFETY_STEERING_STATUS_MANUAL_LIMIT |
                             SAFETY_STEERING_STATUS_REARM_REQUIRED;
    control->rearm_required = 1U;
    SteeringControl_StartSafeSequence(control, now_ms);
  }
  else if (SteeringControl_IsMoving(control) &&
           (control->motor_enable_confirmed != 0U) &&
           ((control->response_valid == 0U) ||
            ((uint32_t)(now_ms - control->last_response_tick_ms) >
             STEERING_CONTROL_RESPONSE_TIMEOUT_MS)))
  {
    control->fault_flags |= SAFETY_STEERING_FAULT_PROTOCOL;
    control->status_flags |= SAFETY_STEERING_STATUS_REARM_REQUIRED;
    control->rearm_required = 1U;
    control->active_fault_pending = 1U;
    SteeringControl_StartSafeSequence(control, now_ms);
  }

  if ((control->pending_response != STEERING_CONTROL_RESPONSE_NONE) &&
      ((uint32_t)(now_ms - control->pending_response_start_ms) >
       STEERING_CONTROL_RESPONSE_TIMEOUT_MS))
  {
    timed_out_response =
        (SteeringControlPendingResponse)control->pending_response;
    control->pending_response = STEERING_CONTROL_RESPONSE_NONE;
    control->fault_flags |= SAFETY_STEERING_FAULT_PROTOCOL;
    ++control->rx_errors;

    if (timed_out_response == STEERING_CONTROL_RESPONSE_SAFE_ZERO)
    {
      /* A missing zero acknowledgement must not prevent the subsequent
         disable request. Motion remains locally unconfirmed and clamped. */
      if (control->enable_request != 0U)
      {
        control->enable_request = 0U;
        control->requested_velocity_tdeg_per_s = 0;
        control->speed_command_permille = 0;
        control->status_flags |= SAFETY_STEERING_STATUS_REARM_REQUIRED;
        control->rearm_required = 1U;
        control->active_fault_pending = 1U;
      }
      control->state = SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING;
      control->next_tx_tick_ms = now_ms;
    }
    else if (timed_out_response == STEERING_CONTROL_RESPONSE_DISABLE)
    {
      control->motor_enable_confirmed = 0U;
      if (control->enable_request != 0U)
      {
        control->enable_request = 0U;
        control->requested_velocity_tdeg_per_s = 0;
        control->speed_command_permille = 0;
        control->status_flags |= SAFETY_STEERING_STATUS_REARM_REQUIRED;
        control->rearm_required = 1U;
        control->active_fault_pending = 1U;
      }
      /* A timeout is not a disable acknowledgement. Remain fail-closed and
         retry the disable transaction; fault clearing requires a successful
         zero/disable sequence, never an inferred physical state. */
      control->state = SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING;
      control->next_tx_tick_ms = now_ms;
    }
    else
    {
      control->status_flags |= SAFETY_STEERING_STATUS_REARM_REQUIRED;
      control->rearm_required = 1U;
      control->active_fault_pending = 1U;
      SteeringControl_StartSafeSequence(control, now_ms);
    }
  }

  if ((control->state == SAFETY_STEERING_STATE_DISABLED) &&
      SteeringControl_TimeReached(now_ms, control->safe_refresh_tick_ms))
  {
    control->state = SAFETY_STEERING_STATE_SAFE_ZERO_PENDING;
    control->next_tx_tick_ms = now_ms;
  }
  SteeringControl_UpdateStatus(control);
}

static void SteeringControl_BuildSdoFrame(SteeringControlFrame *frame,
                                          SteeringControlFrameKind kind,
                                          uint16_t index)
{
  memset(frame, 0, sizeof(*frame));
  frame->kind = kind;
  frame->extended_id = STEERING_CONTROL_TX_EXT_ID;
  frame->length = 8U;
  frame->data[0] = 0x23U;
  frame->data[1] = (uint8_t)index;
  frame->data[2] = (uint8_t)(index >> 8U);
  frame->data[3] = STEERING_SDO_REQUEST_SUBINDEX;
}

bool SteeringControl_GetPendingFrame(SteeringControl *control,
                                     uint32_t now_ms,
                                     SteeringControlFrame *frame)
{
  uint16_t raw_speed_command;

  if ((control == NULL) || (frame == NULL) ||
      (control->pending_response != STEERING_CONTROL_RESPONSE_NONE) ||
      !SteeringControl_TimeReached(now_ms, control->next_tx_tick_ms))
  {
    return false;
  }
  switch ((SAFETY_SteeringState)control->state)
  {
    case SAFETY_STEERING_STATE_SAFE_ZERO_PENDING:
      SteeringControl_BuildSdoFrame(frame, STEERING_CONTROL_FRAME_SAFE_ZERO,
                                    STEERING_SDO_INDEX_SPEED);
      break;
    case SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING:
      SteeringControl_BuildSdoFrame(frame, STEERING_CONTROL_FRAME_DISABLE,
                                    STEERING_SDO_INDEX_DISABLE);
      break;
    case SAFETY_STEERING_STATE_ENABLE_PENDING:
    case SAFETY_STEERING_STATE_ENABLE_WAIT_ACK:
      SteeringControl_BuildSdoFrame(frame, STEERING_CONTROL_FRAME_ENABLE,
                                    STEERING_SDO_INDEX_ENABLE);
      break;
    case SAFETY_STEERING_STATE_ACTIVE:
      SteeringControl_BuildSdoFrame(frame, STEERING_CONTROL_FRAME_SPEED,
                                    STEERING_SDO_INDEX_SPEED);
      raw_speed_command = (uint16_t)control->speed_command_permille;
      frame->data[4] = (uint8_t)(raw_speed_command >> 8U);
      frame->data[5] = (uint8_t)raw_speed_command;
      frame->data[6] = (control->speed_command_permille < 0) ? 0xFFU : 0x00U;
      frame->data[7] = frame->data[6];
      break;
    case SAFETY_STEERING_STATE_DISABLED:
    default:
      return false;
  }
  return true;
}

void SteeringControl_ConfirmTransmit(SteeringControl *control,
                                     SteeringControlFrameKind kind,
                                     uint32_t now_ms)
{
  if (control == NULL)
  {
    return;
  }
  ++control->tx_frames;
  switch (kind)
  {
    case STEERING_CONTROL_FRAME_SAFE_ZERO:
      control->state = SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING;
      control->pending_response = STEERING_CONTROL_RESPONSE_SAFE_ZERO;
      control->pending_response_start_ms = now_ms;
      break;
    case STEERING_CONTROL_FRAME_DISABLE:
      control->pending_response = STEERING_CONTROL_RESPONSE_DISABLE;
      control->pending_response_start_ms = now_ms;
      break;
    case STEERING_CONTROL_FRAME_ENABLE:
      control->state = SAFETY_STEERING_STATE_ENABLE_WAIT_ACK;
      control->pending_response = STEERING_CONTROL_RESPONSE_ENABLE;
      control->pending_response_start_ms = now_ms;
      break;
    case STEERING_CONTROL_FRAME_SPEED:
      control->last_transmitted_speed_command_permille =
          control->speed_command_permille;
      control->last_transmitted_applied_velocity_tdeg_per_s =
          SteeringControl_SpeedCommandToAppliedVelocity(
              control->speed_command_permille);
      control->pending_response = STEERING_CONTROL_RESPONSE_SPEED;
      control->pending_response_start_ms = now_ms;
      break;
    case STEERING_CONTROL_FRAME_NONE:
    default:
      break;
  }
  SteeringControl_UpdateStatus(control);
}

void SteeringControl_RecordTxDeferred(SteeringControl *control,
                                      uint32_t now_ms)
{
  if (control == NULL)
  {
    return;
  }
  ++control->tx_deferred;
  control->next_tx_tick_ms = now_ms + 1U;
}

void SteeringControl_RecordTxError(SteeringControl *control,
                                   uint32_t now_ms)
{
  bool active;
  bool response_pending;

  if (control == NULL)
  {
    return;
  }
  active = (control->enable_request != 0U) ||
           (control->motor_enable_confirmed != 0U);
  response_pending =
      control->pending_response != STEERING_CONTROL_RESPONSE_NONE;
  ++control->tx_errors;
  control->fault_flags |= SAFETY_STEERING_FAULT_TX;
  if (active)
  {
    control->status_flags |= SAFETY_STEERING_STATUS_REARM_REQUIRED;
    control->rearm_required = 1U;
    control->active_fault_pending = 1U;
  }
  SteeringControl_RequireFaultSafeConfirmation(control, now_ms);
  if (!response_pending &&
      (control->pending_response == STEERING_CONTROL_RESPONSE_NONE))
  {
    control->next_tx_tick_ms = now_ms + 5U;
  }
}

void SteeringControl_RecordBusState(SteeringControl *control,
                                    uint8_t bus_state,
                                    uint32_t tx_error_count,
                                    uint32_t rx_error_count,
                                    uint32_t now_ms)
{
  bool active;

  if (control == NULL)
  {
    return;
  }
  active = (control->enable_request != 0U) ||
           (control->motor_enable_confirmed != 0U);
  if ((bus_state == SAFETY_STEERING_BUS_OFF) &&
      (control->bus_state != SAFETY_STEERING_BUS_OFF))
  {
    ++control->bus_off_events;
  }
  control->bus_state = bus_state;
  /* Hardware TEC/REC are instantaneous, non-monotonic values. The public
     counters remain cumulative and are updated by IRQ/error events. */
  (void)tx_error_count;
  (void)rx_error_count;
  if (bus_state == SAFETY_STEERING_BUS_OFF)
  {
    control->fault_flags |= SAFETY_STEERING_FAULT_BUS_OFF;
    control->motor_enable_confirmed = 0U;
    if (active)
    {
      control->status_flags |= SAFETY_STEERING_STATUS_REARM_REQUIRED;
      control->rearm_required = 1U;
      control->active_fault_pending = 1U;
    }
    SteeringControl_RequireFaultSafeConfirmation(control, now_ms);
  }
  SteeringControl_UpdateStatus(control);
}

void SteeringControl_RecordProtocolError(SteeringControl *control,
                                         uint32_t fault_flags,
                                         uint32_t now_ms)
{
  bool active;

  if (control == NULL)
  {
    return;
  }
  active = (control->enable_request != 0U) ||
           (control->motor_enable_confirmed != 0U);
  control->fault_flags |= fault_flags;
  ++control->rx_errors;
  if (active)
  {
    control->motor_enable_confirmed = 0U;
    control->status_flags |= SAFETY_STEERING_STATUS_REARM_REQUIRED;
    control->rearm_required = 1U;
    control->active_fault_pending = 1U;
  }
  SteeringControl_RequireFaultSafeConfirmation(control, now_ms);
}

static uint16_t SteeringControl_ReadIndex(const uint8_t *data)
{
  return (uint16_t)data[1] | ((uint16_t)data[2] << 8U);
}

static uint32_t SteeringControl_ReadLe32(const uint8_t *data)
{
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8U) |
         ((uint32_t)data[2] << 16U) |
         ((uint32_t)data[3] << 24U);
}

static uint16_t SteeringControl_ReadBe16(const uint8_t *data)
{
  return ((uint16_t)data[0] << 8U) | (uint16_t)data[1];
}

static uint16_t SteeringControl_ExpectedIndex(
    SteeringControlPendingResponse pending_response)
{
  switch (pending_response)
  {
    case STEERING_CONTROL_RESPONSE_SAFE_ZERO:
    case STEERING_CONTROL_RESPONSE_SPEED:
      return STEERING_SDO_INDEX_SPEED;
    case STEERING_CONTROL_RESPONSE_DISABLE:
      return STEERING_SDO_INDEX_DISABLE;
    case STEERING_CONTROL_RESPONSE_ENABLE:
      return STEERING_SDO_INDEX_ENABLE;
    case STEERING_CONTROL_RESPONSE_NONE:
    default:
      return 0U;
  }
}

static bool SteeringControl_ResponseStateMatches(
    const SteeringControl *control,
    SteeringControlPendingResponse pending_response)
{
  switch (pending_response)
  {
    case STEERING_CONTROL_RESPONSE_SAFE_ZERO:
    case STEERING_CONTROL_RESPONSE_DISABLE:
      return control->state == SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING;
    case STEERING_CONTROL_RESPONSE_ENABLE:
      return (control->state == SAFETY_STEERING_STATE_ENABLE_WAIT_ACK) &&
             (control->enable_request != 0U) &&
             (control->rearm_required == 0U);
    case STEERING_CONTROL_RESPONSE_SPEED:
      return (control->state == SAFETY_STEERING_STATE_ACTIVE) &&
             (control->enable_request != 0U) &&
             (control->motor_enable_confirmed != 0U);
    case STEERING_CONTROL_RESPONSE_NONE:
    default:
      return false;
  }
}

static void SteeringControl_RecordMatchedResponse(
    SteeringControl *control, uint32_t now_ms)
{
  ++control->rx_frames;
  control->last_rx_tick_ms = now_ms;
  control->last_response_tick_ms = now_ms;
  control->rx_valid = 1U;
  control->response_valid = 1U;
}

bool SteeringControl_HandleRxFrame(SteeringControl *control,
                                   uint32_t extended_id,
                                   const uint8_t *data, uint8_t length,
                                   uint32_t now_ms)
{
  uint16_t index;
  uint16_t expected_index;
  uint16_t heartbeat_dtc;
  SteeringControlPendingResponse pending_response;
  bool active;

  if ((control == NULL) || (data == NULL) || (length > 8U))
  {
    return false;
  }
  if (extended_id == STEERING_CONTROL_HEARTBEAT_EXT_ID)
  {
    if (length != 8U)
    {
      ++control->rx_errors;
      return false;
    }
    /* LK170DD01005-083 heartbeat words are big-endian: electrical angle,
       signed motor-speed feedback, signed speed-given and controller DTC.
       It proves only diagnostic CAN activity; it cannot complete an SDO,
       confirm enable, or refresh the strict SDO response watchdog. */
    control->motor_speed_feedback_raw =
        (int16_t)SteeringControl_ReadBe16(&data[2]);
    control->heartbeat_speed_given_permille =
        (int16_t)SteeringControl_ReadBe16(&data[4]);
    heartbeat_dtc = SteeringControl_ReadBe16(&data[6]);
    control->motor_fault_code = heartbeat_dtc;
    ++control->rx_frames;
    control->last_rx_tick_ms = now_ms;
    control->rx_valid = 1U;
    if (heartbeat_dtc != 0U)
    {
      control->fault_flags |= SAFETY_STEERING_FAULT_MOTOR_DTC;
      active = (control->enable_request != 0U) ||
               (control->motor_enable_confirmed != 0U);
      if (active)
      {
        control->status_flags |= SAFETY_STEERING_STATUS_REARM_REQUIRED;
        control->rearm_required = 1U;
        control->active_fault_pending = 1U;
      }
      SteeringControl_RequireFaultSafeConfirmation(control, now_ms);
    }
    SteeringControl_UpdateStatus(control);
    return true;
  }
  if ((extended_id != STEERING_CONTROL_SDO_RESPONSE_EXT_ID) ||
      (length != 8U))
  {
    ++control->rx_errors;
    return false;
  }

  index = SteeringControl_ReadIndex(data);
  pending_response =
      (SteeringControlPendingResponse)control->pending_response;
  expected_index = SteeringControl_ExpectedIndex(pending_response);
  if ((pending_response == STEERING_CONTROL_RESPONSE_NONE) ||
      (index != expected_index) ||
      !SteeringControl_ResponseStateMatches(control, pending_response) ||
      ((data[0] != STEERING_SDO_COMMAND_DOWNLOAD_RESPONSE) &&
       (data[0] != STEERING_SDO_COMMAND_ABORT)) ||
      ((data[0] == STEERING_SDO_COMMAND_DOWNLOAD_RESPONSE) &&
       (data[3] != STEERING_SDO_RESPONSE_SUBINDEX)) ||
      ((data[0] == STEERING_SDO_COMMAND_ABORT) &&
       (data[3] != STEERING_SDO_REQUEST_SUBINDEX)))
  {
    ++control->rx_errors;
    return false;
  }
  if ((data[0] == STEERING_SDO_COMMAND_DOWNLOAD_RESPONSE) &&
      ((data[4] != 0U) || (data[5] != 0U) ||
       (data[6] != 0U) || (data[7] != 0U)))
  {
    ++control->rx_errors;
    return false;
  }

  SteeringControl_RecordMatchedResponse(control, now_ms);
  control->pending_response = STEERING_CONTROL_RESPONSE_NONE;
  if (data[0] == STEERING_SDO_COMMAND_ABORT)
  {
    control->motor_abort_code = SteeringControl_ReadLe32(&data[4]);
    control->motor_fault_code =
        (control->motor_abort_code > UINT16_MAX) ? UINT16_MAX :
                                                  (uint16_t)control->motor_abort_code;
    SteeringControl_RecordProtocolError(control,
                                        SAFETY_STEERING_FAULT_PROTOCOL,
                                        now_ms);
    if (pending_response == STEERING_CONTROL_RESPONSE_SAFE_ZERO)
    {
      control->state = SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING;
      control->next_tx_tick_ms = now_ms;
    }
    else if (pending_response == STEERING_CONTROL_RESPONSE_DISABLE)
    {
      control->motor_enable_confirmed = 0U;
      control->applied_velocity_tdeg_per_s = 0;
      /* A motor abort explicitly rejects the disable request. Retry it and
         keep CanClearFault() closed until a transaction-bound disable ACK. */
      control->state = SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING;
      control->next_tx_tick_ms = now_ms;
    }
    SteeringControl_UpdateStatus(control);
    return true;
  }

  if (pending_response == STEERING_CONTROL_RESPONSE_SAFE_ZERO)
  {
    control->applied_velocity_tdeg_per_s = 0;
    control->state = SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING;
    control->next_tx_tick_ms = now_ms;
  }
  else if (pending_response == STEERING_CONTROL_RESPONSE_DISABLE)
  {
    control->motor_enable_confirmed = 0U;
    control->applied_velocity_tdeg_per_s = 0;
    control->fault_safe_confirmation_required = 0U;
    if ((control->enable_request != 0U) &&
        (control->rearm_required == 0U))
    {
      control->state = SAFETY_STEERING_STATE_ENABLE_PENDING;
      control->next_tx_tick_ms = now_ms;
    }
    else
    {
      control->state = SAFETY_STEERING_STATE_DISABLED;
      control->safe_refresh_tick_ms = now_ms +
                                      STEERING_CONTROL_SAFE_REFRESH_MS;
    }
  }
  else if (pending_response == STEERING_CONTROL_RESPONSE_ENABLE)
  {
    control->motor_enable_confirmed = 1U;
    control->state = SAFETY_STEERING_STATE_ACTIVE;
    control->next_tx_tick_ms = now_ms;
  }
  else if (pending_response == STEERING_CONTROL_RESPONSE_SPEED)
  {
    control->applied_velocity_tdeg_per_s =
        control->last_transmitted_applied_velocity_tdeg_per_s;
    control->next_tx_tick_ms = now_ms + STEERING_CONTROL_TX_PERIOD_MS;
  }
  SteeringControl_UpdateStatus(control);
  return true;
}

bool SteeringControl_ConsumeActiveFault(SteeringControl *control)
{
  bool pending;

  if (control == NULL)
  {
    return false;
  }
  pending = control->active_fault_pending != 0U;
  control->active_fault_pending = 0U;
  return pending;
}

bool SteeringControl_CanClearFault(const SteeringControl *control)
{
  return (control != NULL) &&
         (control->bus_state != SAFETY_STEERING_BUS_OFF) &&
         (control->state == SAFETY_STEERING_STATE_DISABLED) &&
         (control->pending_response == STEERING_CONTROL_RESPONSE_NONE) &&
         (control->motor_enable_confirmed == 0U) &&
         (control->fault_safe_confirmation_required == 0U) &&
         (control->motor_fault_code == 0U);
}

void SteeringControl_ClearFaults(SteeringControl *control)
{
  if ((control == NULL) || !SteeringControl_CanClearFault(control))
  {
    return;
  }
  control->fault_flags = 0U;
  control->motor_abort_code = 0U;
  control->motor_fault_code = 0U;
  control->active_fault_pending = 0U;
}
