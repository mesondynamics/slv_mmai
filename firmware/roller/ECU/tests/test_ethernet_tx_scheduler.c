#include "ethernet_tx_scheduler.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

static void SetAllConfigs(EthernetTxScheduleConfig *config,
                          uint32_t period_ms,
                          uint32_t phase_ms)
{
  uint32_t category;

  for (category = 0U; category < ETHERNET_TX_CATEGORY_COUNT; ++category)
  {
    config[category].period_ms = period_ms;
    config[category].phase_ms = phase_ms;
  }
}

static void TestFixedAbsolutePhase(void)
{
  EthernetTxScheduler scheduler;
  EthernetTxScheduleConfig config[ETHERNET_TX_CATEGORY_COUNT];

  SetAllConfigs(config, 1000U, 900U);
  config[ETHERNET_TX_CATEGORY_STATUS].period_ms = 10U;
  config[ETHERNET_TX_CATEGORY_STATUS].phase_ms = 2U;
  assert(EthernetTxScheduler_Init(&scheduler, 100U, config));

  EthernetTxScheduler_Update(&scheduler, 101U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 101U) ==
         ETHERNET_TX_CATEGORY_NONE);

  /* Dispatching the 102 ms event late must not move the next event to 115. */
  EthernetTxScheduler_Update(&scheduler, 105U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 105U) ==
         ETHERNET_TX_CATEGORY_STATUS);
  EthernetTxScheduler_Update(&scheduler, 111U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 111U) ==
         ETHERNET_TX_CATEGORY_NONE);
  EthernetTxScheduler_Update(&scheduler, 112U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 112U) ==
         ETHERNET_TX_CATEGORY_STATUS);
}

static void TestPriorityAndOneDispatchPerMillisecond(void)
{
  EthernetTxScheduler scheduler;
  EthernetTxScheduleConfig config[ETHERNET_TX_CATEGORY_COUNT];
  uint32_t category;

  SetAllConfigs(config, 100U, 0U);
  assert(EthernetTxScheduler_Init(&scheduler, 0U, config));
  EthernetTxScheduler_Update(&scheduler, 225U, true);

  /* A >100 ms jump collapses missed events.  Repeated Pop at one HAL tick can
     still dispatch only the highest-priority category. */
  assert(EthernetTxScheduler_Pop(&scheduler, 225U) ==
         ETHERNET_TX_CATEGORY_STATUS);
  assert(EthernetTxScheduler_Pop(&scheduler, 225U) ==
         ETHERNET_TX_CATEGORY_NONE);

  for (category = (uint32_t)ETHERNET_TX_CATEGORY_STEERING;
       category < ETHERNET_TX_CATEGORY_COUNT;
       ++category)
  {
    assert(EthernetTxScheduler_Pop(&scheduler, 225U + category) ==
           (EthernetTxCategory)category);
  }
}

static void TestUint32Wrap(void)
{
  EthernetTxScheduler scheduler;
  EthernetTxScheduleConfig config[ETHERNET_TX_CATEGORY_COUNT];
  uint32_t start_ms = UINT32_MAX - 3U;

  SetAllConfigs(config, 1000U, 900U);
  config[ETHERNET_TX_CATEGORY_STATUS].period_ms = 10U;
  config[ETHERNET_TX_CATEGORY_STATUS].phase_ms = 2U;
  assert(EthernetTxScheduler_Init(&scheduler, start_ms, config));

  EthernetTxScheduler_Update(&scheduler, UINT32_MAX - 1U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, UINT32_MAX - 1U) ==
         ETHERNET_TX_CATEGORY_STATUS);
  EthernetTxScheduler_Update(&scheduler, 7U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 7U) ==
         ETHERNET_TX_CATEGORY_NONE);
  EthernetTxScheduler_Update(&scheduler, 8U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 8U) ==
         ETHERNET_TX_CATEGORY_STATUS);
}

static void TestStaleDropAvoidsNearNeighborBurst(void)
{
  EthernetTxScheduler scheduler;
  EthernetTxScheduleConfig config[ETHERNET_TX_CATEGORY_COUNT];

  SetAllConfigs(config, 1000U, 900U);
  config[ETHERNET_TX_CATEGORY_STATUS].period_ms = 10U;
  config[ETHERNET_TX_CATEGORY_STATUS].phase_ms = 0U;
  assert(EthernetTxScheduler_Init(&scheduler, 100U, config));

  /* The latest missed event is nine milliseconds old, so it is discarded
     rather than being sent immediately before the fresh 110 ms event. */
  EthernetTxScheduler_Update(&scheduler, 109U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 109U) ==
         ETHERNET_TX_CATEGORY_NONE);
  EthernetTxScheduler_Update(&scheduler, 110U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 110U) ==
         ETHERNET_TX_CATEGORY_STATUS);
  EthernetTxScheduler_Update(&scheduler, 111U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 111U) ==
         ETHERNET_TX_CATEGORY_NONE);

  /* A once-fresh pending event also expires while it waits behind traffic. */
  assert(EthernetTxScheduler_Init(&scheduler, 200U, config));
  EthernetTxScheduler_Update(&scheduler, 200U, false);
  EthernetTxScheduler_Update(&scheduler, 206U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 206U) ==
         ETHERNET_TX_CATEGORY_NONE);
}

