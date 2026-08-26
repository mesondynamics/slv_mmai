#ifndef SAFETY_API_H
#define SAFETY_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAFETY_SLOW_ADC_COUNT       9U
#define SAFETY_CURRENT_ADC_COUNT    2U
#define SAFETY_PWM_PERIOD_COUNTS    6250U
#define SAFETY_PWM_MAX_COMPARE      (SAFETY_PWM_PERIOD_COUNTS - 1U)

#define SAFETY_ARM_TOKEN            0x41524D21UL /* "ARM!" */
#define SAFETY_CLEAR_FAULT_TOKEN    0x434C5246UL /* "CLRF" */

enum
{
  SAFETY_STATUS_READY            = (1UL << 0),
  SAFETY_STATUS_ESTOP_ACTIVE     = (1UL << 1),
  SAFETY_STATUS_FAULT_LATCHED    = (1UL << 2),
  SAFETY_STATUS_OUTPUTS_ARMED    = (1UL << 3),
  SAFETY_STATUS_ADC_RUNNING      = (1UL << 4),
  SAFETY_STATUS_GTZC_VIOLATION   = (1UL << 5),
  SAFETY_STATUS_INTERNAL_ERROR   = (1UL << 6)
};

typedef enum
{
  SAFETY_RESULT_OK              = 0,
  SAFETY_RESULT_BAD_ARGUMENT    = -1,
  SAFETY_RESULT_NOT_READY       = -2,
  SAFETY_RESULT_ESTOP_ACTIVE    = -3,
  SAFETY_RESULT_FAULT_LATCHED   = -4,
  SAFETY_RESULT_NOT_ARMED       = -5,
  SAFETY_RESULT_RANGE           = -6,
  SAFETY_RESULT_DIRECTION       = -7,
  SAFETY_RESULT_STALE_SEQUENCE  = -8,
  SAFETY_RESULT_INTERNAL_ERROR  = -9
} SAFETY_Result;

typedef struct
{
  uint32_t status;
  uint32_t slow_sequence;
  uint32_t current_sequence;
  uint16_t slow_adc[SAFETY_SLOW_ADC_COUNT];
  uint16_t current_adc[SAFETY_CURRENT_ADC_COUNT];
  uint16_t reserved;
} SAFETY_AdcSnapshot;

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_API_H */
