# Contributing

MMAI shares the engineering ideas, source, and development history of mobile-machine
actuation interfaces. Useful contributions include clearer explanations, corrected
source references, understandable machine adapters, and documented design changes.

- Explain the concrete function or design decision a change addresses.
- Keep schematic revisions, units, signal names, and code paths traceable.
- Preserve third-party notices and credit sources. Contribute only material you
  own or are entitled to redistribute.
- Keep machine-specific wiring, bus mappings, and calibration identifiable.
- Describe measurements with their actual board, load, and conditions. Distinguish
  a design target from an observation.
- Use English for new technical documentation. Keep the English and Chinese front
  READMEs consistent when changing their shared content.
- Use portable POSIX `sh` for new or revised shell scripts. Avoid unnecessary
  rewrites of historical source purely for style.

Historical code and tests preserve earlier assumptions and approaches. A focused
contribution need not modernize every generation. If a change deliberately alters
physical behavior, explain that behavior and the evidence supporting it.

See [Safety](docs/SAFETY.md) and [Security](SECURITY.md).
