#ifndef SAFETY_SERVICE_H
#define SAFETY_SERVICE_H

#include <stdint.h>
#include "safety_api.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t Safety_ServiceInit(void);
uint32_t Safety_GetStatus(void);
int32_t Safety_GetAdcSnapshot(SAFETY_AdcSnapshot *snapshot);
int32_t Safety_GetActuatorSnapshot(SAFETY_ActuatorSnapshot *snapshot);
int32_t Safety_GetValveConfig(SAFETY_ValveConfigSnapshot *snapshot);
int32_t Safety_ApplyValveConfig(const SAFETY_ValveConfig *config);
int32_t Safety_SaveValveConfig(void);
int32_t Safety_ReloadValveConfig(void);
int32_t Safety_ReadValveTelemetry(SAFETY_ValveTelemetryBatch *batch);
int32_t Safety_ClearFault(uint32_t request_token);
int32_t Safety_ArmOutputs(uint32_t request_token);
int32_t Safety_DisarmOutputs(void);
int32_t Safety_SubmitActuatorCommand(const SAFETY_ActuatorCommand *command);
int32_t Safety_KickWatchdog(uint32_t heartbeat);
void Safety_FaultFromException(void);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_SERVICE_H */
