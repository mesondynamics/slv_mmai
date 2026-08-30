#include "ethernet_tx_scheduler.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static bool EthernetTxScheduler_DeadlineReached(uint32_t now_ms,
                                                uint32_t deadline_ms)
{
  return (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool EthernetTxScheduler_EventIsStale(uint32_t now_ms,
                                             uint32_t event_deadline_ms,
                                             uint32_t period_ms)
{
  int32_t age_ms = (int32_t)(now_ms - event_deadline_ms);

  return (age_ms >= 0) && ((uint32_t)age_ms > (period_ms / 2U));
}

bool EthernetTxScheduler_Init(
    EthernetTxScheduler *scheduler,
    uint32_t now_ms,
    const EthernetTxScheduleConfig config[ETHERNET_TX_CATEGORY_COUNT])
{
  uint32_t category;

  if (scheduler == NULL)
  {
    return false;
  }

  /* A failed reconfiguration must fail closed instead of leaving an older
     schedule (and possibly an older pending transmission) active. */
  (void)memset(scheduler, 0, sizeof(*scheduler));
  if (config == NULL)
  {
    return false;
  }

  for (category = 0U; category < ETHERNET_TX_CATEGORY_COUNT; ++category)
  {
    if ((config[category].period_ms == 0U) ||
        (config[category].period_ms > (uint32_t)INT32_MAX) ||
        (config[category].phase_ms >= config[category].period_ms))
    {
      return false;
    }
  }

  for (category = 0U; category < ETHERNET_TX_CATEGORY_COUNT; ++category)
  {
    scheduler->period_ms[category] = config[category].period_ms;
    scheduler->next_deadline_ms[category] =
        now_ms + config[category].phase_ms;
    scheduler->pending_deadline_ms[category] = 0U;
  }
  scheduler->initialized = true;
  return true;
}

void EthernetTxScheduler_Update(EthernetTxScheduler *scheduler,
                                uint32_t now_ms,
                                bool telemetry_active)
{
  uint32_t category;

  if ((scheduler == NULL) || !scheduler->initialized)
  {
    return;
  }

  for (category = 0U; category < ETHERNET_TX_CATEGORY_COUNT; ++category)
  {
    uint8_t category_mask = (uint8_t)(1U << category);
    uint32_t period_ms = scheduler->period_ms[category];
    uint32_t deadline_ms = scheduler->next_deadline_ms[category];

    if (EthernetTxScheduler_DeadlineReached(now_ms, deadline_ms))
    {
      uint32_t elapsed_ms = now_ms - deadline_ms;
      uint32_t elapsed_periods = elapsed_ms / period_ms;
      uint32_t latest_deadline_ms =
          deadline_ms + (elapsed_periods * period_ms);

      scheduler->next_deadline_ms[category] =
          latest_deadline_ms + period_ms;
      scheduler->pending_deadline_ms[category] = latest_deadline_ms;
      if (EthernetTxScheduler_EventIsStale(now_ms,
                                           latest_deadline_ms,
                                           period_ms))
      {
        scheduler->pending_mask &= (uint8_t)~category_mask;
      }
      else
      {
        scheduler->pending_mask |= category_mask;
      }
    }
    else if (((scheduler->pending_mask & category_mask) != 0U) &&
             EthernetTxScheduler_EventIsStale(
                 now_ms,
                 scheduler->pending_deadline_ms[category],
                 period_ms))
    {
      scheduler->pending_mask &= (uint8_t)~category_mask;
    }

    if ((category == (uint32_t)ETHERNET_TX_CATEGORY_TELEMETRY) &&
        !telemetry_active)
    {
      scheduler->pending_mask &= (uint8_t)~category_mask;
    }
  }
}

EthernetTxCategory EthernetTxScheduler_Pop(EthernetTxScheduler *scheduler,
                                           uint32_t now_ms)
{
  uint32_t category;

  if ((scheduler == NULL) || !scheduler->initialized ||
      (scheduler->dispatch_recorded &&
       (scheduler->last_dispatch_ms == now_ms)))
  {
    return ETHERNET_TX_CATEGORY_NONE;
  }

  for (category = 0U; category < ETHERNET_TX_CATEGORY_COUNT; ++category)
  {
    uint8_t category_mask = (uint8_t)(1U << category);

    if ((scheduler->pending_mask & category_mask) == 0U)
    {
      continue;
    }
    if (EthernetTxScheduler_EventIsStale(
            now_ms,
            scheduler->pending_deadline_ms[category],
            scheduler->period_ms[category]))
    {
      scheduler->pending_mask &= (uint8_t)~category_mask;
      continue;
    }

    scheduler->pending_mask &= (uint8_t)~category_mask;
    scheduler->last_dispatch_ms = now_ms;
    scheduler->dispatch_recorded = true;
    return (EthernetTxCategory)category;
  }

  return ETHERNET_TX_CATEGORY_NONE;
}
