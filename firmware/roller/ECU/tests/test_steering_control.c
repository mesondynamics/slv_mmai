#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "steering_control.h"

_Static_assert(SAFETY_COMMAND_TIMEOUT_MS == 300UL,
               "Secure command watchdog must remain 300 ms");
_Static_assert(sizeof(SAFETY_ActuatorCommand) == 36U,
               "actuator command ABI changed");
_Static_assert(offsetof(SAFETY_ActuatorCommand, steering_enable) == 31U,
               "steering_enable ABI offset changed");
_Static_assert(offsetof(SAFETY_ActuatorCommand, steering_source) == 32U,
               "steering_source ABI offset changed");
_Static_assert(offsetof(SAFETY_ActuatorCommand, steering_flags) == 33U,
               "steering_flags ABI offset changed");
_Static_assert(offsetof(SAFETY_ActuatorCommand,
                        steering_velocity_tdeg_per_s) == 34U,
               "steering velocity ABI offset changed");
_Static_assert(sizeof(SAFETY_ActuatorSnapshot) == 60U,
               "frozen actuator snapshot ABI changed");
_Static_assert(sizeof(SAFETY_SteeringSnapshot) == 72U,
               "steering snapshot ABI changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot,
                        requested_velocity_tdeg_per_s) == 56U,
               "requested velocity ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot,
                        applied_velocity_tdeg_per_s) == 58U,
               "applied velocity ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot,
                        speed_command_permille) == 60U,
               "speed command ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot,
                        motor_speed_feedback_raw) == 62U,
               "motor feedback ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot,
                        motor_fault_code) == 64U,
               "motor fault ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot, source) == 66U,
               "source ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot, state) == 67U,
               "state ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot, bus_state) == 68U,
               "bus state ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot, command_enable) == 69U,
               "command enable ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot,
                        motor_enable_confirmed) == 70U,
               "enable confirmation ABI offset changed");
_Static_assert(offsetof(SAFETY_SteeringSnapshot, reserved) == 71U,
               "reserved tail ABI offset changed");
_Static_assert(STEERING_CONTROL_TX_EXT_ID == 0x06000001UL,
               "supplier motor command ID changed");
_Static_assert(STEERING_CONTROL_SDO_RESPONSE_EXT_ID == 0x05800001UL,
               "supplier motor response ID changed");
_Static_assert(STEERING_CONTROL_HEARTBEAT_EXT_ID == 0x07000001UL,
               "supplier motor heartbeat ID changed");

#define INDEX_SPEED   0x2000U
#define INDEX_DISABLE 0x200CU
#define INDEX_ENABLE  0x200DU

static void make_ack(uint8_t frame[8], uint16_t index, uint8_t subindex)
{
  memset(frame, 0, 8U);
  frame[0] = 0x60U;
  frame[1] = (uint8_t)index;
  frame[2] = (uint8_t)(index >> 8U);
  frame[3] = subindex;
}

static SteeringControlFrame take_frame(SteeringControl *control,
                                       uint32_t now_ms,
                                       SteeringControlFrameKind kind)
{
  SteeringControlFrame frame;
  static const uint8_t zero_request[8] = {
    0x23U, 0x00U, 0x20U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U
  };
  static const uint8_t disable_request[8] = {
    0x23U, 0x0CU, 0x20U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U
  };
  static const uint8_t enable_request[8] = {
    0x23U, 0x0DU, 0x20U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U
  };

  memset(&frame, 0, sizeof(frame));
  assert(SteeringControl_GetPendingFrame(control, now_ms, &frame));
  assert(frame.kind == kind);
  assert(frame.extended_id == 0x06000001UL);
  assert(frame.length == 8U);
  assert(frame.data[0] == 0x23U);
  assert(frame.data[3] == 0x01U);
  if (kind == STEERING_CONTROL_FRAME_SAFE_ZERO)
  {
    assert(memcmp(frame.data, zero_request, sizeof(zero_request)) == 0);
  }
  else if (kind == STEERING_CONTROL_FRAME_DISABLE)
  {
    assert(memcmp(frame.data, disable_request,
                  sizeof(disable_request)) == 0);
  }
  else if (kind == STEERING_CONTROL_FRAME_ENABLE)
  {
    assert(memcmp(frame.data, enable_request,
                  sizeof(enable_request)) == 0);
  }
  return frame;
}

static void confirm_frame(SteeringControl *control,
                          const SteeringControlFrame *frame,
                          uint32_t now_ms)
{
  SteeringControl_ConfirmTransmit(control, frame->kind, now_ms);
}

static void accept_ack(SteeringControl *control, uint16_t index,
                       uint32_t now_ms)
{
  uint8_t ack[8];

  make_ack(ack, index, 0x00U);
  assert(SteeringControl_HandleRxFrame(
      control, STEERING_CONTROL_SDO_RESPONSE_EXT_ID,
      ack, sizeof(ack), now_ms));
}

