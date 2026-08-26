#include "ecu_app.h"

#include "main.h"
#include "secure_nsc.h"

#define ECU_WATCHDOG_SERVICE_PERIOD_MS  100U

static SAFETY_AdcSnapshot safety_snapshot;
static uint32_t watchdog_heartbeat;
static uint32_t watchdog_service_tick;

int32_t ECU_AppInit(void)
{
  int32_t result;

  /* The PHY is held in reset by the CubeMX GPIO initial state. Give its power
     rails time to settle before releasing reset. */
  HAL_Delay(10U);
  HAL_GPIO_WritePin(RMII_NRST_GPIO_Port, RMII_NRST_Pin, GPIO_PIN_SET);

  result = SECURE_SafetyGetAdcSnapshot(&safety_snapshot);
  if (result != SAFETY_RESULT_OK)
  {
    return result;
  }

  watchdog_service_tick = HAL_GetTick();
  watchdog_heartbeat = 0U;
  return SECURE_SafetyKickWatchdog(watchdog_heartbeat);
}

void ECU_AppProcess(void)
{
  uint32_t now = HAL_GetTick();

  if ((uint32_t)(now - watchdog_service_tick) >= ECU_WATCHDOG_SERVICE_PERIOD_MS)
  {
    watchdog_service_tick = now;
    ++watchdog_heartbeat;

    /* In the production application, advance this heartbeat only after all
       required control and communication task health checks have passed. */
    if (SECURE_SafetyGetAdcSnapshot(&safety_snapshot) != SAFETY_RESULT_OK)
    {
      (void)SECURE_SafetyDisarmOutputs();
      return;
    }
    if (SECURE_SafetyKickWatchdog(watchdog_heartbeat) != SAFETY_RESULT_OK)
    {
      (void)SECURE_SafetyDisarmOutputs();
    }
  }
}

const SAFETY_AdcSnapshot *ECU_AppGetSafetySnapshot(void)
{
  return &safety_snapshot;
}
