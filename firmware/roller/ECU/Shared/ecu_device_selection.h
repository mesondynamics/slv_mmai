#ifndef ECU_DEVICE_SELECTION_H
#define ECU_DEVICE_SELECTION_H

/* Persistent project selection, outside CubeMX-generated files.
 * 1 = SN-EJAHGJI (.11/.12); 2 = SN-EJAHGJQ (.21/.22).
 * A command-line ECU_DEVICE_ID definition may select a separate build.
 * Never use a different device's bootloader or factory recovery backup.
 */
#ifndef ECU_DEVICE_ID
#define ECU_DEVICE_ID 2
#endif

#endif
