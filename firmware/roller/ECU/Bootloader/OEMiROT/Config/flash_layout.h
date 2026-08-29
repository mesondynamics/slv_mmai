/*
 * Based on STMicroelectronics STM32CubeH5 v1.7.0 OEMiROT_Boot flash_layout.h
 * and MCUboot. This product-specific layout uses scratch swap so an interrupted
 * update can resume or revert without destroying the last bootable image.
 */
#ifndef __FLASH_LAYOUT_H__
#define __FLASH_LAYOUT_H__

#include "ecu_flash_layout.h"

/* MCUBOOT_OVERWRITE_ONLY intentionally not defined: use scratch swap. */
/* MCUBOOT_EXT_LOADER intentionally not defined: OTA is application mediated. */
#define MCUBOOT_APP_IMAGE_NUMBER            2
#define MCUBOOT_S_DATA_IMAGE_NUMBER         0
#define MCUBOOT_NS_DATA_IMAGE_NUMBER        0
#define MCUBOOT_IMAGE_NUMBER                2
#define MCUBOOT_USE_HASH_REF

#define LOADER_FLASH_DEV_NAME               Driver_FLASH0
#define FLASH_AREA_IMAGE_SECTOR_SIZE        ECU_FLASH_SECTOR_SIZE
#define FLASH_AREA_WRP_GROUP_SIZE           0x00008000
#define FLASH_B_SIZE                        ECU_FLASH_BANK_SIZE
#define FLASH_TOTAL_SIZE                    ECU_FLASH_TOTAL_SIZE
#define FLASH_BASE_ADDRESS                  ECU_FLASH_BASE_NS

#define FLASH_AREA_0_ID                     1
#define FLASH_AREA_1_ID                     2
#define FLASH_AREA_2_ID                     3
#define FLASH_AREA_3_ID                     4
#define FLASH_AREA_SCRATCH_ID               9

#define FLASH_AREA_BL2_OFFSET               ECU_BOOT_OFFSET
#define FLASH_AREA_BL2_SIZE                 ECU_BOOT_SIZE
#define FLASH_AREA_SCRATCH_DEVICE_ID        (FLASH_DEVICE_ID - FLASH_DEVICE_ID)
#define FLASH_AREA_SCRATCH_OFFSET           ECU_SCRATCH_OFFSET
#define FLASH_AREA_SCRATCH_SIZE             ECU_SCRATCH_SIZE
#define FLASH_BL2_HDP_END                   (FLASH_AREA_SCRATCH_OFFSET + FLASH_AREA_SCRATCH_SIZE - 1)

#define FLASH_S_PARTITION_SIZE              ECU_SECURE_PRIMARY_SIZE
#define FLASH_NS_PARTITION_SIZE             ECU_NONSECURE_PRIMARY_SIZE
#define FLASH_PARTITION_SIZE                (FLASH_S_PARTITION_SIZE + FLASH_NS_PARTITION_SIZE)
#define FLASH_S_DATA_PARTITION_SIZE         0
#define FLASH_NS_DATA_PARTITION_SIZE        0
#define FLASH_MAX_APP_PARTITION_SIZE        FLASH_NS_PARTITION_SIZE
#define FLASH_MAX_DATA_PARTITION_SIZE       0
#define FLASH_MAX_PARTITION_SIZE            FLASH_MAX_APP_PARTITION_SIZE

#define FLASH_AREAS_DEVICE_ID               (FLASH_DEVICE_ID - FLASH_DEVICE_ID)
#define FLASH_AREA_BEGIN_OFFSET             ECU_SECURE_PRIMARY_OFFSET
#define FLASH_AREA_END_OFFSET               (ECU_NONSECURE_PRIMARY_OFFSET + ECU_NONSECURE_PRIMARY_SIZE)

#define FLASH_AREA_0_DEVICE_ID              FLASH_AREAS_DEVICE_ID
#define FLASH_AREA_0_OFFSET                 ECU_SECURE_PRIMARY_OFFSET
#define FLASH_AREA_0_SIZE                   ECU_SECURE_PRIMARY_SIZE
#define FLASH_AREA_1_DEVICE_ID              FLASH_AREAS_DEVICE_ID
#define FLASH_AREA_1_OFFSET                 ECU_NONSECURE_PRIMARY_OFFSET
#define FLASH_AREA_1_SIZE                   ECU_NONSECURE_PRIMARY_SIZE
#define FLASH_AREA_2_DEVICE_ID              FLASH_AREAS_DEVICE_ID
#define FLASH_AREA_2_OFFSET                 ECU_SECURE_SECONDARY_OFFSET
#define FLASH_AREA_2_SIZE                   ECU_SECURE_SECONDARY_SIZE
#define FLASH_AREA_3_DEVICE_ID              FLASH_AREAS_DEVICE_ID
#define FLASH_AREA_3_OFFSET                 ECU_NONSECURE_SECONDARY_OFFSET
#define FLASH_AREA_3_SIZE                   ECU_NONSECURE_SECONDARY_SIZE
#define FLASH_AREA_4_OFFSET                 0
#define FLASH_AREA_4_SIZE                   0
#define FLASH_AREA_5_OFFSET                 0
#define FLASH_AREA_5_SIZE                   0
#define FLASH_AREA_6_OFFSET                 0
#define FLASH_AREA_6_SIZE                   0
#define FLASH_AREA_7_OFFSET                 0
#define FLASH_AREA_7_SIZE                   0

