/*
 * Roller ECU NSC ABI v2 import map.
 *
 * Include the immutable firmware-1.0.12 v1 map, then pin only the steering
 * snapshot veneer appended by actuator API v3. Future veneers must append
 * after 0x0c05dca9; neither this address nor any v1 address may move.
 */

.include "secure_nsc_abi_v1.s"

nsc_v1_symbol SECURE_SafetyGetSteeringSnapshot,       0x0c05dca9