static void complete_boot_safe_sequence(SteeringControl *control,
                                        uint32_t now_ms)
{
  SteeringControlFrame frame;

  SteeringControl_Init(control, now_ms);
  frame = take_frame(control, now_ms, STEERING_CONTROL_FRAME_SAFE_ZERO);
  assert(frame.data[1] == 0x00U && frame.data[2] == 0x20U);
  confirm_frame(control, &frame, now_ms);
  assert(!SteeringControl_GetPendingFrame(control, now_ms, &frame));
  accept_ack(control, INDEX_SPEED, now_ms + 1U);

  frame = take_frame(control, now_ms + 1U,
                     STEERING_CONTROL_FRAME_DISABLE);
  assert(frame.data[1] == 0x0CU && frame.data[2] == 0x20U);
  confirm_frame(control, &frame, now_ms + 1U);
  accept_ack(control, INDEX_DISABLE, now_ms + 2U);
  assert(control->state == SAFETY_STEERING_STATE_DISABLED);
  assert(control->motor_enable_confirmed == 0U);
}

static void complete_pending_safe_sequence(SteeringControl *control,
                                           uint32_t now_ms)
{
  SteeringControlFrame frame;

  assert(control->state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  frame = take_frame(control, now_ms, STEERING_CONTROL_FRAME_SAFE_ZERO);
  confirm_frame(control, &frame, now_ms);
  accept_ack(control, INDEX_SPEED, now_ms + 1U);
  frame = take_frame(control, now_ms + 1U,
                     STEERING_CONTROL_FRAME_DISABLE);
  confirm_frame(control, &frame, now_ms + 1U);
  accept_ack(control, INDEX_DISABLE, now_ms + 2U);
  assert(control->state == SAFETY_STEERING_STATE_DISABLED);
}

static void enable_and_ack(SteeringControl *control, int16_t velocity,
                           uint8_t source, uint32_t now_ms)
{
  SteeringControlFrame frame;

  assert(SteeringControl_SetCommand(
      control, velocity, 1U, source,
      SAFETY_STEERING_FLAG_RATE_MODE, now_ms) == SAFETY_RESULT_OK);
  frame = take_frame(control, now_ms, STEERING_CONTROL_FRAME_ENABLE);
  confirm_frame(control, &frame, now_ms);
  assert(!SteeringControl_GetPendingFrame(control, now_ms, &frame));
  assert(control->motor_enable_confirmed == 0U);
  accept_ack(control, INDEX_ENABLE, now_ms + 1U);
  assert(control->state == SAFETY_STEERING_STATE_ACTIVE);
  assert(control->motor_enable_confirmed == 1U);
}

static SteeringControlFrame send_speed_and_ack(SteeringControl *control,
                                               uint32_t now_ms)
{
  SteeringControlFrame frame = take_frame(
      control, now_ms, STEERING_CONTROL_FRAME_SPEED);

  confirm_frame(control, &frame, now_ms);
  accept_ack(control, INDEX_SPEED, now_ms + 1U);
  return frame;
}

static void test_command_validation(void)
{
  SteeringControl control;

  assert(SteeringControl_CommandValuesValid(
      0, 0U, 0U, 0U));
  assert(!SteeringControl_CommandValuesValid(
      0, 1U, SAFETY_STEERING_FLAG_RATE_MODE, 0U));
  assert(!SteeringControl_CommandValuesValid(
      1, 1U, SAFETY_STEERING_FLAG_RATE_MODE, 0U));
  assert(SteeringControl_CommandValuesValid(
      6000, 1U, SAFETY_STEERING_FLAG_RATE_MODE, 1U));
  assert(!SteeringControl_CommandValuesValid(
      6001, 1U, SAFETY_STEERING_FLAG_RATE_MODE, 1U));
  assert(!SteeringControl_CommandValuesValid(
      0, 2U, SAFETY_STEERING_FLAG_RATE_MODE, 1U));
  assert(!SteeringControl_CommandValuesValid(
      0, 1U, 0x80U, 1U));

  complete_boot_safe_sequence(&control, 0U);
  assert(SteeringControl_SetCommand(
      &control, 0, 0U, 0U, 0U, 3U) == SAFETY_RESULT_RANGE);
  assert(control.owner_valid == 0U);
  assert(SteeringControl_SetCommand(
      &control, 0, 0U, 4U, 0U, 3U) == SAFETY_RESULT_RANGE);
  assert(control.owner_valid == 0U);
}

static void test_speed_permille_conversion_and_ack_gate(void)
{
  SteeringControl control;
  SteeringControlFrame frame;

  complete_boot_safe_sequence(&control, 0U);
  assert(SteeringControl_SetCommand(
      &control, 6000, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 10U) == SAFETY_RESULT_OK);
  frame = take_frame(&control, 10U, STEERING_CONTROL_FRAME_ENABLE);
  confirm_frame(&control, &frame, 10U);
  assert(!SteeringControl_GetPendingFrame(&control, 11U, &frame));
  assert(control.speed_command_permille == -1000);
  accept_ack(&control, INDEX_ENABLE, 12U);
  frame = send_speed_and_ack(&control, 12U);
  assert(frame.data[4] == 0xFCU && frame.data[5] == 0x18U);
  assert(frame.data[6] == 0xFFU && frame.data[7] == 0xFFU);
  assert(control.applied_velocity_tdeg_per_s == 4800);

  assert(SteeringControl_SetCommand(
      &control, -6000, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 20U) == SAFETY_RESULT_OK);
  frame = send_speed_and_ack(&control, 20U);
  assert(frame.data[4] == 0x03U && frame.data[5] == 0xE8U);
  assert(frame.data[6] == 0x00U && frame.data[7] == 0x00U);
  assert(control.speed_command_permille == 1000);
  assert(control.applied_velocity_tdeg_per_s == -4800);
}

static void test_transaction_binding_and_heartbeat(void)
{
  SteeringControl control;
  SteeringControlFrame frame;
  uint8_t response[8];
  uint8_t heartbeat[8] = {
    0x12U, 0x34U, 0xFEU, 0x0CU, 0x01U, 0xF4U, 0x00U, 0x00U
  };
  uint32_t response_tick;

  complete_boot_safe_sequence(&control, 0U);
  response_tick = control.last_response_tick_ms;
  make_ack(response, INDEX_ENABLE, 0x00U);
  assert(!SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_SDO_RESPONSE_EXT_ID,
      response, sizeof(response), 10U));
  assert(control.last_response_tick_ms == response_tick);
  assert(control.motor_enable_confirmed == 0U);

  assert(SteeringControl_SetCommand(
      &control, 1200, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 11U) == SAFETY_RESULT_OK);
  frame = take_frame(&control, 11U, STEERING_CONTROL_FRAME_ENABLE);
  confirm_frame(&control, &frame, 11U);
  response_tick = control.last_response_tick_ms;

  assert(SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_HEARTBEAT_EXT_ID,
      heartbeat, sizeof(heartbeat), 12U));
  assert(control.motor_speed_feedback_raw == -500);
  assert(control.heartbeat_speed_given_permille == 500);
  assert(control.last_rx_tick_ms == 12U);
  assert(control.last_response_tick_ms == response_tick);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_ENABLE);
  assert(control.motor_enable_confirmed == 0U);

  make_ack(response, INDEX_SPEED, 0x00U);
  assert(!SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_SDO_RESPONSE_EXT_ID,
      response, sizeof(response), 13U));
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_ENABLE);
  make_ack(response, INDEX_ENABLE, 0x01U);
  assert(!SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_SDO_RESPONSE_EXT_ID,
      response, sizeof(response), 14U));
  assert(control.last_response_tick_ms == response_tick);

  make_ack(response, INDEX_ENABLE, 0x00U);
  assert(SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_SDO_RESPONSE_EXT_ID,
      response, sizeof(response), 15U));
  assert(control.motor_enable_confirmed == 1U);

  make_ack(response, INDEX_SPEED, 0x00U);
  assert(!SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_SDO_RESPONSE_EXT_ID,
      response, sizeof(response), 16U));
  frame = take_frame(&control, 16U, STEERING_CONTROL_FRAME_SPEED);
  confirm_frame(&control, &frame, 16U);
  make_ack(response, INDEX_DISABLE, 0x00U);
  assert(!SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_SDO_RESPONSE_EXT_ID,
      response, sizeof(response), 17U));
  assert(control.motor_enable_confirmed == 1U);
  accept_ack(&control, INDEX_SPEED, 18U);
}

