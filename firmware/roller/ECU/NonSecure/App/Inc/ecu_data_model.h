#ifndef ECU_DATA_MODEL_H
#define ECU_DATA_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "ecu_protocol.h"
#include "safety_api.h"

typedef enum
{
  ECU_CONTROL_IDLE = 0,
  ECU_CONTROL_REMOTE = 1,
  ECU_CONTROL_OPERATOR = 2,
  ECU_CONTROL_AUTONOMOUS = 3,
  ECU_CONTROL_EMERGENCY = 4,
  ECU_CONTROL_PHYSICAL_ESTOP = 5
} ECU_ControlMode;

bool ECU_DataModelInit(void);
int32_t ECU_DataModelApplyControl(const ECU_ControlPayloadV2 *control,
                                  uint32_t secure_sequence);
void ECU_DataModelControlLost(void);
void ECU_DataModelSetAuthority(ECU_ControlMode mode, uint8_t sender_id);
void ECU_DataModelUpdateStatus(void);
const ECU_StatusPayloadV2 *ECU_DataModelGetStatus(void);
const ECU_ControlPayloadV2 *ECU_DataModelGetControl(void);
const SAFETY_AdcSnapshot *ECU_DataModelGetAdcSnapshot(void);
const SAFETY_ActuatorSnapshot *ECU_DataModelGetActuatorSnapshot(void);
bool ECU_DataModelControlIsNeutral(const ECU_ControlPayloadV2 *control);

#endif /* ECU_DATA_MODEL_H */
