#ifndef ECU_APP_H
#define ECU_APP_H

#include <stdint.h>
#include "safety_api.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t ECU_AppInit(void);
void ECU_AppProcess(void);
const SAFETY_AdcSnapshot *ECU_AppGetSafetySnapshot(void);

#ifdef __cplusplus
}
#endif

#endif /* ECU_APP_H */