static void test_heartbeat_timeout_and_dtc_recovery(void)
{
  SteeringControl control;
  SteeringControlFrame frame;
  uint8_t heartbeat[8] = {0U, 1U, 0U, 2U, 0U, 3U, 0U, 1U};
  uint32_t response_tick;

  SteeringControl_Init(&control, 0U);
  frame = take_frame(&control, 0U, STEERING_CONTROL_FRAME_SAFE_ZERO);
  confirm_frame(&control, &frame, 0U);
  assert(SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_HEARTBEAT_EXT_ID,
      heartbeat, sizeof(heartbeat), 200U));
  assert(control.response_valid == 0U);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_SAFE_ZERO);
  response_tick = control.last_response_tick_ms;
  SteeringControl_Step(&control, 250U);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_SAFE_ZERO);
  SteeringControl_Step(&control, 251U);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_NONE);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING);
  assert(control.last_response_tick_ms == response_tick);

  complete_boot_safe_sequence(&control, 300U);
  heartbeat[6] = 0U;
  heartbeat[7] = 1U;
  assert(SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_HEARTBEAT_EXT_ID,
      heartbeat, sizeof(heartbeat), 310U));
  assert(control.motor_fault_code == 1U);
  assert((control.fault_flags & SAFETY_STEERING_FAULT_MOTOR_DTC) != 0U);
  assert(control.fault_safe_confirmation_required == 1U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  assert(!SteeringControl_CanClearFault(&control));
  assert(SteeringControl_SetCommand(
      &control, 100, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 311U) == SAFETY_RESULT_NOT_READY);

  heartbeat[7] = 0U;
  assert(SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_HEARTBEAT_EXT_ID,
      heartbeat, sizeof(heartbeat), 312U));
  assert(control.motor_fault_code == 0U);
  assert(!SteeringControl_CanClearFault(&control));
  frame = take_frame(&control, 312U, STEERING_CONTROL_FRAME_SAFE_ZERO);
  confirm_frame(&control, &frame, 312U);
  accept_ack(&control, INDEX_SPEED, 313U);
  frame = take_frame(&control, 313U, STEERING_CONTROL_FRAME_DISABLE);
  confirm_frame(&control, &frame, 313U);
  accept_ack(&control, INDEX_DISABLE, 314U);
  assert(control.fault_safe_confirmation_required == 0U);
  assert(SteeringControl_CanClearFault(&control));
  SteeringControl_ClearFaults(&control);
  assert(control.fault_flags == 0U);
  assert(SteeringControl_SetCommand(
      &control, 100, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 315U) == SAFETY_RESULT_OK);

  assert(!SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_HEARTBEAT_EXT_ID,
      heartbeat, 7U, 316U));
}

