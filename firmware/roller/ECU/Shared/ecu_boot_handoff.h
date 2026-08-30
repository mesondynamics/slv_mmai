#ifndef ECU_BOOT_HANDOFF_H
#define ECU_BOOT_HANDOFF_H

#include <stdint.h>

/*
 * The STM32H563 engineering-information aperture is not a reliable CPU source
 * at OEMiROT HDPL1: access can return zero or fault according to the inherited
 * SAU/cache state.  The per-device boot image therefore publishes its reviewed
 * manufacturing identity in a sealed Secure-SRAM record.  Anti-transplant
 * enforcement additionally requires the CLOSED/HDP-protected pairing record
 * and the ATECC608C non-exportable private key; the UID words are not treated
 * as a secret or as the sole binding factor.  This address is excluded from
 * the Secure application's stack and linker allocations.
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
