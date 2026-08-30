#include "ecu_app.h"

#include "ecu_data_model.h"
#include "ecu_network.h"
#include "j1939.h"
#include "main.h"
#include "secure_nsc.h"
#include "speed_sensor.h"

#define ECU_WATCHDOG_SERVICE_PERIOD_MS        100U
#define ECU_OTA_CONFIRM_STARTUP_TIMEOUT_MS   5000U

static SAFETY_AdcSnapshot safety_snapshot;
static uint32_t watchdog_heartbeat;
static uint32_t watchdog_service_tick;
static uint32_t lwip_random_state = 0x8AEAB502UL;
#if defined(ECU_OEMIROT_LAYOUT)
static uint32_t ota_confirmation_deadline;
static uint8_t ota_confirmation_pending;
static uint8_t ota_confirmation_failed;
#endif

uint32_t ECU_LwipRandom(void)
{
  lwip_random_state = (lwip_random_state * 1664525UL) +
                      1013904223UL + HAL_GetTick();
  return lwip_random_state;
}

void ECU_LwipAssert(const char *file, uint32_t line)
{
  (void)file;
  (void)line;
  (void)SECURE_SafetyDisarmOutputs();
  __disable_irq();
  while (1)
  {
  }
}

int32_t ECU_AppInit(void)
{
  int32_t result;

  result = SECURE_SafetyGetAdcSnapshot(&safety_snapshot);
  if (result != SAFETY_RESULT_OK)
  {
    return result;
  }

  if (!ECU_DataModelInit())
  {
    (void)SECURE_SafetyDisarmOutputs();
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  if (!SpeedSensor_Init())
  {
    (void)SECURE_SafetyDisarmOutputs();
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
  /* CAN1 is a read-only telemetry domain. Its Secure health bit and J1939
     valid mask expose a local fault without blocking Ethernet diagnostics or
     OTA recovery on a vehicle with a disconnected/bus-off CAN harness. */
  (void)J1939_Init();
  if (!ECU_NetworkInit())
  {
    (void)SECURE_SafetyDisarmOutputs();
    return SAFETY_RESULT_INTERNAL_ERROR;
  }
#if defined(ECU_OEMIROT_LAYOUT)
  ota_confirmation_pending = 0U;
  ota_confirmation_failed = 0U;
  if ((SECURE_SafetyGetStatus() & SAFETY_STATUS_OTA_UNCONFIRMED) != 0U)
  {
    /* A test swap is accepted only after the fixed-address LAN8742 identity
       probe succeeds.  Cable/link presence is deliberately not required. */
    if (ECU_NetworkStartupReady())
    {
      result = SECURE_SafetyOtaConfirmRunningImages();
      if (result != SAFETY_RESULT_OK)
      {
        ECU_NetworkSetOperational(false);
        (void)SECURE_SafetyDisarmOutputs();
        return result;
      }
    }
    else
    {
      ota_confirmation_pending = 1U;
      ota_confirmation_deadline = HAL_GetTick() +
          ECU_OTA_CONFIRM_STARTUP_TIMEOUT_MS;
      ECU_NetworkSetOperational(false);
    }
  }
  if (ota_confirmation_pending == 0U)
  {
    ECU_NetworkSetOperational(true);
  }
#else
  ECU_NetworkSetOperational(true);
#endif

  watchdog_service_tick = HAL_GetTick();
  watchdog_heartbeat = 0U;
  return SECURE_SafetyKickWatchdog(watchdog_heartbeat);
}

void ECU_AppProcess(void)
{
  uint32_t now;

#if defined(ECU_OEMIROT_LAYOUT)
  if (ota_confirmation_failed != 0U)
  {
    return;
  }
#endif

  ECU_NetworkProcess();
  now = HAL_GetTick();

#if defined(ECU_OEMIROT_LAYOUT)
  if (ota_confirmation_pending != 0U)
  {
    if ((int32_t)(now - ota_confirmation_deadline) >= 0)
    {
      /* Stop servicing the IWDG so OEMiROT can revert an unconfirmed image.
         Already-confirmed images never enter this path. */
      ota_confirmation_failed = 1U;
      ECU_NetworkSetOperational(false);
      return;
    }
    if (ECU_NetworkStartupReady())
    {
      if (SECURE_SafetyOtaConfirmRunningImages() != SAFETY_RESULT_OK)
      {
        ota_confirmation_failed = 1U;
        ECU_NetworkSetOperational(false);
        return;
      }
      ota_confirmation_pending = 0U;
      ECU_NetworkSetOperational(true);
    }
  }
#endif

  if ((uint32_t)(now - watchdog_service_tick) >= ECU_WATCHDOG_SERVICE_PERIOD_MS)
  {
    watchdog_service_tick = now;
    ++watchdog_heartbeat;

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
