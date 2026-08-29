#ifndef ECU_BOOT_HANDOFF_H
#define ECU_BOOT_HANDOFF_H

#include <stdint.h>

/*
 * OEMiROT reads the immutable STM32 identity before RSS enters HDPL3.  The
 * engineering-information aperture returns zero to the application at HDPL3,
 * so the boot stage publishes a sealed record in Secure SRAM1.  This address
 * is excluded from the Secure application's stack and linker allocations.
 */
#define ECU_BOOT_HANDOFF_ADDRESS  0x3003FBC0UL
#define ECU_BOOT_HANDOFF_MAGIC    0x44495545UL /* "EUID" */
#define ECU_BOOT_HANDOFF_SCHEMA   1UL
#define ECU_BOOT_HANDOFF_SIZE     64UL

typedef struct
{
  uint32_t magic;
  uint32_t magic_inv;
  uint32_t schema;
  uint32_t schema_inv;
  uint32_t record_size;
  uint32_t record_size_inv;
  uint32_t idcode;
  uint32_t idcode_inv;
  uint32_t mcu_uid[3];
  uint32_t mcu_uid_inv[3];
  uint32_t crc32c;
  uint32_t crc32c_inv;
} ECU_BootHandoff;

#if defined(__cplusplus)
static_assert(sizeof(ECU_BootHandoff) == ECU_BOOT_HANDOFF_SIZE,
              "OEMiROT UID handoff ABI changed");
#else
_Static_assert(sizeof(ECU_BootHandoff) == ECU_BOOT_HANDOFF_SIZE,
               "OEMiROT UID handoff ABI changed");
#endif

#endif /* ECU_BOOT_HANDOFF_H */