static void test_heartbeat_cannot_satisfy_enable_or_speed_watchdog(void)
{
  SteeringControl control;
  SteeringControlFrame frame;
  uint8_t heartbeat[8] = {0U};

  complete_boot_safe_sequence(&control, 0U);
  assert(SteeringControl_SetCommand(
      &control, 100, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 10U) == SAFETY_RESULT_OK);
  frame = take_frame(&control, 10U, STEERING_CONTROL_FRAME_ENABLE);
  confirm_frame(&control, &frame, 10U);
  assert(SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_HEARTBEAT_EXT_ID,
      heartbeat, sizeof(heartbeat), 100U));
  assert(SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_HEARTBEAT_EXT_ID,
      heartbeat, sizeof(heartbeat), 200U));
  SteeringControl_Step(&control, 260U);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_ENABLE);
  SteeringControl_Step(&control, 261U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  assert(control.rearm_required == 1U);
  assert(control.motor_enable_confirmed == 0U);
  assert(SteeringControl_ConsumeActiveFault(&control));

  complete_boot_safe_sequence(&control, 300U);
  enable_and_ack(&control, 100, SAFETY_STEERING_SOURCE_REMOTE, 310U);
  frame = take_frame(&control, 312U, STEERING_CONTROL_FRAME_SPEED);
  confirm_frame(&control, &frame, 312U);
  assert((control.status_flags & SAFETY_STEERING_STATUS_TX_PENDING) != 0U);
  assert(SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_HEARTBEAT_EXT_ID,
      heartbeat, sizeof(heartbeat), 400U));
  assert(SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_HEARTBEAT_EXT_ID,
      heartbeat, sizeof(heartbeat), 500U));
  SteeringControl_Step(&control, 561U);
  assert(control.state == SAFETY_STEERING_STATE_ACTIVE);
  SteeringControl_Step(&control, 562U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  assert(control.rearm_required == 1U);
  assert(control.motor_enable_confirmed == 0U);
  assert(SteeringControl_ConsumeActiveFault(&control));
}

static void test_disable_requires_a_transaction_bound_ack(void)
{
  SteeringControl control;
  SteeringControlFrame frame;
  uint8_t abort_frame[8] = {
    0x80U, 0x0CU, 0x20U, 0x01U, 0x01U, 0x00U, 0x04U, 0x05U
  };
  uint8_t heartbeat[8] = {0U};

  SteeringControl_Init(&control, 0U);
  frame = take_frame(&control, 0U, STEERING_CONTROL_FRAME_SAFE_ZERO);
  confirm_frame(&control, &frame, 0U);
  accept_ack(&control, INDEX_SPEED, 1U);
  frame = take_frame(&control, 1U, STEERING_CONTROL_FRAME_DISABLE);
  confirm_frame(&control, &frame, 1U);
  SteeringControl_Step(&control, 252U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING);
  assert(!SteeringControl_CanClearFault(&control));
  frame = take_frame(&control, 252U, STEERING_CONTROL_FRAME_DISABLE);
  confirm_frame(&control, &frame, 252U);
  accept_ack(&control, INDEX_DISABLE, 253U);
  assert(control.state == SAFETY_STEERING_STATE_DISABLED);
  assert(SteeringControl_CanClearFault(&control));

  SteeringControl_Init(&control, 300U);
  frame = take_frame(&control, 300U, STEERING_CONTROL_FRAME_SAFE_ZERO);
  confirm_frame(&control, &frame, 300U);
  accept_ack(&control, INDEX_SPEED, 301U);
  frame = take_frame(&control, 301U, STEERING_CONTROL_FRAME_DISABLE);
  confirm_frame(&control, &frame, 301U);
  assert(SteeringControl_HandleRxFrame(
      &control, 0x05800001UL, abort_frame, sizeof(abort_frame), 302U));
  assert(control.state == SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING);
  assert(!SteeringControl_CanClearFault(&control));
  frame = take_frame(&control, 302U, STEERING_CONTROL_FRAME_DISABLE);
  confirm_frame(&control, &frame, 302U);
  accept_ack(&control, INDEX_DISABLE, 303U);
  assert(!SteeringControl_CanClearFault(&control));
  assert(SteeringControl_HandleRxFrame(
      &control, 0x07000001UL, heartbeat, sizeof(heartbeat), 304U));
  assert(SteeringControl_CanClearFault(&control));
}

