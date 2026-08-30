/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gtzc_s.c
  * @brief   This file provides code for the configuration
  *          of the GTZC_S instances.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "gtzc_s.h"

/* USER CODE BEGIN 0 */

/*
 * STM32CubeMX 6.18 emits SRAM3 as privileged-only and does not persist an
 * MPCBB3 privilege-vector override in ECU.ioc.  Ethernet DMA transactions are
 * non-privileged, so the SRAM3 window containing the NonSecure ETH
 * descriptors and packet pools must allow non-privileged access.
 *
 * One MPCBB privilege vector covers 32 x 512-byte blocks (16 KiB).  Keep this
 * exception deliberately bounded to 0x20050000..0x2005FFFF (64 KiB).
 */
#define ETH_DMA_SRAM3_PRIV_VECTOR_COUNT  4U

/* USER CODE END 0 */

/* GTZC_S init function */
void MX_GTZC_S_Init(void)
{

  /* USER CODE BEGIN GTZC_S_Init 0 */

  /* USER CODE END GTZC_S_Init 0 */

  MPCBB_ConfigTypeDef MPCBB_Area_Desc = {0};

  /* USER CODE BEGIN GTZC_S_Init 1 */

  MPCBB_ConfigTypeDef ethernet_dma_area_desc = {0};
  uint32_t ethernet_dma_vector;

  /* USER CODE END GTZC_S_Init 1 */
  if (HAL_GTZC_TZIC_EnableIT(GTZC_PERIPH_TIM4) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZIC_EnableIT(GTZC_PERIPH_TIM6) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZIC_EnableIT(GTZC_PERIPH_TIM7) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZIC_EnableIT(GTZC_PERIPH_IWDG) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZIC_EnableIT(GTZC_PERIPH_FDCAN1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZIC_EnableIT(GTZC_PERIPH_FDCAN2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZIC_EnableIT(GTZC_PERIPH_SPI4) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZIC_EnableIT(GTZC_PERIPH_ADC12) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZIC_EnableIT(GTZC_PERIPH_GPDMA1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZIC_EnableIT(GTZC_PERIPH_EXTI) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZSC_ConfigPeriphAttributes(GTZC_PERIPH_TIM4, GTZC_TZSC_PERIPH_SEC|GTZC_TZSC_PERIPH_NPRIV) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZSC_ConfigPeriphAttributes(GTZC_PERIPH_TIM6, GTZC_TZSC_PERIPH_SEC|GTZC_TZSC_PERIPH_NPRIV) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZSC_ConfigPeriphAttributes(GTZC_PERIPH_TIM7, GTZC_TZSC_PERIPH_SEC|GTZC_TZSC_PERIPH_NPRIV) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZSC_ConfigPeriphAttributes(GTZC_PERIPH_IWDG, GTZC_TZSC_PERIPH_SEC|GTZC_TZSC_PERIPH_NPRIV) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZSC_ConfigPeriphAttributes(GTZC_PERIPH_FDCAN1, GTZC_TZSC_PERIPH_SEC|GTZC_TZSC_PERIPH_NPRIV) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZSC_ConfigPeriphAttributes(GTZC_PERIPH_FDCAN2, GTZC_TZSC_PERIPH_SEC|GTZC_TZSC_PERIPH_NPRIV) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZSC_ConfigPeriphAttributes(GTZC_PERIPH_SPI4, GTZC_TZSC_PERIPH_SEC|GTZC_TZSC_PERIPH_NPRIV) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZSC_ConfigPeriphAttributes(GTZC_PERIPH_ADC12, GTZC_TZSC_PERIPH_SEC|GTZC_TZSC_PERIPH_NPRIV) != HAL_OK)
  {
    Error_Handler();
  }
  MPCBB_Area_Desc.SecureRWIllegalMode = GTZC_MPCBB_SRWILADIS_ENABLE;
  MPCBB_Area_Desc.InvertSecureState = GTZC_MPCBB_INVSECSTATE_NOT_INVERTED;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[0] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[1] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[2] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[3] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[4] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[5] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[6] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[7] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[8] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[9] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[10] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[11] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[12] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[13] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[14] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[15] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[16] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[17] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[18] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_SecConfig_array[19] =   0x00000000;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[0] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[1] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[2] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[3] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[4] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[5] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[6] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[7] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[8] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[9] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[10] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[11] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[12] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[13] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[14] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[15] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[16] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[17] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[18] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_PrivConfig_array[19] =   0xFFFFFFFF;
  MPCBB_Area_Desc.AttributeConfig.MPCBB_LockConfig_array[0] =   0x00000000;
  if (HAL_GTZC_MPCBB_ConfigMem(SRAM3_BASE, &MPCBB_Area_Desc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN GTZC_S_Init 2 */

  /* ATECC challenge generation and verification are Secure-only. Keep RNG and
     PKA privileged even if a future CubeMX peripheral list changes defaults. */
  if (HAL_GTZC_TZSC_ConfigPeriphAttributes(
          GTZC_PERIPH_RNG, GTZC_TZSC_PERIPH_SEC | GTZC_TZSC_PERIPH_PRIV) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_GTZC_TZSC_ConfigPeriphAttributes(
          GTZC_PERIPH_PKA, GTZC_TZSC_PERIPH_SEC | GTZC_TZSC_PERIPH_PRIV) != HAL_OK)
  {
    Error_Handler();
  }

  /* CubeMX regeneration-safe post-configuration for the ETH DMA SRAM window. */
  if (HAL_GTZC_MPCBB_GetConfigMem(SRAM3_BASE, &ethernet_dma_area_desc) != HAL_OK)
  {
    Error_Handler();
  }
  for (ethernet_dma_vector = 0U;
       ethernet_dma_vector < ETH_DMA_SRAM3_PRIV_VECTOR_COUNT;
       ethernet_dma_vector++)
  {
    ethernet_dma_area_desc.AttributeConfig.MPCBB_PrivConfig_array[ethernet_dma_vector] = 0U;
  }
  if (HAL_GTZC_MPCBB_ConfigMem(SRAM3_BASE, &ethernet_dma_area_desc) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE END GTZC_S_Init 2 */

}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

