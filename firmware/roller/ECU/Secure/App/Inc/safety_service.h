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
int32_t Safety_GetSteeringSnapshot(SAFETY_SteeringSnapshot *snapshot);
int32_t Safety_GetJ1939Snapshot(SAFETY_J1939Snapshot *snapshot);
int32_t Safety_GetSecurityStatus(SAFETY_SecurityStatus *status);
int32_t Safety_OtaGetStatus(SAFETY_OtaStatus *status);
int32_t Safety_OtaBegin(const SAFETY_OtaBeginRequest *request,
                        SAFETY_OtaStatus *status);
int32_t Safety_OtaWrite(const SAFETY_OtaChunk *chunk,
                        SAFETY_OtaStatus *status);
int32_t Safety_OtaFinish(uint32_t update_sequence,
                         SAFETY_OtaStatus *status);
int32_t Safety_OtaConfirmRunningImages(void);
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
#if defined(ECU_FACTORY_PROVISIONING)
int32_t Safety_FactoryGetStatus(SAFETY_FactoryStatus *status);
int32_t Safety_FactoryProvision(
    const SAFETY_FactoryProvisionRequest *request,
    SAFETY_FactoryStatus *status);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_SERVICE_H */
