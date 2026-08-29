#ifndef ECU_FLASH_LAYOUT_H
#define ECU_FLASH_LAYOUT_H

/*
 * Canonical STM32H563 2 MiB flash layout.
 *
 * Keep this file free of C-only constructs: it is consumed by the OEMiROT
 * linker-script preprocessor as well as by firmware and host-side audits.
 * All offsets are relative to the non-secure physical flash base 0x08000000.
 */
#define ECU_FLASH_BASE_NS                    0x08000000
#define ECU_FLASH_BASE_S                     0x0C000000
#define ECU_FLASH_TOTAL_SIZE                 0x00200000
#define ECU_FLASH_BANK_SIZE                  0x00100000
#define ECU_FLASH_SECTOR_SIZE                0x00002000
#define ECU_FLASH_PROGRAM_UNIT               0x00000010

#define ECU_BOOT_OFFSET                      0x00000000
#define ECU_BOOT_SIZE                        0x00020000
#define ECU_SCRATCH_OFFSET                   0x00020000
#define ECU_SCRATCH_SIZE                     0x00010000

#define ECU_SECURE_PRIMARY_OFFSET            0x00030000
#define ECU_SECURE_PRIMARY_SIZE              0x00030000
#define ECU_SECURE_SECONDARY_OFFSET          0x00060000
#define ECU_SECURE_SECONDARY_SIZE            0x00030000

#define ECU_NONSECURE_SECONDARY_OFFSET       0x00090000
#define ECU_NONSECURE_SECONDARY_SIZE         0x00050000

/* Secure persistent storage is never part of an MCUboot flash area. */
#define ECU_PERSISTENT_OFFSET                0x000E0000
#define ECU_PERSISTENT_SIZE                  0x00020000
#define ECU_SECURITY_STORE_OFFSET            0x000E0000
#define ECU_SECURITY_STORE_SIZE              0x00008000
#define ECU_OTA_JOURNAL_OFFSET               0x000E8000
#define ECU_OTA_JOURNAL_SIZE                 0x00010000
#define ECU_VALVE_CONFIG_OFFSET              0x000FA000
#define ECU_VALVE_CONFIG_SIZE                0x00004000

#define ECU_NONSECURE_PRIMARY_OFFSET         0x00100000
#define ECU_NONSECURE_PRIMARY_SIZE           0x00050000

#define ECU_MCUBOOT_HEADER_SIZE              0x00000400
#define ECU_MCUBOOT_TRAILER_SIZE             0x00002000
#define ECU_CMSE_VENEER_OFFSET               0x0005DC00
#define ECU_CMSE_VENEER_SIZE                 0x00000400

#define ECU_SECURE_VECTOR_ADDRESS            0x0C030400
#define ECU_NONSECURE_VECTOR_ADDRESS         0x08100400

#define ECU_LAYOUT_VERSION                   0x00010000

#endif /* ECU_FLASH_LAYOUT_H */
