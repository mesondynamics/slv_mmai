#ifndef ECU_J1939_H
#define ECU_J1939_H

#include <stdbool.h>
#include <stdint.h>

#define J1939_PGN_EEC1    0xF004UL
#define J1939_PGN_EEC2    0xF003UL
#define J1939_PGN_ET1     0xFEEEUL
#define J1939_PGN_EFL_P1  0xFEEFUL
#define J1939_PGN_AMB     0xFEF5UL
#define J1939_PGN_DM1     0xFECAUL
#define J1939_DATA_TIMEOUT_MS 1000UL

typedef struct
{
  uint16_t engine_rpm;
  uint8_t engine_torque_percent;
  uint8_t driver_demand_percent;
  uint8_t accelerator_pedal_percent;
  uint8_t engine_load_percent;
  int16_t coolant_temp_cdeg;
  int16_t fuel_temp_cdeg;
  uint16_t oil_pressure_kpa;
  uint16_t fuel_pressure_kpa;
  int16_t ambient_temp_cdeg;
  uint8_t dtc_count;
  bool eec1_valid;
  bool eec2_valid;
  bool et1_valid;
  bool eflp1_valid;
  bool amb_valid;
  bool dm1_valid;
  uint32_t eec1_timestamp;
  uint32_t eec2_timestamp;
  uint32_t et1_timestamp;
  uint32_t eflp1_timestamp;
  uint32_t amb_timestamp;
  uint32_t dm1_timestamp;
} J1939Data;

bool J1939_Init(void);
void J1939_Poll(void);
void J1939_GetData(J1939Data *data);

#endif /* ECU_J1939_H */
