#ifndef STEERING_CONTROL_H
#define STEERING_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "safety_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STEERING_CONTROL_TX_EXT_ID             0x06000001UL
#define STEERING_CONTROL_SDO_RESPONSE_EXT_ID   0x05800001UL
#define STEERING_CONTROL_HEARTBEAT_EXT_ID      0x07000001UL
#define STEERING_CONTROL_TX_PERIOD_MS          50UL
#define STEERING_CONTROL_SAFE_REFRESH_MS       250UL
#define STEERING_CONTROL_RESPONSE_TIMEOUT_MS   250UL
#define STEERING_MOTOR_RATED_SPEED_RPM          80

typedef enum
{
  STEERING_CONTROL_FRAME_NONE = 0,
  STEERING_CONTROL_FRAME_SAFE_ZERO,
  STEERING_CONTROL_FRAME_DISABLE,
  STEERING_CONTROL_FRAME_ENABLE,
  STEERING_CONTROL_FRAME_SPEED
} SteeringControlFrameKind;

typedef enum
{
  STEERING_CONTROL_RESPONSE_NONE = 0,
  STEERING_CONTROL_RESPONSE_SAFE_ZERO,
  STEERING_CONTROL_RESPONSE_DISABLE,
  STEERING_CONTROL_RESPONSE_ENABLE,
  STEERING_CONTROL_RESPONSE_SPEED
} SteeringControlPendingResponse;

typedef struct
{
  SteeringControlFrameKind kind;
  uint32_t extended_id;
  uint8_t length;
  uint8_t data[8];
} SteeringControlFrame;

typedef struct
{
  int16_t requested_velocity_tdeg_per_s;
  int16_t applied_velocity_tdeg_per_s;
  int16_t speed_command_permille;
  int16_t last_transmitted_speed_command_permille;
  int16_t last_transmitted_applied_velocity_tdeg_per_s;
  int16_t motor_speed_feedback_raw;
  int16_t heartbeat_speed_given_permille;
  uint16_t motor_fault_code;
  uint8_t source;
  uint8_t state;
  uint8_t bus_state;
  uint8_t enable_request;
  uint8_t motor_enable_confirmed;
  uint8_t owner_valid;
  uint8_t manual_nonzero_active;
  uint8_t rearm_required;
  uint8_t pending_response;
  uint32_t command_tick_ms;
  uint32_t last_rx_tick_ms;
  uint32_t last_response_tick_ms;
  uint32_t manual_nonzero_start_ms;
  uint32_t pending_response_start_ms;
  uint32_t next_tx_tick_ms;
  uint32_t safe_refresh_tick_ms;
  uint32_t status_flags;
  uint32_t fault_flags;
  uint32_t motor_abort_code;
  uint32_t tx_frames;
  uint32_t rx_frames;
  uint32_t tx_errors;
  uint32_t rx_errors;
  uint32_t tx_deferred;
  uint32_t bus_off_events;
  uint8_t rx_valid;
  uint8_t response_valid;
  uint8_t active_fault_pending;
  uint8_t fault_safe_confirmation_required;
} SteeringControl;

void SteeringControl_Init(SteeringControl *control, uint32_t now_ms);
bool SteeringControl_CommandValuesValid(int16_t velocity_tdeg_per_s,
                                        uint8_t enable, uint8_t flags,
                                        uint8_t run_permit_on);
int32_t SteeringControl_SetCommand(SteeringControl *control,
                                   int16_t velocity_tdeg_per_s,
                                   uint8_t enable, uint8_t source,
                                   uint8_t flags, uint32_t now_ms);
void SteeringControl_RequestSafe(SteeringControl *control, uint32_t now_ms);
void SteeringControl_RecordCommandRejected(SteeringControl *control,
                                           uint32_t now_ms);
void SteeringControl_Step(SteeringControl *control, uint32_t now_ms);
bool SteeringControl_GetPendingFrame(SteeringControl *control,
                                     uint32_t now_ms,
                                     SteeringControlFrame *frame);
void SteeringControl_ConfirmTransmit(SteeringControl *control,
                                     SteeringControlFrameKind kind,
                                     uint32_t now_ms);
void SteeringControl_RecordTxDeferred(SteeringControl *control,
                                      uint32_t now_ms);
void SteeringControl_RecordTxError(SteeringControl *control,
                                   uint32_t now_ms);
void SteeringControl_RecordBusState(SteeringControl *control,
                                    uint8_t bus_state,
                                    uint32_t tx_error_count,
                                    uint32_t rx_error_count,
                                    uint32_t now_ms);
void SteeringControl_RecordProtocolError(SteeringControl *control,
                                         uint32_t fault_flags,
                                         uint32_t now_ms);
bool SteeringControl_HandleRxFrame(SteeringControl *control,
                                   uint32_t extended_id,
                                   const uint8_t *data, uint8_t length,
                                   uint32_t now_ms);
bool SteeringControl_ConsumeActiveFault(SteeringControl *control);
bool SteeringControl_CanClearFault(const SteeringControl *control);
void SteeringControl_ClearFaults(SteeringControl *control);

#ifdef __cplusplus
}
#endif

#endif /* STEERING_CONTROL_H */