#if ((FLASH_AREA_BL2_OFFSET + FLASH_AREA_BL2_SIZE) % FLASH_AREA_WRP_GROUP_SIZE) != 0
#error "OEMiROT size must align to a 32 KiB WRP group"
#endif
#if (FLASH_AREA_SCRATCH_OFFSET % FLASH_AREA_IMAGE_SECTOR_SIZE) != 0
#error "Scratch offset is not sector aligned"
#endif
#if (FLASH_AREA_0_OFFSET % FLASH_AREA_IMAGE_SECTOR_SIZE) != 0 || \
    (FLASH_AREA_1_OFFSET % FLASH_AREA_IMAGE_SECTOR_SIZE) != 0 || \
    (FLASH_AREA_2_OFFSET % FLASH_AREA_IMAGE_SECTOR_SIZE) != 0 || \
    (FLASH_AREA_3_OFFSET % FLASH_AREA_IMAGE_SECTOR_SIZE) != 0
#error "MCUboot slot is not sector aligned"
#endif
#if (FLASH_AREA_0_SIZE != FLASH_AREA_2_SIZE) || \
    (FLASH_AREA_1_SIZE != FLASH_AREA_3_SIZE)
#error "Each MCUboot primary/secondary slot pair must have equal sizes"
#endif
#if (ECU_NONSECURE_SECONDARY_OFFSET + ECU_NONSECURE_SECONDARY_SIZE) > ECU_PERSISTENT_OFFSET
#error "NonSecure secondary overlaps secure persistent storage"
#endif
#if (ECU_PERSISTENT_OFFSET + ECU_PERSISTENT_SIZE) > ECU_NONSECURE_PRIMARY_OFFSET
#error "Secure persistent storage crosses Bank 1"
#endif
#if FLASH_AREA_END_OFFSET > FLASH_TOTAL_SIZE
#error "Flash layout exceeds STM32H563 flash"
#endif

#define MCUBOOT_STATUS_MAX_ENTRIES          (((FLASH_MAX_PARTITION_SIZE - 1) / FLASH_AREA_SCRATCH_SIZE) + 1)
#define MCUBOOT_MAX_IMG_SECTORS             (FLASH_MAX_PARTITION_SIZE / FLASH_AREA_IMAGE_SECTOR_SIZE)

#define SECURE_IMAGE_OFFSET                 0
#define SECURE_IMAGE_MAX_SIZE               FLASH_S_PARTITION_SIZE
#define NON_SECURE_IMAGE_OFFSET             0
#define NON_SECURE_IMAGE_MAX_SIZE           FLASH_NS_PARTITION_SIZE

#define FLASH_PRIMARY_SECURE_DEV_NAME       Driver_FLASH0
#define FLASH_PRIMARY_NONSECURE_DEV_NAME    Driver_FLASH0
#define FLASH_DEV_NAME                      Driver_FLASH0

/* NV counters and image hash references are held in protected OBKeys. */
#define BL2_NV_COUNTERS_AREA_ADDR            FLASH_BL2_NVCNT_AREA_OFFSET
#define BL2_NV_COUNTERS_AREA_SIZE            FLASH_BL2_NVCNT_AREA_SIZE
#define OBK_HDPL0_OFFSET                     0x000
#define OBK_HDPL0_END                        0x0FF
#define OBK_HDPL1_OFFSET                     0x100
#define OBK_HDPL1_END                        0x8FF
#define OBK_HDPL2_OFFSET                     0x900
#define OBK_HDPL2_END                        0xBFF
#define OBK_HDPL3_OFFSET                     0xC00
#define OBK_HDPL3_END                        0x1FFF

#define ENGI_BASE_NS                         0x08FFF800
#define ENGI_SIZE                            0x40
#define RSS_LIB_BASE                         0x0FF94000
#define RSS_LIB_SIZE                         0x2000
#define BOOTLOADER_BASE_NS                   0x0BF97000
#define BOOTLOADER_SIZE                      0x9400
#define STM32_DESCRIPTOR_BASE_NS_3           0x0BF9FB00
#define STM32_DESCRIPTOR_BASE_NS_2           0x0BF9FD00
#define STM32_DESCRIPTOR_BASE_NS_1           0x0BF9FE00
#define RSSLIB_PFUNC_3                       0x0BF9FB68UL
#define RSSLIB_PFUNC_2                       0x0BF9FD68UL
#define RSSLIB_PFUNC_1                       0x0BF9FE68UL
#define STM32_DESCRIPTOR_SIZE                0x100
#define STM32_DESCRIPTOR_BASE_NS             STM32_DESCRIPTOR_BASE_NS_3
#define STM32_DESCRIPTOR_END_NS              (STM32_DESCRIPTOR_BASE_NS_1 + STM32_DESCRIPTOR_SIZE - 1)

#endif /* __FLASH_LAYOUT_H__ */
