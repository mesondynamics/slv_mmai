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

CMSE_NS_ENTRY int32_t SECURE_SafetyGetActuatorSnapshot(
    SAFETY_ActuatorSnapshot *snapshot)
{
  SAFETY_ActuatorSnapshot *checked_snapshot;

  checked_snapshot = (SAFETY_ActuatorSnapshot *)cmse_check_address_range(
      snapshot, sizeof(*snapshot), CMSE_NONSECURE | CMSE_MPU_READWRITE);
  if (checked_snapshot == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  return Safety_GetActuatorSnapshot(checked_snapshot);
}

CMSE_NS_ENTRY int32_t SECURE_SafetyGetSecurityStatus(
    SAFETY_SecurityStatus *status)
{
  SAFETY_SecurityStatus *checked_status;

  checked_status = (SAFETY_SecurityStatus *)cmse_check_address_range(
      status, sizeof(*status), CMSE_NONSECURE | CMSE_MPU_READWRITE);
  if (checked_status == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  return Safety_GetSecurityStatus(checked_status);
}

CMSE_NS_ENTRY int32_t SECURE_SafetyOtaGetStatus(SAFETY_OtaStatus *status)
{
  SAFETY_OtaStatus *checked_status =
      (SAFETY_OtaStatus *)cmse_check_address_range(
          status, sizeof(*status), CMSE_NONSECURE | CMSE_MPU_READWRITE);
  if (checked_status == NULL) { return SAFETY_RESULT_BAD_ARGUMENT; }
  return Safety_OtaGetStatus(checked_status);
}

CMSE_NS_ENTRY int32_t SECURE_SafetyOtaBegin(
    const SAFETY_OtaBeginRequest *request, SAFETY_OtaStatus *status)
{
  const SAFETY_OtaBeginRequest *checked_request =
      (const SAFETY_OtaBeginRequest *)cmse_check_address_range(
          (void *)request, sizeof(*request), CMSE_NONSECURE | CMSE_MPU_READ);
  SAFETY_OtaStatus *checked_status =
      (SAFETY_OtaStatus *)cmse_check_address_range(
          status, sizeof(*status), CMSE_NONSECURE | CMSE_MPU_READWRITE);
  SAFETY_OtaBeginRequest secure_request;
  if ((checked_request == NULL) || (checked_status == NULL))
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  secure_request = *checked_request;
  return Safety_OtaBegin(&secure_request, checked_status);
}

CMSE_NS_ENTRY int32_t SECURE_SafetyOtaWrite(
    const SAFETY_OtaChunk *chunk, SAFETY_OtaStatus *status)
{
  const SAFETY_OtaChunk *checked_chunk =
      (const SAFETY_OtaChunk *)cmse_check_address_range(
          (void *)chunk, sizeof(*chunk), CMSE_NONSECURE | CMSE_MPU_READ);
  SAFETY_OtaStatus *checked_status =
      (SAFETY_OtaStatus *)cmse_check_address_range(
          status, sizeof(*status), CMSE_NONSECURE | CMSE_MPU_READWRITE);
  SAFETY_OtaChunk secure_chunk;
  if ((checked_chunk == NULL) || (checked_status == NULL))
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  secure_chunk = *checked_chunk;
  return Safety_OtaWrite(&secure_chunk, checked_status);
}

CMSE_NS_ENTRY int32_t SECURE_SafetyOtaFinish(
    uint32_t update_sequence, SAFETY_OtaStatus *status)
{
  SAFETY_OtaStatus *checked_status =
      (SAFETY_OtaStatus *)cmse_check_address_range(
          status, sizeof(*status), CMSE_NONSECURE | CMSE_MPU_READWRITE);
  if (checked_status == NULL) { return SAFETY_RESULT_BAD_ARGUMENT; }
  return Safety_OtaFinish(update_sequence, checked_status);
}

CMSE_NS_ENTRY int32_t SECURE_SafetyOtaConfirmRunningImages(void)
{
  return Safety_OtaConfirmRunningImages();
}

#if defined(ECU_FACTORY_PROVISIONING)
CMSE_NS_ENTRY int32_t SECURE_SafetyFactoryGetStatus(
    SAFETY_FactoryStatus *status)
{
  SAFETY_FactoryStatus *checked_status =
      (SAFETY_FactoryStatus *)cmse_check_address_range(
          status, sizeof(*status), CMSE_NONSECURE | CMSE_MPU_READWRITE);
  if (checked_status == NULL) { return SAFETY_RESULT_BAD_ARGUMENT; }
  return Safety_FactoryGetStatus(checked_status);
}

CMSE_NS_ENTRY int32_t SECURE_SafetyFactoryProvision(
    const SAFETY_FactoryProvisionRequest *request,
    SAFETY_FactoryStatus *status)
{
  const SAFETY_FactoryProvisionRequest *checked_request =
      (const SAFETY_FactoryProvisionRequest *)cmse_check_address_range(
          (void *)request, sizeof(*request), CMSE_NONSECURE);
  SAFETY_FactoryStatus *checked_status =
      (SAFETY_FactoryStatus *)cmse_check_address_range(
          status, sizeof(*status), CMSE_NONSECURE | CMSE_MPU_READWRITE);
  if ((checked_request == NULL) || (checked_status == NULL))
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  return Safety_FactoryProvision(checked_request, checked_status);
}
#endif

CMSE_NS_ENTRY int32_t SECURE_SafetyGetValveConfig(
    SAFETY_ValveConfigSnapshot *snapshot)
{
  SAFETY_ValveConfigSnapshot *checked_snapshot;

  checked_snapshot = (SAFETY_ValveConfigSnapshot *)cmse_check_address_range(
      snapshot, sizeof(*snapshot), CMSE_NONSECURE | CMSE_MPU_READWRITE);
  if (checked_snapshot == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  return Safety_GetValveConfig(checked_snapshot);
}

CMSE_NS_ENTRY int32_t SECURE_SafetyApplyValveConfig(
    const SAFETY_ValveConfig *config)
{
  const SAFETY_ValveConfig *checked_config;
  SAFETY_ValveConfig secure_config;

  checked_config = (const SAFETY_ValveConfig *)cmse_check_address_range(
      (void *)config, sizeof(*config), CMSE_NONSECURE | CMSE_MPU_READ);
  if (checked_config == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  secure_config = *checked_config;
  return Safety_ApplyValveConfig(&secure_config);
}

CMSE_NS_ENTRY int32_t SECURE_SafetySaveValveConfig(void)
{
  return Safety_SaveValveConfig();
}

CMSE_NS_ENTRY int32_t SECURE_SafetyReloadValveConfig(void)
{
  return Safety_ReloadValveConfig();
}

CMSE_NS_ENTRY int32_t SECURE_SafetyReadValveTelemetry(
    SAFETY_ValveTelemetryBatch *batch)
{
  SAFETY_ValveTelemetryBatch *checked_batch;

  checked_batch = (SAFETY_ValveTelemetryBatch *)cmse_check_address_range(
      batch, sizeof(*batch), CMSE_NONSECURE | CMSE_MPU_READWRITE);
  if (checked_batch == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  return Safety_ReadValveTelemetry(checked_batch);
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

CMSE_NS_ENTRY int32_t SECURE_SafetySubmitActuatorCommand(
    const SAFETY_ActuatorCommand *command)
{
  const SAFETY_ActuatorCommand *checked_command;
  SAFETY_ActuatorCommand secure_command;

  checked_command = (const SAFETY_ActuatorCommand *)cmse_check_address_range(
      (void *)command, sizeof(*command), CMSE_NONSECURE | CMSE_MPU_READ);
  if (checked_command == NULL)
  {
    return SAFETY_RESULT_BAD_ARGUMENT;
  }
  /* Copy once after attribution/MPU validation so NonSecure cannot alter
     fields between validation and application. */
  secure_command = *checked_command;
  return Safety_SubmitActuatorCommand(&secure_command);
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

