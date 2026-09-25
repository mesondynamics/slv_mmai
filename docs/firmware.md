# Firmware reading guide

[Project overview](../README.md) · [Interfaces](interfaces.md)

The useful reading unit is a complete signal path. Start with a command or
feedback field, then follow its validation, state, mapping, and physical interface.
The source retains the structure of the actuation ECU implementation.

## H563 ECU

The [ECU project](../firmware/roller/ECU/) separates Secure actuator functions
from NonSecure network/application functions.

| Responsibility | Source entry point |
| --- | --- |
| Wire formats and parsing | [ecu_protocol.h](../firmware/roller/ECU/NonSecure/App/Inc/ecu_protocol.h), [ecu_protocol.c](../firmware/roller/ECU/NonSecure/App/Src/ecu_protocol.c) |
| Network receive and control arbitration | [ecu_network.c](../firmware/roller/ECU/NonSecure/App/Src/ecu_network.c), [control_authority_policy.h](../firmware/roller/ECU/NonSecure/App/Network/control_authority_policy.h) |
| Application loop and heartbeat | [ecu_app.c](../firmware/roller/ECU/NonSecure/App/Src/ecu_app.c) |
| Shared Secure/NonSecure interface | [safety_api.h](../firmware/roller/ECU/Secure_nsclib/safety_api.h) |
| Output enable, interlocks, and faults | [safety_service.c](../firmware/roller/ECU/Secure/App/Src/safety_service.c), [run_permit_policy.c](../firmware/roller/ECU/Secure/App/Src/run_permit_policy.c) |
| Proportional-valve current loop | [valve_control.c](../firmware/roller/ECU/Secure/App/Src/valve_control.c) |
| Steering command and CAN path | [steering_control.c](../firmware/roller/ECU/Secure/App/Src/steering_control.c), [steering_can.c](../firmware/roller/ECU/Secure/App/Src/steering_can.c) |
| Machine-state CAN acquisition | [vehicle_can.c](../firmware/roller/ECU/Secure/App/Src/vehicle_can.c), [vehicle_j1939.c](../firmware/roller/ECU/Secure/App/Src/vehicle_j1939.c) |
| State assembly and speed input | [ecu_data_model.c](../firmware/roller/ECU/NonSecure/App/Src/ecu_data_model.c), [speed_sensor.c](../firmware/roller/ECU/NonSecure/App/Src/speed_sensor.c) |
| Boot/update memory boundaries | [ecu_flash_layout.h](../firmware/roller/ECU/Shared/ecu_flash_layout.h), [OEMiROT project](../firmware/roller/ECU/Bootloader/OEMiROT/) |

For propulsion, follow the requested coil current through command validation,
Secure output permission, current control, and PWM compare values. Current
feedback closes this electrical loop; it does not by itself establish vehicle
speed or stopping distance.

For steering, distinguish requested angle/rate, the mode the implementation
accepts, actuator enable, and returned actuator state. For relays, trace the
semantic command to the actual contact wiring. Those mappings are machine-specific.

The implementation distinguishes automatic-output shutdown from manual run
permission. An emergency-stop transition can have different consequences from an
ordinary stale-command transition. Read the policy and state together before
borrowing an isolated function.

## Feedback and upper-level integration

Follow physical inputs through the
[state model](../firmware/roller/ECU/NonSecure/App/Src/ecu_data_model.c) and
[speed acquisition](../firmware/roller/ECU/NonSecure/App/Src/speed_sensor.c).
The upper-level controller must interpret validity, freshness, units, and faults
alongside the returned values. The [interface guide](interfaces.md) describes the
command/state boundary without assuming a particular host framework.

## Tools and historical context

The projects include CMake/CubeMX inputs, host utilities, and tests. They are
part of the engineering record, with assumptions tied to their original
hardware and development environment. Read a tool before using it, especially
if it can write flash, change device lifecycle, update firmware, or operate outputs.
An example command or a historical result is not a deployment procedure for a
different board.
