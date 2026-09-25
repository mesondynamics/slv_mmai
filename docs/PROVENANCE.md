# Project provenance and development lineage

[Project overview](../README.md) · [Related engineering work](PRIOR_ART.md)

MMAI continues the mobile-machine interface work developed in the `mesondynamics`
repository lineage. Earlier work included street-sweeper development; this tree
documents the actuation ECU path for mobile machinery.
The recurring approach is to connect existing actuators and sensors through an
understandable electrical interface while keeping upper-level control separate.

## Source milestones

These are source-history markers. Dates are recorded Git author dates, and the
commit identifiers refer to this repository's history. A repository's initial
date does not imply that every later circuit or subsystem existed at that time.

| Date | Commit | Source milestone |
| --- | --- | --- |
| 2024-12-26 | `8027500` | Repository initialization and earlier engineering/template material |
| 2024-12-26 | `ef54d0d` | Initial repository/submodule setup |
| 2026-07-22 | `7406ac9` | ECU firmware organized under `firmware/roller/ECU` |
| 2026-07-29 | `03fa8d0` | ECU KiCad design introduced at `pcb/roller/ecu` |
| 2026-09-14 | `ac20bfa` | Abstract hardware project introduced |
| 2026-09-24 | `a8c1d1b` | Expanded documentation of retrofit paths and implementation choices |

For example, read a milestone with `git show 03fa8d0`, or follow the hardware
history with `git log --follow -- pcb/roller/ecu/PROJECT1.kicad_sch`. Earlier paths
can be inspected directly in the corresponding revision.

## ECU development and reusable ideas

The H563 ECU concentrates actuator control, electrical feedback, and a protected
firmware boundary in a dedicated interface controller. Retained revisions show
the development of its hardware and firmware.

The reusable ideas are command framing, validation, feedback acquisition, output
mapping, control freshness, and machine-specific interlocks. Relay assignments, actuator bus
messages, geometry, scaling, and calibration belong to a particular integration;
they should be distinguished from the general architecture.

## Attribution

The development history retains the author identities **Onicc**, **Yuan Mei**,
and **Chandra Tsai**. Existing source notices identify additional component
authors and licensors. Git authorship, circuit revision identifiers, and external
technical references serve different purposes; preserve each in its context.

The [hardware guide](hardware.md) and [firmware guide](firmware.md) connect these
milestones to the current source locations. Public technical precedents are
collected separately in [Prior art and related systems](PRIOR_ART.md).
