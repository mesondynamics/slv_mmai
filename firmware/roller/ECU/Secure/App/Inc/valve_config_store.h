#ifndef VALVE_CONFIG_STORE_H
#define VALVE_CONFIG_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "safety_api.h"

bool ValveConfigStore_Load(SAFETY_ValveConfig *config, uint32_t *generation,
                           uint32_t *crc32c);
bool ValveConfigStore_Save(const SAFETY_ValveConfig *config,
                           uint32_t generation, uint32_t *crc32c);
uint32_t ValveConfigStore_Crc32c(const void *data, uint32_t length);

#endif /* VALVE_CONFIG_STORE_H */