static void test_repeated_disabled_zero_preserves_pending_transactions(void)
{
  SteeringControl control;
  SteeringControlFrame frame;
  uint32_t pending_start;
  uint32_t next_tx;

  complete_boot_safe_sequence(&control, 0U);
  enable_and_ack(&control, 100, SAFETY_STEERING_SOURCE_REMOTE, 10U);
  (void)send_speed_and_ack(&control, 12U);

  assert(SteeringControl_SetCommand(
      &control, 0, 0U, SAFETY_STEERING_SOURCE_REMOTE,
      0U, 100U) == SAFETY_RESULT_OK);
  frame = take_frame(&control, 100U, STEERING_CONTROL_FRAME_SAFE_ZERO);
  confirm_frame(&control, &frame, 100U);
  pending_start = control.pending_response_start_ms;
  next_tx = control.next_tx_tick_ms;
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_SAFE_ZERO);

  /* The network reapplies its selected disabled command every 20 ms. A motor
     ACK delayed beyond that cadence must still complete the original SDO. */
  assert(SteeringControl_SetCommand(
      &control, 0, 0U, SAFETY_STEERING_SOURCE_REMOTE,
      0U, 120U) == SAFETY_RESULT_OK);
  assert(SteeringControl_SetCommand(
      &control, 0, 0U, SAFETY_STEERING_SOURCE_REMOTE,
      0U, 140U) == SAFETY_RESULT_OK);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_SAFE_ZERO);
  assert(control.pending_response_start_ms == pending_start);
  assert(control.next_tx_tick_ms == next_tx);
  assert(!SteeringControl_GetPendingFrame(&control, 140U, &frame));
  accept_ack(&control, INDEX_SPEED, 145U);

  frame = take_frame(&control, 145U, STEERING_CONTROL_FRAME_DISABLE);
  confirm_frame(&control, &frame, 145U);
  pending_start = control.pending_response_start_ms;
  next_tx = control.next_tx_tick_ms;
  assert(SteeringControl_SetCommand(
      &control, 0, 0U, SAFETY_STEERING_SOURCE_REMOTE,
      0U, 165U) == SAFETY_RESULT_OK);
  assert(SteeringControl_SetCommand(
      &control, 0, 0U, SAFETY_STEERING_SOURCE_REMOTE,
      0U, 185U) == SAFETY_RESULT_OK);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_DISABLE);
  assert(control.pending_response_start_ms == pending_start);
  assert(control.next_tx_tick_ms == next_tx);
  accept_ack(&control, INDEX_DISABLE, 190U);
  assert(control.state == SAFETY_STEERING_STATE_DISABLED);

  next_tx = control.safe_refresh_tick_ms;
  assert(SteeringControl_SetCommand(
      &control, 0, 0U, SAFETY_STEERING_SOURCE_REMOTE,
      0U, 210U) == SAFETY_RESULT_OK);
  assert(control.state == SAFETY_STEERING_STATE_DISABLED);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_NONE);
  assert(control.safe_refresh_tick_ms == next_tx);
  assert(!SteeringControl_GetPendingFrame(&control, 210U, &frame));
}

static void test_repeated_safe_request_preserves_pending_transactions(void)
{
  SteeringControl control;
  SteeringControlFrame frame;
  uint32_t pending_start;
  uint32_t next_tx;

  complete_boot_safe_sequence(&control, 0U);
  enable_and_ack(&control, 100, SAFETY_STEERING_SOURCE_REMOTE, 10U);
  (void)send_speed_and_ack(&control, 12U);

  SteeringControl_RequestSafe(&control, 100U);
  frame = take_frame(&control, 100U, STEERING_CONTROL_FRAME_SAFE_ZERO);
  confirm_frame(&control, &frame, 100U);
  pending_start = control.pending_response_start_ms;
  next_tx = control.next_tx_tick_ms;
  SteeringControl_RequestSafe(&control, 101U);
  SteeringControl_RequestSafe(&control, 120U);
  SteeringControl_RequestSafe(&control, 140U);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_SAFE_ZERO);
  assert(control.pending_response_start_ms == pending_start);
  assert(control.next_tx_tick_ms == next_tx);
  accept_ack(&control, INDEX_SPEED, 145U);

  frame = take_frame(&control, 145U, STEERING_CONTROL_FRAME_DISABLE);
  confirm_frame(&control, &frame, 145U);
  pending_start = control.pending_response_start_ms;
  next_tx = control.next_tx_tick_ms;
  SteeringControl_RequestSafe(&control, 146U);
  SteeringControl_RequestSafe(&control, 165U);
  SteeringControl_RequestSafe(&control, 185U);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_DISABLE);
  assert(control.pending_response_start_ms == pending_start);
  assert(control.next_tx_tick_ms == next_tx);
  accept_ack(&control, INDEX_DISABLE, 190U);
  assert(control.state == SAFETY_STEERING_STATE_DISABLED);

  next_tx = control.safe_refresh_tick_ms;
  SteeringControl_RequestSafe(&control, 191U);
  assert(control.state == SAFETY_STEERING_STATE_DISABLED);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_NONE);
  assert(control.safe_refresh_tick_ms == next_tx);
}

