/*
 * Frozen Roller ECU NSC ABI v1 import map.
 *
 * GNU ld consumes this object through --in-implib. Existing SG veneer
 * addresses therefore cannot move when Secure code is rebuilt or a new
 * service is appended. Values include the Thumb bit, matching the import
 * library shipped with firmware 1.0.12.
 */

.syntax unified
.thumb

.macro nsc_v1_symbol name, address
  .global \name
  .type \name, %function
  .set \name, \address
  .size \name, 8
.endm

nsc_v1_symbol SECURE_SafetyKickWatchdog,              0x0c05dc01
nsc_v1_symbol SECURE_SystemCoreClockUpdate,           0x0c05dc09
nsc_v1_symbol SECURE_SafetyReadValveTelemetry,        0x0c05dc11
nsc_v1_symbol SECURE_SafetyApplyValveConfig,          0x0c05dc19
nsc_v1_symbol SECURE_SafetyGetActuatorSnapshot,       0x0c05dc21
nsc_v1_symbol SECURE_SafetyOtaGetStatus,              0x0c05dc29
nsc_v1_symbol SECURE_SafetyOtaBegin,                  0x0c05dc31
nsc_v1_symbol SECURE_SafetyDisarmOutputs,             0x0c05dc39
nsc_v1_symbol SECURE_RegisterCallback,                0x0c05dc41
nsc_v1_symbol SECURE_SafetyClearFault,                0x0c05dc49
nsc_v1_symbol SECURE_SafetyGetAdcSnapshot,            0x0c05dc51
nsc_v1_symbol SECURE_SafetyGetStatus,                 0x0c05dc59
nsc_v1_symbol SECURE_SafetySubmitActuatorCommand,     0x0c05dc61
nsc_v1_symbol SECURE_SafetySaveValveConfig,           0x0c05dc69
nsc_v1_symbol SECURE_SafetyArmOutputs,                0x0c05dc71
nsc_v1_symbol SECURE_SafetyOtaWrite,                  0x0c05dc79
nsc_v1_symbol SECURE_SafetyGetSecurityStatus,         0x0c05dc81
nsc_v1_symbol SECURE_SafetyOtaConfirmRunningImages,   0x0c05dc89
nsc_v1_symbol SECURE_SafetyGetValveConfig,            0x0c05dc91
nsc_v1_symbol SECURE_SafetyOtaFinish,                 0x0c05dc99
nsc_v1_symbol SECURE_SafetyReloadValveConfig,         0x0c05dca1

.section .note.GNU-stack,"",%progbits
