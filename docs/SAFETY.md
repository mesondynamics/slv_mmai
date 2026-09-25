# Safety

Mobile machinery can cause serious injury or death. This repository is
experimental engineering material and does not, by itself, constitute a
certified functional-safety system or establish suitability for road use.

Machine-specific hazard analysis and engineering review are needed before
changing steering, propulsion, braking, or original interlocks. The person
integrating the system must establish the appropriate physical protections and
operating procedures for that machine.

## Understand the complete stopping path

For every output, determine what occurs on reset, power loss, stale commands,
invalid input, communication failure, and emergency stop. An electrically
de-energized output is not automatically a safe mechanical state. Hydraulic
pressure, gravity, inertia, springs, and capacitors may retain energy after
power is removed.

A hardware emergency-stop path should not depend solely on application software
or network delivery. Its interaction with original manual controls, brakes, and
run permission needs an explicit machine-level design. A watchdog is one part
of that design, not a substitute for it.

## Explore at low energy

Use isolated signals, indicators, and suitable dummy loads to understand I/O
behavior before connecting machinery. Establish current limits, load protection,
grounding, and a means to remove energy. Progress from electrical observation to
one actuator at a time under controlled conditions, with an independent stop
available and people clear of motion.

Before relying on a port, exercise its command-loss, watchdog, output-disable,
manual-control, and fault-reset behavior on that installation. Previous code or
board evidence does not establish another machine's behavior. Example mappings
are not approval for unattended or safety-critical operation.
