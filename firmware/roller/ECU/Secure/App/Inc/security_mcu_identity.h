#ifndef SECURITY_MCU_IDENTITY_H
#define SECURITY_MCU_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the STM32 UID authenticated by the current boot path.  OEMiROT
 * builds consume the sealed early-boot SRAM handoff; development builds read
 * the engineering-information registers directly. */
bool SecurityMcuIdentity_Get(uint32_t mcu_uid[3]);

#ifdef __cplusplus
}
#endif

#endif /* SECURITY_MCU_IDENTITY_H */
