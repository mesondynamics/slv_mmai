#ifndef ECU_NETWORK_H
#define ECU_NETWORK_H

#include <stdbool.h>
#include <stdint.h>

#define ECU_IP_ADDRESS_0       172U
#define ECU_IP_ADDRESS_1        16U
#define ECU_IP_ADDRESS_2         0U
#define ECU_IP_ADDRESS_3        11U
#define ECU_STATUS_PORT       50001U
#define ECU_CONTROL_PORT      50002U
#define ECU_DIAGNOSTIC_PORT   50003U
#define ECU_TELEMETRY_PORT    50004U
#define ECU_TUNING_PORT       50005U
#define ECU_OTA_PORT          50006U

typedef struct
{
  uint32_t valid_control_frames;
  uint32_t invalid_control_frames;
  uint32_t rejected_control_frames;
  uint32_t authority_switches;
  uint32_t status_frames_sent;
  uint32_t diagnostic_frames_sent;
  uint32_t legacy_v1_frames_rejected;
  uint32_t telemetry_frames_sent;
  uint32_t telemetry_dropped_samples;
} ECU_NetworkCounters;

bool ECU_NetworkInit(void);
void ECU_NetworkProcess(void);
bool ECU_NetworkLinkIsUp(void);
const ECU_NetworkCounters *ECU_NetworkGetCounters(void);

#endif /* ECU_NETWORK_H */
