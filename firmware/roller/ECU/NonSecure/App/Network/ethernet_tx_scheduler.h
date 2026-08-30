#ifndef ECU_ETHERNET_TX_SCHEDULER_H
#define ECU_ETHERNET_TX_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

/* Ordered from highest to lowest transmit priority. */
typedef enum
{
  ETHERNET_TX_CATEGORY_STATUS = 0,
  ETHERNET_TX_CATEGORY_STEERING,
  ETHERNET_TX_CATEGORY_DIAGNOSTIC,
  ETHERNET_TX_CATEGORY_SECURITY,
  ETHERNET_TX_CATEGORY_LEGACY,
  ETHERNET_TX_CATEGORY_TELEMETRY,
  ETHERNET_TX_CATEGORY_COUNT,
  ETHERNET_TX_CATEGORY_NONE = 0xFF
} EthernetTxCategory;

typedef struct
{
  uint32_t period_ms;
  uint32_t phase_ms;
} EthernetTxScheduleConfig;

typedef struct
{
  uint32_t period_ms[ETHERNET_TX_CATEGORY_COUNT];
  uint32_t next_deadline_ms[ETHERNET_TX_CATEGORY_COUNT];
  uint32_t pending_deadline_ms[ETHERNET_TX_CATEGORY_COUNT];
  uint32_t last_dispatch_ms;
  uint8_t pending_mask;
  bool dispatch_recorded;
  bool initialized;
} EthernetTxScheduler;

/* phase_ms is the first deadline's offset from now_ms and must be less than
 * period_ms.  Periods must fit in INT32_MAX so signed-delta tick comparisons
 * remain unambiguous across uint32_t HAL tick wrap. */
bool EthernetTxScheduler_Init(
    EthernetTxScheduler *scheduler,
    uint32_t now_ms,
    const EthernetTxScheduleConfig config[ETHERNET_TX_CATEGORY_COUNT]);

/* Call at least once per INT32_MAX milliseconds.  Deadlines stay locked to
 * their initial absolute phase; missed periods are collapsed to one event. */
void EthernetTxScheduler_Update(EthernetTxScheduler *scheduler,
                                uint32_t now_ms,
                                bool telemetry_active);

/* Returns at most one category for a given now_ms value. */
EthernetTxCategory EthernetTxScheduler_Pop(EthernetTxScheduler *scheduler,
                                           uint32_t now_ms);

#endif /* ECU_ETHERNET_TX_SCHEDULER_H */
