# Hardware reading guide

[Project overview](../README.md) · [Firmware](firmware.md)

The board sources show how a controller is connected to existing electrical
actuators and sensors. Read each revision together with its schematic, connector
assignments, and firmware; component ratings alone are not assembled-board ratings.

## H563 actuation ECU

Open [PROJECT1.kicad_pro](../pcb/roller/ecu/PROJECT1.kicad_pro). The
[root schematic](../pcb/roller/ecu/PROJECT1.kicad_sch) links the functional sheets:

| Sheet | Circuit function |
| --- | --- |
| [PAGE1](../pcb/roller/ecu/PAGE1.kicad_sch) | Supply conversion and indicators |
| [PAGE2](../pcb/roller/ecu/PAGE2.kicad_sch) | STM32H563, clocks, reset, debug, and security device |
| [PAGE3](../pcb/roller/ecu/PAGE3.kicad_sch) | Ethernet PHY and physical interface |
| [PAGE4](../pcb/roller/ecu/PAGE4.kicad_sch) | Two CAN physical interfaces |
| [PAGE5](../pcb/roller/ecu/PAGE5.kicad_sch) | Serial low-side relay-driver chain |
| [PAGE6](../pcb/roller/ecu/PAGE6.kicad_sch) | Relay contacts and relay-supply storage |
| [PAGE7](../pcb/roller/ecu/PAGE7.kicad_sch) | Two PWM valve-drive channels with current sensing |
| [PAGE8](../pcb/roller/ecu/PAGE8.kicad_sch) | Analog, temperature, isolated digital, and pulse inputs |
| [PAGE9](../pcb/roller/ecu/PAGE9.kicad_sch) | Power entry, reverse-current/polarity protection, and connectors |

The proportional-valve example uses low-side MOSFET switching, recirculation
paths, and current measurement. Firmware closes the coil-current loop. The
hydraulic circuit determines what the resulting valve current does to the machine.

The relay chain and contact matrix provide several ways to connect existing
signals. A contact may interrupt an original path, select a replacement signal,
or switch a load. Relay numbers alone do not define a portable machine function.

The [ECU source notes](../pcb/roller/ecu/README.md) retain circuit-source mappings
and related retrofit examples. The English overview and original Chinese notes
are linked there.

## Design revisions

Read the current `PROJECT1` schematic alongside earlier ECU revisions in the Git
history. Changes to connector assignments, output circuits, or input conditioning
need to be understood in the context of their matching firmware.
[Provenance](PROVENANCE.md) identifies retained ECU development milestones.

## Reading a design for a different machine

For each connector and output, identify the supply, reference ground, current
path, protection components, expected load, feedback, and reset state. Pay
particular attention to inductive energy, transient protection, fusing, grounding,
and manual-control coexistence. Resolve the actual symbol, footprint, and 3D model
used by the selected revision.

Voltage/current limits and environmental capability require revision-specific
evidence. Treat an estimate as a design target, and a component datasheet limit
as a component limit. This repository does not turn either into a certified
machine-level specification.
