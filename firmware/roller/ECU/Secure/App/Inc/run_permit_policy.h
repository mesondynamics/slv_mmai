#ifndef RUN_PERMIT_POLICY_H
#define RUN_PERMIT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "safety_api.h"

/* Faults in this mask indicate that the Secure actuator boundary itself can no
   longer be trusted.  Isolated valve/CAN faults and a retained physical-E-stop
   fault are deliberately absent: they inhibit automated outputs, but must not
   strand a manually driven vehicle after the physical E-stop is released. */
#define RUN_PERMIT_CRITICAL_STATUS_MASK                                  \
  (SAFETY_STATUS_GTZC_VIOLATION | SAFETY_STATUS_INTERNAL_ERROR |         \
   SAFETY_STATUS_TPIC_ERROR | SAFETY_STATUS_SW_I2C_BUS_FAULT |           \
   SAFETY_STATUS_ATECC_MISSING | SAFETY_STATUS_ATECC_UNPAIRED |          \
   SAFETY_STATUS_ATECC_AUTH_FAILED | SAFETY_STATUS_OTA_ACTIVE |          \
   SAFETY_STATUS_OTA_READY | SAFETY_STATUS_OTA_UNCONFIRMED)

bool RunPermitPolicy_CanClose(uint32_t safety_status,
                              uint32_t security_flags,
                              bool physical_estop_active,
                              bool running_images_confirmed);

#endif /* RUN_PERMIT_POLICY_H */
