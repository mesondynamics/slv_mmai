# JLCPCB 4-layer fabrication profile

This project uses a 1.6002 mm, 4-layer stackup:

- F.Cu / In1.Cu / In2.Cu / B.Cu
- 2 oz outer copper and 1 oz inner copper
- ENIG finish, with vias tented on both sides

The configured DRC limits are deliberately more conservative than JLCPCB's current multilayer minimum capability:

| Check | Project rule |
| --- | --- |
| Track width and copper clearance | 0.20 mm |
| Copper to routed board edge | 0.50 mm |
| Hole to copper clearance | 0.28 mm |
| Through-hole diameter | 0.25 mm |
| Via diameter / drill | 0.60 / 0.30 mm |
| Via annular ring | 0.15 mm |
| Solder-mask web | 0.20 mm |
| Silkscreen clearance / text | 0.15 mm / 1.0 mm |

Before submitting to JLCPCB:

1. Select a 4-layer, 1.6 mm board with 2 oz outer copper, 1 oz inner copper, ENIG finish, and tented vias to match the PCB stackup.
2. Run ERC and DRC with no unreviewed errors. Resolve silkscreen warnings or accept the configured mask subtraction where appropriate.
3. Plot Gerbers and drill files to `output/JLC_4L`; the project creates Gerber attributes and a job file.
4. Include a fabrication note when ordering if U4's POFV footprint requires epoxy-filled and capped vias.