static void test_fault_safe_confirmation_is_distinct_and_idempotent(void)
{
  SteeringControl control;
  SteeringControlFrame frame;
  uint32_t pending_start;
  uint32_t next_tx;

  complete_boot_safe_sequence(&control, 0U);
  SteeringControl_RecordTxError(&control, 100U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  assert(control.fault_safe_confirmation_required == 1U);
  frame = take_frame(&control, 105U, STEERING_CONTROL_FRAME_SAFE_ZERO);
  confirm_frame(&control, &frame, 105U);
  pending_start = control.pending_response_start_ms;
  next_tx = control.next_tx_tick_ms;

  SteeringControl_RecordTxError(&control, 106U);
  SteeringControl_RecordTxError(&control, 125U);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_SAFE_ZERO);
  assert(control.pending_response_start_ms == pending_start);
  assert(control.next_tx_tick_ms == next_tx);
  accept_ack(&control, INDEX_SPEED, 140U);

  frame = take_frame(&control, 140U, STEERING_CONTROL_FRAME_DISABLE);
  confirm_frame(&control, &frame, 140U);
  pending_start = control.pending_response_start_ms;
  next_tx = control.next_tx_tick_ms;
  SteeringControl_RecordTxError(&control, 141U);
  SteeringControl_RecordTxError(&control, 160U);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_DISABLE);
  assert(control.pending_response_start_ms == pending_start);
  assert(control.next_tx_tick_ms == next_tx);
  assert(!SteeringControl_CanClearFault(&control));
  accept_ack(&control, INDEX_DISABLE, 180U);
  assert(control.state == SAFETY_STEERING_STATE_DISABLED);
  assert(control.fault_safe_confirmation_required == 0U);
  assert(SteeringControl_CanClearFault(&control));

  /* A later fault report is a new event and requires a new proof even though
     the previously acknowledged state was disabled. */
  SteeringControl_RecordTxError(&control, 200U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  assert(control.fault_safe_confirmation_required == 1U);
  assert(!SteeringControl_CanClearFault(&control));
}

