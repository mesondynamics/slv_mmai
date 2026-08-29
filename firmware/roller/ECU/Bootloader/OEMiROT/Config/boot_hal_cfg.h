/**
  ******************************************************************************
  * @file    boot_hal_cfg.h
  * @brief   Roller ECU OEMiROT platform policy derived from CubeH5 v1.7.0
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2023 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

#ifndef BOOT_HAL_CFG_H
#define BOOT_HAL_CFG_H

#include "stm32h5xx_hal.h"

#define RTC_CLOCK_SOURCE_LSI
#ifdef RTC_CLOCK_SOURCE_LSI
#define RTC_ASYNCH_PREDIV  0x7F
#define RTC_SYNCH_PREDIV   0x00F9
#endif
#ifdef RTC_CLOCK_SOURCE_LSE
#define RTC_ASYNCH_PREDIV  0x7F
#define RTC_SYNCH_PREDIV   0x00FF
#endif

#define OEMIROT_ICACHE_ENABLE

#define OEMIROT_WRP_PROTECT_ENABLE
#define OEMIROT_HDP_PROTECT_ENABLE
#define OEMIROT_SECURE_USER_SRAM2_ERASE_AT_RESET
#define OEMIROT_SECURE_USER_SRAM2_ECC

/* Keep build optimization/error behavior independent of the irreversible
   product-state gate. ReleaseOpen is a fully optimized, non-development boot
   used only while the bench device remains OPEN. ReleaseClosed requires the
   hardware CLOSED state and is the image used at the final production gate. */
#if defined(OEMIROT_DEV_MODE) || defined(ROLLER_OEMIROT_OPEN_BENCH)
#define OEMIROT_OB_PRODUCT_STATE_VALUE OB_PROD_STATE_OPEN
#else
#define OEMIROT_OB_PRODUCT_STATE_VALUE OB_PROD_STATE_CLOSED
#endif

#define NO_TAMPER            (0)
#define INTERNAL_TAMPER_ONLY (1)
#define ALL_TAMPER           (2)
#define OEMIROT_TAMPER_ENABLE INTERNAL_TAMPER_ONLY

#define OEMIROT_OB_BOOT_LOCK OB_BOOT_LOCK_ENABLE

#ifdef OEMIROT_DEV_MODE
#define OEMIROT_ERROR_HANDLER_STOP_EXEC
#endif

#define OEMIROT_FLASH_PRIVONLY_ENABLE
#define OEMIROT_MPU_PROTECTION
#define OEMIROT_FAST_WAKE_UP

typedef enum
{
  OEMIROT_SUCCESS = 0U,
  OEMIROT_FAILED
} OEMIROT_ErrorStatus;

void Error_Handler(void) __NO_RETURN;

#endif /* BOOT_HAL_CFG_H */
