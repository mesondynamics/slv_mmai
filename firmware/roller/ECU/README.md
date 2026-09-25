# STM32H563 actuation ECU firmware

[MMAI overview](../../../README.md) · [Firmware reading guide](../../../docs/firmware.md)

This project implements an actuation interface with Secure and NonSecure firmware
domains, an Ethernet command/state interface, relay outputs, proportional-valve
current control, and CAN-connected machine/actuator paths.

## Read the implementation

| Area | Entry point |
| --- | --- |
| Command framing and parsing | [ecu_protocol.h](NonSecure/App/Inc/ecu_protocol.h), [ecu_protocol.c](NonSecure/App/Src/ecu_protocol.c) |
| Network handling | [ecu_network.c](NonSecure/App/Src/ecu_network.c) |
| Protected interface | [safety_api.h](Secure_nsclib/safety_api.h) |
| Output policy and interlocks | [safety_service.c](Secure/App/Src/safety_service.c), [run_permit_policy.c](Secure/App/Src/run_permit_policy.c) |
| Valve-current loop | [valve_control.c](Secure/App/Src/valve_control.c) |
| Steering control | [steering_control.c](Secure/App/Src/steering_control.c), [steering_can.c](Secure/App/Src/steering_can.c) |
| Machine CAN feedback | [vehicle_can.c](Secure/App/Src/vehicle_can.c) |
| Boot/update layout | [ecu_flash_layout.h](Shared/ecu_flash_layout.h), [OEMiROT](Bootloader/OEMiROT/) |

Trace each requested output through the parser, authority/enable policy, machine
mapping, and driver. For feedback, trace the physical input through scaling,
validity/freshness, and state encoding. Manual run permission and automatic-output
enable are distinct decisions in this implementation.

## Design context

The project contains STM32CubeMX/CMake inputs, vendor support, host tools, and
historical tests. They describe a particular hardware generation and its original
engineering assumptions. Device profiles and wiring mappings are examples of
integration-specific configuration, not universal defaults.

The retained detailed notes include the original Chinese
[protocol and UI description](docs/ECU_Protocol_and_UI.md),
[boot/update design](docs/ECU_Security_and_OTA.md), and
[bench-method notes](docs/ECU_Bench_Acceptance.md). The English
[interface guide](../../../docs/interfaces.md) gives the main protocol boundaries.

Before using manufacturing, recovery, update, or control tools, understand their
physical effects and the target hardware. See [Safety](../../../docs/SAFETY.md).
