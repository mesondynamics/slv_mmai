# Understanding a new machine integration

[Architecture](architecture.md) · [Interfaces](interfaces.md) · [Safety](SAFETY.md)

Begin with the machine's existing functions and control paths. Choose the
electrical interface after understanding the mechanical result it can produce.

## 1. Inventory the functions

List steering, propulsion, braking, direction/transmission, work actuators,
lights, horn, manual controls, interlocks, and sensors. Identify which functions
can already be controlled electrically and which would need an added actuator.

## 2. Make an interface table

Record one row per function. The examples below are conceptual, not wiring
instructions for a particular machine.

| Function | Possible interface | Useful feedback | Questions to resolve |
| --- | --- | --- | --- |
| Steering | Motor drive, CAN, electrohydraulic input | Angle/position, actuator state | Direction, travel limits, torque, manual coexistence |
| Propulsion | Proportional valve, analog command, CAN | Speed, current, pressure, drive state | Neutral, direction, braking interaction, loss of command |
| Auxiliary | Relay/contact, discrete output, CAN | Original switch or load state | Contact topology, polarity, load current, default state |

Distinguish a command interface from a measurement interface. A speed pulse is
feedback, not a speed command; a valve-current loop is not automatically a
vehicle-speed controller.

## 3. Define state and units

Write down physical units, ranges, signs, scaling, and freshness. Decide how
unknown or stale feedback is represented. Identify which original switches and
interlocks must be observed, and which outputs need independent confirmation.

## 4. Define each transition

Describe startup, enable, manual takeover, direction changes, stop, fault reset,
and communication loss. For every output, specify reset, power-loss, invalid-command,
watchdog, and emergency-stop states. Include retained energy and mechanical
effects in that reasoning.

## 5. Map the interface in code

Reuse command framing, validation, and state representation where they fit.
Keep machine wiring, raw bus messages, scaling, and calibration in identifiable
mapping code. A small explicit mapping is often easier to study than a new
framework. Follow the source paths in the [firmware guide](firmware.md).

## 6. Explore the physical behavior gradually

For an actual installation, start with isolated inputs and dummy loads. Move to
controlled, low-energy commissioning one actuator at a time. Establish the
low-level behavior before adding upper-level autonomy. Record the exact board,
wiring, load, and assumptions associated with any observation.

The source examples provide an engineering starting point; each machine still
needs its own electrical, hydraulic, mechanical, and safety decisions.
