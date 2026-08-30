#ifndef SECURE_TIMEBASE_H
#define SECURE_TIMEBASE_H

#include <stdbool.h>
#include <stdint.h>

/* TIM7 is a Secure-only, free-running 1 MHz counter.  Every caller must keep
 * one measured interval below a counter wrap (65.536 ms). */
#define SECURE_TIMEBASE_COUNTER_HZ       1000000UL
#define SECURE_TIMEBASE_MAX_INTERVAL_US    60000UL

bool SecureTimebase_Init(void);
bool SecureTimebase_IsRunning(void);
uint16_t SecureTimebase_NowUs16(void);
bool SecureTimebase_HasElapsed(uint16_t start, uint32_t interval_us,
                               bool *elapsed);
bool SecureTimebase_DelayUs(uint32_t interval_us);

#endif /* SECURE_TIMEBASE_H */
