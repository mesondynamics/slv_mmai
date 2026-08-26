#ifndef ECU_SPEED_SENSOR_H
#define ECU_SPEED_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

#define SPEED_SENSOR_PULSES_PER_KILOMETER 20000UL
#define SPEED_SENSOR_TIMEOUT_MS          1500UL

typedef struct
{
  uint16_t speed_centi_kph;
  uint16_t frequency_centi_hz;
  bool signal_present;
} SpeedSensorMeasurement;

bool SpeedSensor_Init(void);
void SpeedSensor_GetMeasurement(SpeedSensorMeasurement *measurement);

#endif /* ECU_SPEED_SENSOR_H */
