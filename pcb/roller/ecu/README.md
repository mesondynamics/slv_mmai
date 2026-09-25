# Actuation interface ECU hardware

**English** · [中文设计说明与完整参考资料](README.zh-CN.md)

This STM32H563-based board illustrates the electrical boundary between an
upper-level controller and existing machine actuators and sensors. Open
[PROJECT1.kicad_pro](PROJECT1.kicad_pro) and the
[root schematic](PROJECT1.kicad_sch) to explore the design.

The [hardware reading guide](../../../docs/hardware.md) maps the nine schematic
sheets to their functions. The [architecture](../../../docs/architecture.md)
places the board in the larger steering, propulsion, and auxiliary-I/O system.

## Design paths

- **Steering:** connect to a supported external actuator/controller; mechanical
  coupling and feedback depend on the machine.
- **Propulsion:** the reference circuit drives two proportional-valve coils with
  PWM and current sensing. The machine's hydraulic circuit determines how coil
  current affects propulsion.
- **Auxiliaries:** a serial driver chain and relay contacts can switch or select
  existing machine signals. Wiring and contact assignments remain machine-specific.
- **Feedback:** analog, temperature, isolated digital, pulse, and bus interfaces
  provide the inputs available to the selected integration.

## Circuit-source notes

The original [Chinese technical notes](README.zh-CN.md) include the detailed
PAGE1-PAGE9 circuit mapping, public source identifiers S01-S60, and illustrated
retrofit examples. Selected entry points are:

| Topic | Detailed notes |
| --- | --- |
| Steering actuators | [Motor/steering integration](README.zh-CN.md#steer-sys-01) |
| Proportional-valve drive | [PWM and current-sensing path](README.zh-CN.md#sol-os-01) |
| Relay/contact switching | [Relay interface path](README.zh-CN.md#rly-sys-01) |
| Solenoid reference circuit | [Analog Devices CN0415](README.zh-CN.md#s38) |
| Earlier hydraulic retrofit example | [CASE CX160 project reference](README.zh-CN.md#s60) |

These references explain particular design functions; they do not make all
machine interfaces interchangeable. Consult the selected board revision and
[safety guidance](../../../docs/SAFETY.md) before any physical integration.
