/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    Secure_nsclib/secure_nsc.h
  * @author  MCD Application Team
  * @brief   Header for secure non-secure callable APIs list
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

/* USER CODE BEGIN Non_Secure_CallLib_h */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef SECURE_NSC_H
#define SECURE_NSC_H

/* Includes ------------------------------------------------------------------*/
#include <stdint.h>
#include "safety_api.h"

/* Exported types ------------------------------------------------------------*/
/**
  * @brief  non-secure callback ID enumeration definition
  */
typedef enum
{
SECURE_FAULT_CB_ID     = 0x00U, /*!< System secure fault callback ID */
  GTZC_ERROR_CB_ID       = 0x01U  /*!< GTZC secure error callback ID */
} SECURE_CallbackIDTypeDef;
/* Exported constants --------------------------------------------------------*/
/* Exported macro ------------------------------------------------------------*/
/* Exported functions ------------------------------------------------------- */
void SECURE_RegisterCallback(SECURE_CallbackIDTypeDef CallbackId, void *func);
uint32_t SECURE_SafetyGetStatus(void);
int32_t SECURE_SafetyGetAdcSnapshot(SAFETY_AdcSnapshot *snapshot);
int32_t SECURE_SafetyGetActuatorSnapshot(SAFETY_ActuatorSnapshot *snapshot);
int32_t SECURE_SafetyGetSecurityStatus(SAFETY_SecurityStatus *status);
int32_t SECURE_SafetyOtaGetStatus(SAFETY_OtaStatus *status);
int32_t SECURE_SafetyOtaBegin(const SAFETY_OtaBeginRequest *request,
                              SAFETY_OtaStatus *status);
int32_t SECURE_SafetyOtaWrite(const SAFETY_OtaChunk *chunk,
                              SAFETY_OtaStatus *status);
int32_t SECURE_SafetyOtaFinish(uint32_t update_sequence,
                               SAFETY_OtaStatus *status);
int32_t SECURE_SafetyOtaConfirmRunningImages(void);
#if defined(ECU_FACTORY_PROVISIONING)
int32_t SECURE_SafetyFactoryGetStatus(SAFETY_FactoryStatus *status);
int32_t SECURE_SafetyFactoryProvision(
    const SAFETY_FactoryProvisionRequest *request,
    SAFETY_FactoryStatus *status);
#endif
int32_t SECURE_SafetyGetValveConfig(SAFETY_ValveConfigSnapshot *snapshot);
int32_t SECURE_SafetyApplyValveConfig(const SAFETY_ValveConfig *config);
int32_t SECURE_SafetySaveValveConfig(void);
int32_t SECURE_SafetyReloadValveConfig(void);
int32_t SECURE_SafetyReadValveTelemetry(SAFETY_ValveTelemetryBatch *batch);
int32_t SECURE_SafetyClearFault(uint32_t request_token);
int32_t SECURE_SafetyArmOutputs(uint32_t request_token);
int32_t SECURE_SafetyDisarmOutputs(void);
int32_t SECURE_SafetySubmitActuatorCommand(
    const SAFETY_ActuatorCommand *command);
int32_t SECURE_SafetyKickWatchdog(uint32_t heartbeat);

#endif /* SECURE_NSC_H */
/* USER CODE END Non_Secure_CallLib_h */

