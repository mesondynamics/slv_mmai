# Architecture

[Project overview](../README.md) · [Hardware](hardware.md) · [Firmware](firmware.md)

![Command and feedback paths](architecture.svg)

MMAI covers the boundary between an upper-level command source and the electrical
interfaces of an existing mobile machine. The controller may request steering,
propulsion, operating mode, or an auxiliary function. The machine interface
validates that request, decides whether it may take control, and maps it to the
available actuator connection. Feedback travels back through the interface.

## The three actuation paths

**Steering:** operate an electric steering actuator or another supported steering
input, then acquire the available angle, position, or actuator status. The H563
implementation includes a CAN steering-motor path; the physical mechanical
coupling remains specific to the machine.

**Propulsion:** translate a request into the input accepted by the existing drive.
One implementation controls the current through two proportional hydraulic-valve
coils. A different machine may expose an analog, relay, serial, or CAN drive input.
Valve current and vehicle speed are different quantities; the machine's hydraulic
response and feedback determine how one relates to the other.

**Auxiliaries:** operate relay contacts, discrete electrical signals, or a supported
CAN interface for functions such as lights, horn, or work equipment. Contact
selection, polarity, and coexistence with original switches are adapter choices.

## Commands and state

Keep the chain visible: receive a command, validate its framing and units, check
authority and enable conditions, apply machine mapping, then update the physical
output. In the other direction, sample an input, scale and validate it, record
its freshness, and encode the feedback. [Interfaces](interfaces.md) maps these
steps onto the existing source files.

The conceptual boundary is more general than the existing wire protocols. The
repository retains concrete protocols and machine mappings so their evolution
can be studied directly. A universal transport-neutral API is not assumed.

## Responsibility boundaries

| Layer | Responsibility |
| --- | --- |
| Upper-level controller | Decide intended motion or function; interpret returned state |
| Command/control policy | Validate requests, control authority, freshness, and mode transitions |
| Machine mapping | Interpret units, direction, output assignment, and machine interlocks |
| Electrical driver/input | Generate or acquire the actual voltage, current, contact, pulse, or bus message |
| Physical machine | Steering, propulsion, stored energy, original controls, and mechanical limits |

The H563 generation uses Secure and NonSecure firmware domains to separate
actuator ownership from network handling. This protected boundary is an
implementation choice. Other ports can use different MCUs or partitioning while
keeping command validation and physical output permission explicit.

Manual operation, command loss, emergency stop, and output defaults belong in the
machine design from the start. See [Safety](SAFETY.md) and the
[porting guide](porting-guide.md).
