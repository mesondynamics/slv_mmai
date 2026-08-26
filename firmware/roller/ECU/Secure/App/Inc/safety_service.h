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
int32_t Safety_ClearFault(uint32_t request_token);
int32_t Safety_ArmOutputs(uint32_t request_token);
int32_t Safety_DisarmOutputs(void);
int32_t Safety_SetPwm(uint16_t forward_compare,
                      uint16_t reverse_compare,
                      uint32_t command_sequence);
int32_t Safety_KickWatchdog(uint32_t heartbeat);
void Safety_FaultFromException(void);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_SERVICE_H */
