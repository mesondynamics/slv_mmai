# Command, output, and feedback interfaces

[Architecture](architecture.md) · [Firmware source map](firmware.md)

MMAI describes a boundary, not a mandatory bus or wire format. A port should make
the following information explicit:

| Direction | Information | Mapping questions |
| --- | --- | --- |
| Upper layer to interface | Steering/propulsion request, auxiliary state, mode, enable, stop, heartbeat | What are the units, limits, freshness rule, and control authority? |
| Interface to machine | Relay/contact state, PWM/current, analog command, CAN or serial message | Which physical connection is affected, and what happens when control is lost? |
| Machine to interface | Position, speed, switch state, current, pressure, actuator or bus status | Is the measurement valid, fresh, scaled, and attributable to the intended input? |
| Interface to upper layer | Machine state, fault state, watchdog/enable state, interface health | Can the upper layer distinguish unknown state from a real zero? |

## H563 reference protocol

The source defines an Ethernet/UDP V2 frame with a 24-byte header, an `ECU2` magic
value, version/type, sizes, flags, sequence, timestamp, and CRC32C. The declarations
and serialization code are authoritative for the selected revision:
[header](../firmware/roller/ECU/NonSecure/App/Inc/ecu_protocol.h) and
[implementation](../firmware/roller/ECU/NonSecure/App/Src/ecu_protocol.c).

Message families include control commands, status/diagnostics, valve and steering
telemetry, valve configuration, and OTA transport. Existing control fields also
retain concrete machine functions. Separate those functions from the reusable
framing and validation logic when studying or adapting the protocol.

The V1/A5 compatibility format is distinct. It has its own framing, lengths,
units, and accepted operations; a legacy percentage command cannot be assumed
equivalent to a coil-current request. Use the actual parser and validation policy
instead of casting a host structure onto a network packet.

The shared [safety API](../firmware/roller/ECU/Secure_nsclib/safety_api.h) describes
the protected internal boundary. Numeric limits in that header are firmware
configuration choices, not guarantees of physical actuator capability.

## Machine adapters

The reference design's two CAN paths have different roles: machine-state
acquisition and steering-actuator control. Relay contact assignments, direction
mapping, scaling, and calibration belong to the machine integration. A public
CAN standard does not make a particular machine's command acceptance universal.

For another machine, document each function's command representation, physical
connection, feedback, neutral state, enable conditions, timeout behavior, and
manual-control interaction. Keep the mapping understandable as a table before
introducing abstraction in code.

## Upper-level integration

The host controller must implement the selected ECU wire contract, including
framing, units, state validity, and control freshness. Treat each protocol revision
as a specific interface contract rather than mixing message IDs or command units
across versions. Host frameworks remain outside the actuation-interface boundary.
