/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined ( __ICCARM__ )
#  define CMSE_NS_CALL  __cmse_nonsecure_call
#  define CMSE_NS_ENTRY __cmse_nonsecure_entry
#else
#  define CMSE_NS_CALL  __attribute((cmse_nonsecure_call))
#  define CMSE_NS_ENTRY __attribute((cmse_nonsecure_entry))
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h5xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* Function pointer declaration in non-secure*/
#if defined ( __ICCARM__ )
typedef void (CMSE_NS_CALL *funcptr)(void);
#else
typedef void CMSE_NS_CALL (*funcptr)(void);
#endif

/* typedef for non-secure callback functions */
typedef funcptr funcptr_NS;

/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define TPIC_SRCK_Pin GPIO_PIN_2
#define TPIC_SRCK_GPIO_Port GPIOE
#define TPIC_OE_N_Pin GPIO_PIN_3
#define TPIC_OE_N_GPIO_Port GPIOE
#define TPIC_RCK_Pin GPIO_PIN_4
#define TPIC_RCK_GPIO_Port GPIOE
#define TPIC_CLR_N_Pin GPIO_PIN_5
#define TPIC_CLR_N_GPIO_Port GPIOE
#define TPIC_SER_Pin GPIO_PIN_6
#define TPIC_SER_GPIO_Port GPIOE
#define RCC_OSC_IN_Pin GPIO_PIN_0
#define RCC_OSC_IN_GPIO_Port GPIOH
#define OIL_LEVEL_ADC_Pin GPIO_PIN_0
#define OIL_LEVEL_ADC_GPIO_Port GPIOC
#define RMII_MDC_Pin GPIO_PIN_1
#define RMII_MDC_GPIO_Port GPIOC
#define OIL_TEMP_ADC_Pin GPIO_PIN_2
#define OIL_TEMP_ADC_GPIO_Port GPIOC
#define WATER_LEVEL_ADC_Pin GPIO_PIN_3
#define WATER_LEVEL_ADC_GPIO_Port GPIOC
#define SPEED_IN_Pin GPIO_PIN_0
#define SPEED_IN_GPIO_Port GPIOA
#define RMII_REF_CLK_Pin GPIO_PIN_1
#define RMII_REF_CLK_GPIO_Port GPIOA
#define RMII_MDIO_Pin GPIO_PIN_2
#define RMII_MDIO_GPIO_Port GPIOA
#define TEMP_SENSOR_1_ADC_Pin GPIO_PIN_3
#define TEMP_SENSOR_1_ADC_GPIO_Port GPIOA
#define FWD_VALVE_CURR_ADC_Pin GPIO_PIN_4
#define FWD_VALVE_CURR_ADC_GPIO_Port GPIOA
#define TEMP_SENSOR_2_ADC_Pin GPIO_PIN_5
#define TEMP_SENSOR_2_ADC_GPIO_Port GPIOA
#define REV_VALVE_CURR_ADC_Pin GPIO_PIN_6
#define REV_VALVE_CURR_ADC_GPIO_Port GPIOA
#define RMII_CRS_DV_Pin GPIO_PIN_7
#define RMII_CRS_DV_GPIO_Port GPIOA
#define RMII_RXD0_Pin GPIO_PIN_4
#define RMII_RXD0_GPIO_Port GPIOC
#define RMII_RXD1_Pin GPIO_PIN_5
#define RMII_RXD1_GPIO_Port GPIOC
#define ENGINE_SENSOR_ADC_Pin GPIO_PIN_0
#define ENGINE_SENSOR_ADC_GPIO_Port GPIOB
#define VBAT_12V_ADC_Pin GPIO_PIN_1
#define VBAT_12V_ADC_GPIO_Port GPIOB
#define ESTOP_DETECT_Pin GPIO_PIN_2
#define ESTOP_DETECT_GPIO_Port GPIOB
#define TPIC_CTRL_BUF_EN_Pin GPIO_PIN_7
#define TPIC_CTRL_BUF_EN_GPIO_Port GPIOE
#define CAN2_RX_Pin GPIO_PIN_12
#define CAN2_RX_GPIO_Port GPIOB
#define CAN2_TX_Pin GPIO_PIN_13
#define CAN2_TX_GPIO_Port GPIOB
#define RMII_NRST_Pin GPIO_PIN_14
#define RMII_NRST_GPIO_Port GPIOB
#define RMII_TXD1_Pin GPIO_PIN_15
#define RMII_TXD1_GPIO_Port GPIOB
#define SWDIO_Pin GPIO_PIN_13
#define SWDIO_GPIO_Port GPIOA
#define SWCLK_Pin GPIO_PIN_14
#define SWCLK_GPIO_Port GPIOA
#define CAN1_RX_Pin GPIO_PIN_0
#define CAN1_RX_GPIO_Port GPIOD
#define CAN1_TX_Pin GPIO_PIN_1
#define CAN1_TX_GPIO_Port GPIOD
#define RMII_TX_EN_Pin GPIO_PIN_11
#define RMII_TX_EN_GPIO_Port GPIOG
#define RMII_TXD0_Pin GPIO_PIN_13
#define RMII_TXD0_GPIO_Port GPIOG
#define FWD_VALVE_PWM_Pin GPIO_PIN_6
#define FWD_VALVE_PWM_GPIO_Port GPIOB
#define REV_VALVE_PWM_Pin GPIO_PIN_7
#define REV_VALVE_PWM_GPIO_Port GPIOB
#define SW_I2C_SDA_Pin GPIO_PIN_8
#define SW_I2C_SDA_GPIO_Port GPIOB
#define SW_I2C_SCL_Pin GPIO_PIN_9
#define SW_I2C_SCL_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
