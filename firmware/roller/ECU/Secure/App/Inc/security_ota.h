#ifndef SECURITY_OTA_H
#define SECURITY_OTA_H

#include <stdint.h>

#include "safety_api.h"

void SecurityOta_Init(void);
int32_t SecurityOta_GetStatus(SAFETY_OtaStatus *status);
int32_t SecurityOta_Begin(const SAFETY_OtaBeginRequest *request,
                          SAFETY_OtaStatus *status);
int32_t SecurityOta_Write(const SAFETY_OtaChunk *chunk,
                          SAFETY_OtaStatus *status);
int32_t SecurityOta_Finish(uint32_t update_sequence,
                           SAFETY_OtaStatus *status);
int32_t SecurityOta_ConfirmRunningImages(void);

#endif /* SECURITY_OTA_H */
