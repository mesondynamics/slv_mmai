/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "adc.h"
#include "gpdma.h"
#include "gtzc_s.h"
#include "icache.h"
#include "iwdg.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ecu_flash_layout.h"
#include "safety_service.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* USER CODE BEGIN VTOR_TABLE */

/* Non-secure Vector table to jump to (internal Flash Bank2 here)             */
/* Caution: address must correspond to non-secure internal Flash where is     */
/*          mapped in the non-secure vector table                             */
#if defined(ECU_OEMIROT_LAYOUT)
#define VTOR_TABLE_NS_START_ADDR  0x08100400U
#else
#define VTOR_TABLE_NS_START_ADDR  0x08100000U
#endif

/* USER CODE END VTOR_TABLE*/

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void NonSecure_Init(void);
void SystemClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

#define SECURITY_SAU_UID_REGION          7U
#define SECURITY_SAU_UID_WINDOW_SIZE     0x40U

static void SecuritySau_SetRegion(uint32_t region, uint32_t base,
                                  uint32_t limit, uint32_t nsc)
{
  SAU->RNR = region & SAU_RNR_REGION_Msk;
  SAU->RBAR = base & SAU_RBAR_BADDR_Msk;
  SAU->RLAR = (limit & SAU_RLAR_LADDR_Msk) |
              ((nsc != 0U) ? SAU_RLAR_NSC_Msk : 0U) |
              SAU_RLAR_ENABLE_Msk;
}

static void SecurityMpu_ClearOemirotInheritedRegions(void)
{
  uint32_t region;
  uint32_t region_count =
      (MPU->TYPE & MPU_TYPE_DREGION_Msk) >> MPU_TYPE_DREGION_Pos;

  /*
   * JumpHDPLvl3 deliberately hands the application an enabled MPU whose
   * regions constrain the OEMiRoT execution surface.  CubeMX MPU_Config()
   * overwrites region 0 but does not disable the remaining inherited regions.
   * They therefore retain higher priority than the application region and can
   * reject otherwise valid HDPL3 accesses.  Preserve the CubeMX-owned region 0
   * and explicitly retire every boot-stage-only comparator before re-enabling
   * the privileged background map.
   */
  HAL_MPU_Disable();
  for (region = 1U; region < region_count; ++region)
  {
    MPU->RNR = region;
    MPU->RLAR = 0U;
  }
  MPU->RNR = MPU_REGION_NUMBER0;
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
  __DSB();
  __ISB();
}

static void SecuritySau_ConfigureOemirotApplication(void)
{
  /*
   * JumpHDPLvl3 leaves RSS hand-off regions in the SAU, while the generic
   * CubeMX SystemInit deliberately disables the SAU.  Rebuild the minimum
   * HDPL3 application attribution here without rewriting RSS-owned region 2.
   * On STM32H563 RSS uses region 2 while transitioning through HDPL1; direct
   * replacement of that comparator makes the engineering-information bus
   * window fault even if an overlapping region is subsequently installed.
   * Region 7 therefore carries the runtime UID/flash-size window.
   *
   * This code is in a CubeMX USER CODE section because the CubeMX SAU page
   * cannot express an OEMiROT hand-off that preserves an RSS-owned region.
   */
  SAU->CTRL = 0U;
  __DSB();
  __ISB();

  SecuritySau_SetRegion(
      0U,
      ECU_FLASH_BASE_S + ECU_CMSE_VENEER_OFFSET,
      ECU_FLASH_BASE_S + ECU_CMSE_VENEER_OFFSET +
          ECU_CMSE_VENEER_SIZE - 1U,
      1U);
  SecuritySau_SetRegion(
      1U,
      ECU_FLASH_BASE_NS + ECU_NONSECURE_PRIMARY_OFFSET,
      ECU_FLASH_BASE_NS + ECU_NONSECURE_PRIMARY_OFFSET +
          ECU_NONSECURE_PRIMARY_SIZE - 1U,
      0U);
  SecuritySau_SetRegion(
      3U,
      PERIPH_BASE_NS,
      PERIPH_BASE_NS + 0x0FFFFFFFUL,
      0U);
  SecuritySau_SetRegion(
      4U,
      SRAM3_BASE_NS,
      SRAM3_BASE_NS + SRAM3_SIZE - 1U,
      0U);
  SecuritySau_SetRegion(
      SECURITY_SAU_UID_REGION,
      UID_BASE,
      UID_BASE + SECURITY_SAU_UID_WINDOW_SIZE - 1U,
      0U);

  SAU->CTRL = SAU_CTRL_ENABLE_Msk;
  __DSB();
  __ISB();
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* Retire OEMiRoT-only MPU regions, then restore the reviewed HDPL3
     attribution before the first STM32 UID read.  Both changes are in a
     CubeMX USER CODE block and therefore survive code regeneration. */
  SecurityMpu_ClearOemirotInheritedRegions();
  SecuritySau_ConfigureOemirotApplication();

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();
  /* GTZC initialisation */
  MX_GTZC_S_Init();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_GPDMA1_Init();
  MX_IWDG_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_SPI4_Init();
  MX_TIM4_Init();
  MX_TIM6_Init();
  MX_ICACHE_Init();
  /* USER CODE BEGIN 2 */
  if (Safety_ServiceInit() != SAFETY_RESULT_OK)
  {
    Error_Handler();
  }
  /* USER CODE END 2 */

  /*************** Setup and jump to non-secure *******************************/

  NonSecure_Init();

  /* Non-secure software does not return, this code is not executed */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief  Non-secure call function
  *         This function is responsible for Non-secure initialization and switch
  *         to non-secure state
  * @retval None
  */
static void NonSecure_Init(void)
{
  funcptr_NS NonSecure_ResetHandler;

  SCB_NS->VTOR = VTOR_TABLE_NS_START_ADDR;

  /* Set non-secure main stack (MSP_NS) */
  __TZ_set_MSP_NS((*(uint32_t *)VTOR_TABLE_NS_START_ADDR));

  /* Get non-secure reset handler */
  NonSecure_ResetHandler = (funcptr_NS)(*((uint32_t *)((VTOR_TABLE_NS_START_ADDR) + 4U)));

  /* Start non-secure state software application */
  NonSecure_ResetHandler();
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLL1_SOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 100;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1_VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1_VCORANGE_WIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_PCLK3;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }

  /** Enables the Clock Security System
  */
  HAL_RCC_EnableCSS();

  /** Configure the programming delay
  */
  __HAL_FLASH_SET_PROGRAM_DELAY(FLASH_PROGRAMMING_DELAY_2);
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};
  MPU_Attributes_InitTypeDef MPU_AttributesInit = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region 0 and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x08FFF000;
  MPU_InitStruct.LimitAddress = 0x08FFFFFF;
  MPU_InitStruct.AttributesIndex = MPU_ATTRIBUTES_NUMBER0;
  MPU_InitStruct.AccessPermission = MPU_REGION_ALL_RO;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Attribute 0 and the memory to be protected
  */
  MPU_AttributesInit.Number = MPU_ATTRIBUTES_NUMBER0;
  MPU_AttributesInit.Attributes = INNER_OUTER(MPU_NOT_CACHEABLE);

  HAL_MPU_ConfigMemoryAttributes(&MPU_AttributesInit);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @param None
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  Safety_FaultFromException();
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
