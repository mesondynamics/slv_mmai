#ifndef SECURITY_OTA_POLICY_H
#define SECURITY_OTA_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* MCUboot image_ok is erased (0xFF) until the running test image is
   confirmed, then programmed to 0x01. Any other value is trailer corruption
   and must fail closed. */
bool SecurityOta_EvaluateConfirmationFlags(uint8_t secure_value,
                                           uint8_t nonsecure_value,
                                           uint32_t *confirmed);

#endif /* SECURITY_OTA_POLICY_H */
