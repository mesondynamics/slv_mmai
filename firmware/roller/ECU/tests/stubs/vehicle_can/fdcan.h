#ifndef TEST_VEHICLE_CAN_FDCAN_H
#define TEST_VEHICLE_CAN_FDCAN_H

#include <stdint.h>

typedef enum
{
  HAL_OK = 0,
  HAL_ERROR = 1
} HAL_StatusTypeDef;

typedef struct
{
  uint32_t CCCR;
} FDCAN_GlobalTypeDef;

typedef struct
{
  FDCAN_GlobalTypeDef *Instance;
  uint32_t ErrorCode;
} FDCAN_HandleTypeDef;

typedef struct
{
  uint32_t IdType;
  uint32_t FilterIndex;
  uint32_t FilterType;
  uint32_t FilterConfig;
  uint32_t FilterID1;
  uint32_t FilterID2;
} FDCAN_FilterTypeDef;

typedef struct
{
  uint32_t BusOff;
  uint32_t ErrorPassive;
  uint32_t Warning;
} FDCAN_ProtocolStatusTypeDef;

typedef struct
{
  uint32_t Identifier;
  uint32_t IdType;
  uint32_t RxFrameType;
  uint32_t DataLength;
  uint32_t FDFormat;
} FDCAN_RxHeaderTypeDef;

extern FDCAN_GlobalTypeDef test_fdcan1_instance;
extern FDCAN_HandleTypeDef hfdcan1;
extern uint32_t test_primask;

#define FDCAN1                         (&test_fdcan1_instance)
#define FDCAN_CCCR_INIT                (1UL << 0)
#define FDCAN_EXTENDED_ID              1U
#define FDCAN_FILTER_MASK              2U
#define FDCAN_FILTER_TO_RXFIFO0        3U
#define FDCAN_REJECT                   4U
#define FDCAN_REJECT_REMOTE            5U
#define FDCAN_INTERRUPT_LINE0          0U
#define FDCAN_RX_FIFO0                 0U
#define FDCAN_DATA_FRAME               0U
#define FDCAN_CLASSIC_CAN              0U
#define FDCAN_DLC_BYTES_8              8U

#define FDCAN_IT_RX_FIFO0_NEW_MESSAGE  (1UL << 0)
#define FDCAN_IT_RX_FIFO0_FULL         (1UL << 1)
#define FDCAN_IT_RX_FIFO0_MESSAGE_LOST (1UL << 2)
#define FDCAN_IT_ERROR_WARNING         (1UL << 3)
#define FDCAN_IT_ERROR_PASSIVE         (1UL << 4)
#define FDCAN_IT_BUS_OFF               (1UL << 5)
#define FDCAN_IT_ARB_PROTOCOL_ERROR    (1UL << 6)
#define FDCAN_IT_DATA_PROTOCOL_ERROR   (1UL << 7)
#define FDCAN_IT_RAM_ACCESS_FAILURE    (1UL << 8)
#define FDCAN_IT_RAM_WATCHDOG          (1UL << 9)
#define FDCAN_IT_LIST_RX_FIFO0         (1UL << 10)
#define FDCAN_IT_LIST_BIT_LINE_ERROR   (1UL << 11)
#define FDCAN_IT_LIST_PROTOCOL_ERROR   (1UL << 12)

#define HAL_FDCAN_ERROR_NONE           0U
#define HAL_FDCAN_ERROR_RAM_ACCESS     (1UL << 16)
#define HAL_FDCAN_ERROR_PROTOCOL_ARBT  (1UL << 17)
#define HAL_FDCAN_ERROR_PROTOCOL_DATA  (1UL << 18)
#define HAL_FDCAN_ERROR_RAM_WDG        (1UL << 19)
#define HAL_FDCAN_ERROR_RESERVED_AREA  (1UL << 20)

#define CLEAR_BIT(REG, BIT) ((REG) &= ~(BIT))
#define __DMB() ((void)0)

static inline uint32_t __get_PRIMASK(void)
{
  return test_primask;
}

static inline void __disable_irq(void)
{
  test_primask = 1U;
}

static inline void __enable_irq(void)
{
  test_primask = 0U;
}

HAL_StatusTypeDef HAL_FDCAN_ConfigFilter(
    FDCAN_HandleTypeDef *hfdcan, const FDCAN_FilterTypeDef *filter);
HAL_StatusTypeDef HAL_FDCAN_ConfigGlobalFilter(
    FDCAN_HandleTypeDef *hfdcan, uint32_t nonmatching_standard,
    uint32_t nonmatching_extended, uint32_t reject_standard_remote,
    uint32_t reject_extended_remote);
HAL_StatusTypeDef HAL_FDCAN_ConfigInterruptLines(
    FDCAN_HandleTypeDef *hfdcan, uint32_t interrupt_list,
    uint32_t interrupt_line);
HAL_StatusTypeDef HAL_FDCAN_ActivateNotification(
    FDCAN_HandleTypeDef *hfdcan, uint32_t active_it,
    uint32_t buffer_indexes);
HAL_StatusTypeDef HAL_FDCAN_Start(FDCAN_HandleTypeDef *hfdcan);
HAL_StatusTypeDef HAL_FDCAN_GetProtocolStatus(
    const FDCAN_HandleTypeDef *hfdcan,
    FDCAN_ProtocolStatusTypeDef *protocol_status);
uint32_t HAL_FDCAN_GetRxFifoFillLevel(
    const FDCAN_HandleTypeDef *hfdcan, uint32_t rx_fifo);
HAL_StatusTypeDef HAL_FDCAN_GetRxMessage(
    FDCAN_HandleTypeDef *hfdcan, uint32_t rx_location,
    FDCAN_RxHeaderTypeDef *rx_header, uint8_t *rx_data);

#endif /* TEST_VEHICLE_CAN_FDCAN_H */
