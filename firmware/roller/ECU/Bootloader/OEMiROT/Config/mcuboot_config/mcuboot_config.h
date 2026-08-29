/* Product policy derived from STM32CubeH5 v1.7.0 OEMiROT_Boot. */
#ifndef __MCUBOOT_CONFIG_H__
#define __MCUBOOT_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __BOOTSIM__
#define MCUBOOT_FIH_PROFILE_HIGH
#define MCUBOOT_FLASH_HOMOGENOUS
#define CRYPTO_SCHEME_EC256               0x2
#define CRYPTO_SCHEME                     CRYPTO_SCHEME_EC256
#define NUM_ECC_BYTES                     32
#define MCUBOOT_SIGN_EC256
#define MCUBOOT_ENCRYPT_EC256
#define MCUBOOT_VALIDATE_PRIMARY_SLOT
#define MCUBOOT_USE_FLASH_AREA_GET_SECTORS
#define MCUBOOT_HW_ROLLBACK_PROT
#define MCUBOOT_ENC_IMAGES
#define MCUBOOT_BOOTSTRAP
#define MCUBOOT_USE_MBED_TLS
#include "stm32h5xx_hal.h"
#ifdef OEMIROT_DEV_MODE
#define MCUBOOT_HAVE_LOGGING
#endif
#endif /* !__BOOTSIM__ */

#define MCUBOOT_WATCHDOG_FEED() do { } while (0)

#ifdef __cplusplus
}
#endif

#endif /* __MCUBOOT_CONFIG_H__ */
