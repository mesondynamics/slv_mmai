/*
 * Roller ECU NSC ABI v4 import map.
 *
 * Include the immutable v3 map, then append the Secure-owned network E-stop
 * assert/reset transaction.  Existing field firmware addresses never move.
 */

.include "secure_nsc_abi_v3.s"

nsc_v1_symbol SECURE_SafetyAssertNetworkEStop,        0x0c05dcb9
nsc_v1_symbol SECURE_SafetyResetNetworkEStop,         0x0c05dcc1
