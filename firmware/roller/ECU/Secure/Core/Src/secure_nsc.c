/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    Secure/Src/secure_nsc.c
  * @author  MCD Application Team
  * @brief   This file contains the non-secure callable APIs (secure world)
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

/* USER CODE BEGIN Non_Secure_CallLib */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "secure_nsc.h"
#include "safety_service.h"
#include <arm_cmse.h>
/** @addtogroup STM32H5xx_HAL_Examples

  * @{
  */

/** @addtogroup Templates
  * @{
  */

/* Global variables ----------------------------------------------------------*/
void *pSecureFaultCallback = NULL;   /* Pointer to secure fault callback in Non-secure */
void *pSecureErrorCallback = NULL;   /* Pointer to secure error callback in Non-secure */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Secure registration of non-secure callback.
  * @param  CallbackId  callback identifier
  * @param  func        pointer to non-secure function
  * @retval None
  */
    CMSE_NS_ENTRY void SECURE_RegisterCallback(SECURE_CallbackIDTypeDef CallbackId, void *func)
    {
      void *checked_func = NULL;

      if(func != NULL)
      {
        checked_func = cmse_check_address_range(
            (void *)((uintptr_t)func & ~(uintptr_t)1U),
            sizeof(uint16_t), CMSE_NONSECURE);
      }
      if(checked_func != NULL)
      {
        switch(CallbackId)
        {
          case SECURE_FAULT_CB_ID:           /* SecureFault Interrupt occurred */
          pSecureFaultCallback = cmse_nsfptr_create(func);
          break;
          case GTZC_ERROR_CB_ID:             /* GTZC Interrupt occurred */
          pSecureErrorCallback = cmse_nsfptr_create(func);
          break;
          default:
          /* unknown */
          break;
        }
      }
    }

CMSE_NS_ENTRY uint32_t SECURE_SafetyGetStatus(void)
{
  return Safety_GetStatus();
}

CMSE_NS_ENTRY int32_t SECURE_SafetyGetAdcSnapshot(SAFETY_AdcSnapshot *snapshot)
{
  SAFETY_AdcSnapshot *checked_snapshot;

  checked_snapshot = (SAFETY_AdcSnapshot *)cmse_check_address_range(
      snapshot, sizeof(*snapshot), CMSE_NONSECURE | CMSE_MPU_READWRITE);
  if (checked_snapshot == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  return Safety_GetAdcSnapshot(checked_snapshot);
}

CMSE_NS_ENTRY int32_t SECURE_SafetyClearFault(uint32_t request_token)
{
  return Safety_ClearFault(request_token);
}

CMSE_NS_ENTRY int32_t SECURE_SafetyArmOutputs(uint32_t request_token)
{
  return Safety_ArmOutputs(request_token);
}

CMSE_NS_ENTRY int32_t SECURE_SafetyDisarmOutputs(void)
{
  return Safety_DisarmOutputs();
}

CMSE_NS_ENTRY int32_t SECURE_SafetySetPwm(uint16_t forward_compare,
                                          uint16_t reverse_compare,
                                          uint32_t command_sequence)
{
  return Safety_SetPwm(forward_compare, reverse_compare, command_sequence);
}

CMSE_NS_ENTRY int32_t SECURE_SafetyKickWatchdog(uint32_t heartbeat)
{
  return Safety_KickWatchdog(heartbeat);
}

/**
  * @}
  */

/**
  * @}
  */
/* USER CODE END Non_Secure_CallLib */