static void TestTelemetryGate(void)
{
  EthernetTxScheduler scheduler;
  EthernetTxScheduleConfig config[ETHERNET_TX_CATEGORY_COUNT];

  SetAllConfigs(config, 1000U, 900U);
  config[ETHERNET_TX_CATEGORY_TELEMETRY].period_ms = 10U;
  config[ETHERNET_TX_CATEGORY_TELEMETRY].phase_ms = 0U;
  assert(EthernetTxScheduler_Init(&scheduler, 100U, config));

  EthernetTxScheduler_Update(&scheduler, 100U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 100U) ==
         ETHERNET_TX_CATEGORY_NONE);
  EthernetTxScheduler_Update(&scheduler, 110U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 110U) ==
         ETHERNET_TX_CATEGORY_NONE);
  EthernetTxScheduler_Update(&scheduler, 120U, true);
  assert(EthernetTxScheduler_Pop(&scheduler, 120U) ==
         ETHERNET_TX_CATEGORY_TELEMETRY);
}

static void TestProductionPhasesSurviveLongLoopStall(void)
{
  EthernetTxScheduler scheduler;
  EthernetTxScheduleConfig config[ETHERNET_TX_CATEGORY_COUNT] = {
    [ETHERNET_TX_CATEGORY_STATUS] = {50U, 0U},
    [ETHERNET_TX_CATEGORY_STEERING] = {100U, 10U},
    [ETHERNET_TX_CATEGORY_DIAGNOSTIC] = {100U, 35U},
    [ETHERNET_TX_CATEGORY_SECURITY] = {100U, 60U},
    [ETHERNET_TX_CATEGORY_LEGACY] = {50U, 25U},
    [ETHERNET_TX_CATEGORY_TELEMETRY] = {8U, 0U}
  };

  assert(EthernetTxScheduler_Init(&scheduler, 1000U, config));
  EthernetTxScheduler_Update(&scheduler, 1000U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 1000U) ==
         ETHERNET_TX_CATEGORY_STATUS);
  EthernetTxScheduler_Update(&scheduler, 1010U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 1010U) ==
         ETHERNET_TX_CATEGORY_STEERING);
  EthernetTxScheduler_Update(&scheduler, 1025U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 1025U) ==
         ETHERNET_TX_CATEGORY_LEGACY);
  EthernetTxScheduler_Update(&scheduler, 1035U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 1035U) ==
         ETHERNET_TX_CATEGORY_DIAGNOSTIC);

  /* A 92 ms main-loop stall drops the stale 1100 ms status event, keeps the
     still-fresh 1110/1125 events, and never moves the 1150 ms status phase. */
  EthernetTxScheduler_Update(&scheduler, 1127U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 1127U) ==
         ETHERNET_TX_CATEGORY_STEERING);
  assert(EthernetTxScheduler_Pop(&scheduler, 1127U) ==
         ETHERNET_TX_CATEGORY_NONE);
  EthernetTxScheduler_Update(&scheduler, 1128U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 1128U) ==
         ETHERNET_TX_CATEGORY_LEGACY);
  EthernetTxScheduler_Update(&scheduler, 1135U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 1135U) ==
         ETHERNET_TX_CATEGORY_DIAGNOSTIC);
  EthernetTxScheduler_Update(&scheduler, 1150U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 1150U) ==
         ETHERNET_TX_CATEGORY_STATUS);
  EthernetTxScheduler_Update(&scheduler, 1160U, false);
  assert(EthernetTxScheduler_Pop(&scheduler, 1160U) ==
         ETHERNET_TX_CATEGORY_SECURITY);
}

static void TestInvalidConfigs(void)
{
  EthernetTxScheduler scheduler;
  EthernetTxScheduleConfig config[ETHERNET_TX_CATEGORY_COUNT];

  SetAllConfigs(config, 10U, 0U);
  assert(!EthernetTxScheduler_Init(NULL, 0U, config));
  assert(!EthernetTxScheduler_Init(&scheduler, 0U, NULL));

  assert(EthernetTxScheduler_Init(&scheduler, 0U, config));
  EthernetTxScheduler_Update(&scheduler, 0U, true);
  config[ETHERNET_TX_CATEGORY_STATUS].period_ms = 0U;
  assert(!EthernetTxScheduler_Init(&scheduler, 0U, config));
  assert(!scheduler.initialized);
  assert(scheduler.pending_mask == 0U);
  assert(EthernetTxScheduler_Pop(&scheduler, 0U) ==
         ETHERNET_TX_CATEGORY_NONE);
  config[ETHERNET_TX_CATEGORY_STATUS].period_ms = 10U;
  config[ETHERNET_TX_CATEGORY_STATUS].phase_ms = 10U;
  assert(!EthernetTxScheduler_Init(&scheduler, 0U, config));
  config[ETHERNET_TX_CATEGORY_STATUS].period_ms =
      (uint32_t)INT32_MAX + 1U;
  config[ETHERNET_TX_CATEGORY_STATUS].phase_ms = 0U;
  assert(!EthernetTxScheduler_Init(&scheduler, 0U, config));
}

int main(void)
{
  TestFixedAbsolutePhase();
  TestPriorityAndOneDispatchPerMillisecond();
  TestUint32Wrap();
  TestStaleDropAvoidsNearNeighborBurst();
  TestTelemetryGate();
  TestProductionPhasesSurviveLongLoopStall();
  TestInvalidConfigs();
  return 0;
}
