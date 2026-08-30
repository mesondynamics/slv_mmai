/*
 * Roller ECU NSC ABI v3 import map.
 *
 * Include the immutable v2 map, then pin only the read-only J1939 snapshot
 * veneer. Future veneers must append after 0x0c05dcb1; no v1/v2 address may
 * move because field devices link their NonSecure image against this map.
 */

.include "secure_nsc_abi_v2.s"

nsc_v1_symbol SECURE_SafetyGetJ1939Snapshot,          0x0c05dcb1
