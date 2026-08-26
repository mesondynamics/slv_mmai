#ifndef SOFTWARE_I2C_H
#define SOFTWARE_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PCB revision 1 routes the ATECC608C clock/data nets to the opposite STM32
   I2C1 alternate-function pins.  PB8 is therefore SDA and PB9 is SCL in this
   temporary open-drain software implementation.  Restore hardware I2C after
   the next PCB ECO swaps the two nets. */
bool SoftwareI2C_Init(void);
bool SoftwareI2C_RecoverBus(void);
bool SoftwareI2C_Write(uint8_t address_7bit, const uint8_t *data, size_t length);
bool SoftwareI2C_Read(uint8_t address_7bit, uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* SOFTWARE_I2C_H */
