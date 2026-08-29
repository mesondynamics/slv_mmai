/* Product-specific derivative of STM32CubeH5 v1.7.0 OEMiROT region_defs.h. */
#ifndef __REGION_DEFS_H__
#define __REGION_DEFS_H__

#include "flash_layout.h"

#define BL2_HEAP_SIZE                       0x00000000
#define BL2_MSP_STACK_SIZE                  0x00002000
#define GTZC_RAM_ALIGN                      512
#define GTZC_FLASH_ALIGN                    8192

#define _SRAM1_SIZE_MAX                     0x00040000
#define _SRAM2_SIZE_MAX                     0x00010000
#define _SRAM3_SIZE_MAX                     0x00050000
#define _FLASH_BASE_NS                      ECU_FLASH_BASE_NS
#define _SRAM1_BASE_NS                      0x20000000
#define _SRAM2_BASE_NS                      0x20040000
#define _SRAM3_BASE_NS                      0x20050000
#define _FLASH_BASE_S                       ECU_FLASH_BASE_S
#define _SRAM1_BASE_S                       0x30000000
#define _SRAM2_BASE_S                       0x30040000
#define _SRAM3_BASE_S                       0x30050000

#define TOTAL_ROM_SIZE                      FLASH_TOTAL_SIZE
#define S_TOTAL_RAM_SIZE                    _SRAM2_SIZE_MAX
#define BOOT_SHARED_DATA_SIZE               0
#define BOOT_SHARED_DATA_BASE               0
#define BL2_HEADER_SIZE                     ECU_MCUBOOT_HEADER_SIZE
#define BL2_DATA_HEADER_SIZE                0x20
#define BL2_TRAILER_SIZE                    ECU_MCUBOOT_TRAILER_SIZE

#ifdef BL2
#define S_IMAGE_PRIMARY_PARTITION_OFFSET    FLASH_AREA_0_OFFSET
#define S_IMAGE_SECONDARY_PARTITION_OFFSET  FLASH_AREA_2_OFFSET
#define NS_IMAGE_PRIMARY_PARTITION_OFFSET   FLASH_AREA_1_OFFSET
#define NS_IMAGE_SECONDARY_PARTITION_OFFSET FLASH_AREA_3_OFFSET
#else
#error "Config without BL2 is unsupported"
#endif

#define S_ROM_ALIAS_BASE                    _FLASH_BASE_S
#define NS_ROM_ALIAS_BASE                   _FLASH_BASE_NS
#define S_RAM_ALIAS_BASE                    _SRAM1_BASE_S
#define NS_RAM_ALIAS_BASE                   _SRAM1_BASE_NS
#define S_ROM_ALIAS(x)                      (S_ROM_ALIAS_BASE + (x))
#define NS_ROM_ALIAS(x)                     (NS_ROM_ALIAS_BASE + (x))
#define S_RAM_ALIAS(x)                      (S_RAM_ALIAS_BASE + (x))
#define NS_RAM_ALIAS(x)                     (NS_RAM_ALIAS_BASE + (x))

#define S_IMAGE_PRIMARY_AREA_OFFSET         (S_IMAGE_PRIMARY_PARTITION_OFFSET + BL2_HEADER_SIZE)
#define S_CODE_START                        S_ROM_ALIAS(S_IMAGE_PRIMARY_AREA_OFFSET)
#define NS_IMAGE_PRIMARY_AREA_OFFSET        (NS_IMAGE_PRIMARY_PARTITION_OFFSET + BL2_HEADER_SIZE)
#define NS_CODE_START                       NS_ROM_ALIAS(NS_IMAGE_PRIMARY_AREA_OFFSET)

#define BL2_CODE_START                      S_ROM_ALIAS(FLASH_AREA_BL2_OFFSET)
#define BL2_CODE_SIZE                       FLASH_AREA_BL2_SIZE
#define BL2_CODE_LIMIT                      (BL2_CODE_START + BL2_CODE_SIZE - 1)
#define BL2_BOOT_VTOR_ADDR                  BL2_CODE_START
#define BL2_DATA_START                      _SRAM2_BASE_S
#define BL2_DATA_SIZE                       _SRAM2_SIZE_MAX
#define BL2_DATA_LIMIT                      (BL2_DATA_START + BL2_DATA_SIZE - 1)
#define BL2_SRAM_AREA_BASE                  _SRAM1_BASE_NS
#define BL2_SRAM_AREA_END                   (_SRAM3_BASE_S + _SRAM3_SIZE_MAX - 1)

#if S_CODE_START != ECU_SECURE_VECTOR_ADDRESS
#error "Secure application vector and OEMiROT layout differ"
#endif
#if NS_CODE_START != ECU_NONSECURE_VECTOR_ADDRESS
#error "NonSecure application vector and OEMiROT layout differ"
#endif

#endif /* __REGION_DEFS_H__ */