static void test_safe_zero_timeout_cannot_fall_through_to_enable(void)
{
  SteeringControl control;
  SteeringControlFrame frame;

  SteeringControl_Init(&control, 0U);
  frame = take_frame(&control, 0U, STEERING_CONTROL_FRAME_SAFE_ZERO);
  confirm_frame(&control, &frame, 0U);
  assert(SteeringControl_SetCommand(
      &control, 100, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 1U) == SAFETY_RESULT_OK);
  assert(control.enable_request == 1U);
  SteeringControl_Step(&control, 251U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING);
  assert(control.enable_request == 0U);
  assert(control.rearm_required == 1U);
  frame = take_frame(&control, 251U, STEERING_CONTROL_FRAME_DISABLE);
  confirm_frame(&control, &frame, 251U);
  accept_ack(&control, INDEX_DISABLE, 252U);
  assert(control.state == SAFETY_STEERING_STATE_DISABLED);
  assert(control.motor_enable_confirmed == 0U);
  assert(!SteeringControl_GetPendingFrame(&control, 252U, &frame));

  /* The same interlock applies to the periodic disabled refresh transaction,
     not only to the initial boot transaction. */
  complete_boot_safe_sequence(&control, 1000U);
  SteeringControl_Step(&control, 1252U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  frame = take_frame(&control, 1252U, STEERING_CONTROL_FRAME_SAFE_ZERO);
  confirm_frame(&control, &frame, 1252U);
  assert(SteeringControl_SetCommand(
      &control, 100, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 1253U) == SAFETY_RESULT_OK);
  SteeringControl_Step(&control, 1503U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING);
  assert(control.enable_request == 0U);
  assert(control.rearm_required == 1U);
  frame = take_frame(&control, 1503U, STEERING_CONTROL_FRAME_DISABLE);
  confirm_frame(&control, &frame, 1503U);
  accept_ack(&control, INDEX_DISABLE, 1504U);
  assert(control.state == SAFETY_STEERING_STATE_DISABLED);
  assert(control.motor_enable_confirmed == 0U);
  assert(!SteeringControl_GetPendingFrame(&control, 1504U, &frame));
}

static void test_manual_limit_rearm_and_source_switch(void)
{
  SteeringControl control;

  complete_boot_safe_sequence(&control, 0U);
  enable_and_ack(&control, 0, SAFETY_STEERING_SOURCE_REMOTE, 10U);
  (void)send_speed_and_ack(&control, 12U);
  assert(control.manual_nonzero_active == 0U);
  SteeringControl_Step(&control, 3000U);
  assert(control.state == SAFETY_STEERING_STATE_ACTIVE);
  assert(SteeringControl_SetCommand(
      &control, 1200, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 3001U) == SAFETY_RESULT_OK);
  assert(control.manual_nonzero_active == 1U);
  assert(control.manual_nonzero_start_ms == 3001U);
  assert(SteeringControl_SetCommand(
      &control, 0, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 3100U) == SAFETY_RESULT_OK);
  assert(control.manual_nonzero_active == 0U);
  assert(SteeringControl_SetCommand(
      &control, 1200, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 6000U) == SAFETY_RESULT_OK);
  assert(control.manual_nonzero_active == 1U);
  assert(control.manual_nonzero_start_ms == 6000U);

  complete_boot_safe_sequence(&control, 0U);
  enable_and_ack(&control, 1200, SAFETY_STEERING_SOURCE_REMOTE, 10U);
  (void)send_speed_and_ack(&control, 12U);
  assert(SteeringControl_SetCommand(
      &control, 1200, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 2008U) == SAFETY_RESULT_OK);
  (void)send_speed_and_ack(&control, 2008U);
  SteeringControl_Step(&control, 2009U);
  assert(control.state == SAFETY_STEERING_STATE_ACTIVE);
  SteeringControl_Step(&control, 2010U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  assert(control.rearm_required == 1U);
  assert((control.status_flags & SAFETY_STEERING_STATUS_MANUAL_LIMIT) != 0U);
  assert(SteeringControl_SetCommand(
      &control, 0, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 2011U) == SAFETY_RESULT_RATE_LIMITED);
  assert(SteeringControl_SetCommand(
      &control, 0, 0U, SAFETY_STEERING_SOURCE_REMOTE,
      0U, 2012U) == SAFETY_RESULT_OK);
  assert(control.rearm_required == 0U);

  assert(SteeringControl_SetCommand(
      &control, 1200, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 2013U) == SAFETY_RESULT_OK);
  assert(SteeringControl_SetCommand(
      &control, 1200, 1U, SAFETY_STEERING_SOURCE_OPERATOR,
      SAFETY_STEERING_FLAG_RATE_MODE, 2014U) == SAFETY_RESULT_RATE_LIMITED);
  assert(control.rearm_required == 1U);
  assert((control.status_flags & SAFETY_STEERING_STATUS_SOURCE_SWITCH) != 0U);
  assert(SteeringControl_SetCommand(
      &control, 0, 1U, SAFETY_STEERING_SOURCE_OPERATOR,
      SAFETY_STEERING_FLAG_RATE_MODE, 2015U) == SAFETY_RESULT_RATE_LIMITED);
  assert(SteeringControl_SetCommand(
      &control, 0, 0U, SAFETY_STEERING_SOURCE_OPERATOR,
      0U, 2016U) == SAFETY_RESULT_OK);

  complete_boot_safe_sequence(&control, 3000U);
  enable_and_ack(&control, 1200,
                 SAFETY_STEERING_SOURCE_AUTONOMOUS, 3010U);
  (void)send_speed_and_ack(&control, 3012U);
  assert(control.manual_nonzero_active == 0U);
  assert(SteeringControl_SetCommand(
      &control, 1200, 1U, SAFETY_STEERING_SOURCE_AUTONOMOUS,
      SAFETY_STEERING_FLAG_RATE_MODE, 6000U) == SAFETY_RESULT_OK);
  (void)send_speed_and_ack(&control, 6000U);
  SteeringControl_Step(&control, 6002U);
  assert(control.state == SAFETY_STEERING_STATE_ACTIVE);
}

static void test_abort_bus_off_and_tx_deferred(void)
{
  SteeringControl control;
  SteeringControlFrame frame;
  uint8_t abort_frame[8] = {
    0x80U, 0x0DU, 0x20U, 0x01U, 0x78U, 0x56U, 0x34U, 0x12U
  };
  uint32_t rx_errors;

  complete_boot_safe_sequence(&control, 0U);
  assert(SteeringControl_SetCommand(
      &control, 100, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 10U) == SAFETY_RESULT_OK);
  frame = take_frame(&control, 10U, STEERING_CONTROL_FRAME_ENABLE);
  confirm_frame(&control, &frame, 10U);
  rx_errors = control.rx_errors;
  assert(SteeringControl_HandleRxFrame(
      &control, STEERING_CONTROL_SDO_RESPONSE_EXT_ID,
      abort_frame, sizeof(abort_frame), 11U));
  assert(control.rx_errors == rx_errors + 1U);
  assert(control.motor_abort_code == 0x12345678UL);
  assert(control.motor_fault_code == UINT16_MAX);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  assert(SteeringControl_ConsumeActiveFault(&control));
  assert(!SteeringControl_ConsumeActiveFault(&control));

  SteeringControl_Init(&control, 0U);
  assert(SteeringControl_GetPendingFrame(&control, 0U, &frame));
  SteeringControl_RecordTxDeferred(&control, 0U);
  assert(control.tx_deferred == 1U);
  assert(control.tx_errors == 0U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  assert(!SteeringControl_GetPendingFrame(&control, 0U, &frame));
  assert(SteeringControl_GetPendingFrame(&control, 1U, &frame));

  complete_boot_safe_sequence(&control, 100U);
  SteeringControl_RecordBusState(&control, SAFETY_STEERING_BUS_OFF,
                                 255U, 0U, 110U);
  assert(control.bus_off_events == 1U);
  assert(!SteeringControl_ConsumeActiveFault(&control));
  assert(SteeringControl_SetCommand(
      &control, 100, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 111U) == SAFETY_RESULT_NOT_READY);

  complete_boot_safe_sequence(&control, 200U);
  enable_and_ack(&control, 100, SAFETY_STEERING_SOURCE_REMOTE, 210U);
  SteeringControl_RecordBusState(&control, SAFETY_STEERING_BUS_OFF,
                                 255U, 0U, 212U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  assert(control.motor_enable_confirmed == 0U);
  assert(SteeringControl_ConsumeActiveFault(&control));
}

static void test_latched_local_faults_require_safe_clear_and_rearm(void)
{
  SteeringControl control;
  uint32_t rx_errors;

  complete_boot_safe_sequence(&control, 0U);
  SteeringControl_RecordTxError(&control, 3U);
  assert((control.fault_flags & SAFETY_STEERING_FAULT_TX) != 0U);
  assert(!SteeringControl_ConsumeActiveFault(&control));
  assert(SteeringControl_SetCommand(
      &control, 100, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 4U) == SAFETY_RESULT_NOT_READY);
  /* Failed FDCAN submissions use the bounded 5 ms retry backoff. */
  complete_pending_safe_sequence(&control, 8U);
  assert(SteeringControl_CanClearFault(&control));
  SteeringControl_ClearFaults(&control);
  assert(control.fault_flags == 0U);

  complete_boot_safe_sequence(&control, 100U);
  enable_and_ack(&control, 100, SAFETY_STEERING_SOURCE_REMOTE, 110U);
  (void)send_speed_and_ack(&control, 112U);
  SteeringControl_RecordTxError(&control, 120U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  assert(control.rearm_required == 1U);
  assert(SteeringControl_ConsumeActiveFault(&control));
  complete_pending_safe_sequence(&control, 125U);
  assert(SteeringControl_CanClearFault(&control));
  SteeringControl_ClearFaults(&control);
  assert(SteeringControl_SetCommand(
      &control, 100, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 128U) == SAFETY_RESULT_RATE_LIMITED);
  assert(SteeringControl_SetCommand(
      &control, 0, 0U, SAFETY_STEERING_SOURCE_REMOTE,
      0U, 129U) == SAFETY_RESULT_OK);
  complete_pending_safe_sequence(&control, 129U);
  assert(SteeringControl_SetCommand(
      &control, 100, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, 132U) == SAFETY_RESULT_OK);

  complete_boot_safe_sequence(&control, 200U);
  enable_and_ack(&control, 100, SAFETY_STEERING_SOURCE_REMOTE, 210U);
  (void)send_speed_and_ack(&control, 212U);
  rx_errors = control.rx_errors;
  SteeringControl_RecordProtocolError(
      &control, SAFETY_STEERING_FAULT_PROTOCOL, 220U);
  assert(control.rx_errors == rx_errors + 1U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  assert(control.rearm_required == 1U);
  assert(SteeringControl_ConsumeActiveFault(&control));
}

static void test_wrap_safe_deadlines_and_manual_limit(void)
{
  SteeringControl control;
  SteeringControlFrame frame;
  uint32_t start = UINT32_MAX - 10U;
  uint32_t manual_start = UINT32_MAX - 50U;
  uint32_t refresh;

  SteeringControl_Init(&control, start);
  frame = take_frame(&control, start, STEERING_CONTROL_FRAME_SAFE_ZERO);
  confirm_frame(&control, &frame, start);
  SteeringControl_Step(&control, start + 250U);
  assert(control.pending_response == STEERING_CONTROL_RESPONSE_SAFE_ZERO);
  SteeringControl_Step(&control, start + 251U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_DISABLE_PENDING);

  SteeringControl_Init(&control, UINT32_MAX);
  assert(SteeringControl_GetPendingFrame(&control, UINT32_MAX, &frame));
  SteeringControl_RecordTxDeferred(&control, UINT32_MAX);
  assert(!SteeringControl_GetPendingFrame(&control, UINT32_MAX, &frame));
  assert(SteeringControl_GetPendingFrame(&control, 0U, &frame));

  complete_boot_safe_sequence(&control, UINT32_MAX - 100U);
  enable_and_ack(&control, 100, SAFETY_STEERING_SOURCE_REMOTE,
                 manual_start);
  (void)send_speed_and_ack(&control, manual_start + 2U);
  refresh = manual_start + 1998U;
  assert(SteeringControl_SetCommand(
      &control, 100, 1U, SAFETY_STEERING_SOURCE_REMOTE,
      SAFETY_STEERING_FLAG_RATE_MODE, refresh) == SAFETY_RESULT_OK);
  (void)send_speed_and_ack(&control, refresh);
  SteeringControl_Step(&control, manual_start + 1999U);
  assert(control.state == SAFETY_STEERING_STATE_ACTIVE);
  SteeringControl_Step(&control, manual_start + 2000U);
  assert(control.state == SAFETY_STEERING_STATE_SAFE_ZERO_PENDING);
  assert(control.rearm_required == 1U);
}

int main(void)
{
  test_command_validation();
  test_speed_permille_conversion_and_ack_gate();
  test_transaction_binding_and_heartbeat();
  test_heartbeat_timeout_and_dtc_recovery();
  test_heartbeat_cannot_satisfy_enable_or_speed_watchdog();
  test_safe_zero_timeout_cannot_fall_through_to_enable();
  test_disable_requires_a_transaction_bound_ack();
  test_repeated_disabled_zero_preserves_pending_transactions();
  test_repeated_safe_request_preserves_pending_transactions();
  test_fault_safe_confirmation_is_distinct_and_idempotent();
  test_manual_limit_rearm_and_source_switch();
  test_abort_bus_off_and_tx_deferred();
  test_latched_local_faults_require_safe_clear_and_rearm();
  test_wrap_safe_deadlines_and_manual_limit();
  return 0;
}
