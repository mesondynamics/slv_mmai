#ifndef SECURITY_MCU_IDENTITY_H
#define SECURITY_MCU_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the STM32 manufacturing identity authenticated by the current boot
 * path. OEMiROT builds consume the sealed per-device boot handoff; development
 * builds read the engineering-information registers directly. Runtime board
 * binding also requires the protected ATECC pairing record and challenge. */
bool SecurityMcuIdentity_Get(uint32_t mcu_uid[3]);

#ifdef __cplusplus
}
#endif

#endif /* SECURITY_MCU_IDENTITY_H */
