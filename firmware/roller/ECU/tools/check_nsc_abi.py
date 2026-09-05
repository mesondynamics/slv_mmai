#!/usr/bin/env python3
"""Verify the frozen Roller ECU NSC ABI v1 across source and build outputs."""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Iterable


PROJECT_ROOT = Path(__file__).resolve().parents[1]

# Firmware 1.0.12 established these public veneer addresses.  Keep this map in
# the checker itself: deriving the expected values from secure_nsc_abi_v1.s
# would allow an accidental edit to that file to silently redefine the ABI.
NSC_ABI_V1_SYMBOLS = (
    ("SECURE_SafetyKickWatchdog", 0x0C05DC01),
    ("SECURE_SystemCoreClockUpdate", 0x0C05DC09),
    ("SECURE_SafetyReadValveTelemetry", 0x0C05DC11),
    ("SECURE_SafetyApplyValveConfig", 0x0C05DC19),
    ("SECURE_SafetyGetActuatorSnapshot", 0x0C05DC21),
    ("SECURE_SafetyOtaGetStatus", 0x0C05DC29),
    ("SECURE_SafetyOtaBegin", 0x0C05DC31),
    ("SECURE_SafetyDisarmOutputs", 0x0C05DC39),
    ("SECURE_RegisterCallback", 0x0C05DC41),
    ("SECURE_SafetyClearFault", 0x0C05DC49),
    ("SECURE_SafetyGetAdcSnapshot", 0x0C05DC51),
    ("SECURE_SafetyGetStatus", 0x0C05DC59),
    ("SECURE_SafetySubmitActuatorCommand", 0x0C05DC61),
    ("SECURE_SafetySaveValveConfig", 0x0C05DC69),
    ("SECURE_SafetyArmOutputs", 0x0C05DC71),
    ("SECURE_SafetyOtaWrite", 0x0C05DC79),
    ("SECURE_SafetyGetSecurityStatus", 0x0C05DC81),
    ("SECURE_SafetyOtaConfirmRunningImages", 0x0C05DC89),
    ("SECURE_SafetyGetValveConfig", 0x0C05DC91),
    ("SECURE_SafetyOtaFinish", 0x0C05DC99),
    ("SECURE_SafetyReloadValveConfig", 0x0C05DCA1),
)
NSC_ABI_V1_BY_NAME = dict(NSC_ABI_V1_SYMBOLS)
NSC_ABI_V2_ADDITIONS = (
    ("SECURE_SafetyGetSteeringSnapshot", 0x0C05DCA9),
)
NSC_ABI_V2_BY_NAME = dict(NSC_ABI_V2_ADDITIONS)
NSC_ABI_V3_ADDITIONS = (
    ("SECURE_SafetyGetJ1939Snapshot", 0x0C05DCB1),
)
NSC_ABI_V3_BY_NAME = dict(NSC_ABI_V3_ADDITIONS)
NSC_ABI_V4_ADDITIONS = (
    ("SECURE_SafetyAssertNetworkEStop", 0x0C05DCB9),
    ("SECURE_SafetyResetNetworkEStop", 0x0C05DCC1),
)
NSC_ABI_V4_BY_NAME = dict(NSC_ABI_V4_ADDITIONS)

_SOURCE_INVOCATION = re.compile(
    r"^\s*nsc_v1_symbol\s+([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"
    r"(0[xX][0-9A-Fa-f]+)\s*(?:(?:/\*.*\*/)|(?:@.*))?\s*$"
)
_READELF_SYMBOL = re.compile(
    r"^\s*\d+:\s+([0-9A-Fa-f]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+"
    r"(\S+)\s+(\S+)\s+(\S+)(?:\s+.*)?$"
)
_READELF_SECTION = re.compile(
    r"^\s*\[\s*(\d+)\]\s+(\S+)\s+\S+\s+([0-9A-Fa-f]+)\s+"
    r"[0-9A-Fa-f]+\s+([0-9A-Fa-f]+)\s+"
)


class AbiCheckError(RuntimeError):
    """A persistent NSC ABI invariant was violated."""


@dataclass(frozen=True)
class ElfSymbol:
    value: int
    size: int
    symbol_type: str
    binding: str
    visibility: str
    section_index: str
    name: str


@dataclass(frozen=True)
class ElfSection:
    index: int
    name: str
    address: int
    size: int


def _format_address(value: int) -> str:
    return f"0x{value:08x}"


def parse_abi_source(text: str) -> dict[str, int]:
    """Parse all nsc_v1_symbol invocations and reject malformed duplicates."""
    parsed: dict[str, int] = {}
    for line_number, line in enumerate(text.splitlines(), start=1):
        if re.match(r"^\s*nsc_v1_symbol\b", line) is None:
            continue
        match = _SOURCE_INVOCATION.fullmatch(line)
        if match is None:
            raise AbiCheckError(
                f"malformed nsc_v1_symbol invocation at line {line_number}"
            )
        name = match.group(1)
        if name in parsed:
            raise AbiCheckError(
                f"duplicate frozen ABI symbol {name} at line {line_number}"
            )
        parsed[name] = int(match.group(2), 16)
    return parsed


def validate_abi_source(text: str) -> None:
    parsed = parse_abi_source(text)
    missing = sorted(set(NSC_ABI_V1_BY_NAME) - set(parsed))
    extra = sorted(set(parsed) - set(NSC_ABI_V1_BY_NAME))
    errors: list[str] = []
    if missing:
        errors.append("missing frozen symbols: " + ", ".join(missing))
    if extra:
        errors.append("unexpected v1 symbols: " + ", ".join(extra))
    for name, expected in NSC_ABI_V1_SYMBOLS:
        actual = parsed.get(name)
        if actual is not None and actual != expected:
            errors.append(
                f"{name} is {_format_address(actual)}, expected "
                f"{_format_address(expected)}"
            )
    if errors:
        raise AbiCheckError("ABI source mismatch: " + "; ".join(errors))


def validate_abi_v2_source(text: str) -> None:
    """Validate the append-only v2 map without redefining the frozen v1 map."""
    parsed = parse_abi_source(text)
    errors: list[str] = []
    if text.count('.include "secure_nsc_abi_v1.s"') != 1:
        errors.append("v2 map must include secure_nsc_abi_v1.s exactly once")
    if parsed != NSC_ABI_V2_BY_NAME:
        errors.append(
            "v2 additions are " + repr(parsed) + ", expected " +
            repr(NSC_ABI_V2_BY_NAME)
        )
    if errors:
        raise AbiCheckError("ABI v2 source mismatch: " + "; ".join(errors))


def validate_abi_v3_source(text: str) -> None:
    """Validate the append-only v3 map without redefining v1 or v2."""
    parsed = parse_abi_source(text)
    errors: list[str] = []
    if text.count('.include "secure_nsc_abi_v2.s"') != 1:
        errors.append("v3 map must include secure_nsc_abi_v2.s exactly once")
    if parsed != NSC_ABI_V3_BY_NAME:
        errors.append(
            "v3 additions are " + repr(parsed) + ", expected " +
            repr(NSC_ABI_V3_BY_NAME)
        )
    if errors:
        raise AbiCheckError("ABI v3 source mismatch: " + "; ".join(errors))


def validate_abi_v4_source(text: str) -> None:
    """Validate the append-only v4 map without redefining v1 through v3."""
    parsed = parse_abi_source(text)
    errors: list[str] = []
    if text.count('.include "secure_nsc_abi_v3.s"') != 1:
        errors.append("v4 map must include secure_nsc_abi_v3.s exactly once")
    if parsed != NSC_ABI_V4_BY_NAME:
        errors.append(
            "v4 additions are " + repr(parsed) + ", expected " +
            repr(NSC_ABI_V4_BY_NAME)
        )
    if errors:
        raise AbiCheckError("ABI v4 source mismatch: " + "; ".join(errors))


def parse_readelf_symbols(text: str) -> list[ElfSymbol]:
    symbols: list[ElfSymbol] = []
    for line in text.splitlines():
        match = _READELF_SYMBOL.fullmatch(line)
        if match is None:
            continue
        symbols.append(
            ElfSymbol(
                value=int(match.group(1), 16),
                size=int(match.group(2), 10),
                symbol_type=match.group(3),
                binding=match.group(4),
                visibility=match.group(5),
                section_index=match.group(6),
                name=match.group(7),
            )
        )
    return symbols


def parse_readelf_sections(text: str) -> list[ElfSection]:
    sections: list[ElfSection] = []
    for line in text.splitlines():
        match = _READELF_SECTION.match(line)
        if match is None:
            continue
        sections.append(
            ElfSection(
                index=int(match.group(1), 10),
                name=match.group(2),
                address=int(match.group(3), 16),
                size=int(match.group(4), 16),
            )
        )
    return sections


def _symbols_by_name(
    symbols: Iterable[ElfSymbol],
) -> dict[str, list[ElfSymbol]]:
    result: dict[str, list[ElfSymbol]] = defaultdict(list)
    for symbol in symbols:
        result[symbol.name].append(symbol)
    return result


def _validate_common_symbol(
    symbol: ElfSymbol, name: str, expected_address: int, label: str
) -> list[str]:
    errors: list[str] = []
    if symbol.value != expected_address:
        errors.append(
            f"{label} {name} is {_format_address(symbol.value)}, expected "
            f"{_format_address(expected_address)}"
        )
    if symbol.size != 8:
        errors.append(f"{label} {name} size is {symbol.size}, expected 8")
    if symbol.symbol_type != "FUNC":
        errors.append(
            f"{label} {name} type is {symbol.symbol_type}, expected FUNC"
        )
    if symbol.binding != "GLOBAL":
        errors.append(
            f"{label} {name} binding is {symbol.binding}, expected GLOBAL"
        )
    if symbol.visibility != "DEFAULT":
        errors.append(
            f"{label} {name} visibility is {symbol.visibility}, expected DEFAULT"
        )
    return errors


def validate_import_library(symbols: Iterable[ElfSymbol]) -> None:
    by_name = _symbols_by_name(symbols)
    errors: list[str] = []
    for name, expected in NSC_ABI_V1_SYMBOLS:
        matches = by_name.get(name, [])
        if len(matches) != 1:
            errors.append(
                f"import library contains {len(matches)} definitions of {name}, "
                "expected 1"
            )
            continue
        symbol = matches[0]
        errors.extend(_validate_common_symbol(symbol, name, expected, "import"))
        if symbol.section_index != "ABS":
            errors.append(
                f"import {name} section is {symbol.section_index}, expected ABS"
            )
    if errors:
        raise AbiCheckError("; ".join(errors))


def validate_secure_elf(
    symbols: Iterable[ElfSymbol], sections: Iterable[ElfSection]
) -> None:
    by_name = _symbols_by_name(symbols)
    sg_sections = [section for section in sections if section.name == ".gnu.sgstubs"]
    if len(sg_sections) != 1:
        raise AbiCheckError(
            f"Secure ELF contains {len(sg_sections)} .gnu.sgstubs sections, expected 1"
        )
    sg_section = sg_sections[0]
    errors: list[str] = []
    for name, expected in NSC_ABI_V1_SYMBOLS:
        matches = by_name.get(name, [])
        if len(matches) != 1:
            errors.append(
                f"Secure ELF contains {len(matches)} definitions of {name}, expected 1"
            )
            continue
        symbol = matches[0]
        errors.extend(_validate_common_symbol(symbol, name, expected, "ELF"))
        if symbol.section_index != str(sg_section.index):
            errors.append(
                f"ELF {name} section is {symbol.section_index}, expected "
                f".gnu.sgstubs index {sg_section.index}"
            )
        code_address = symbol.value & ~1
        if not (
            sg_section.address
            <= code_address
            < sg_section.address + sg_section.size
        ):
            errors.append(
                f"ELF {name} is outside .gnu.sgstubs "
                f"[{_format_address(sg_section.address)}, "
                f"{_format_address(sg_section.address + sg_section.size)})"
            )
    if errors:
        raise AbiCheckError("; ".join(errors))


def validate_v2_additions(
    import_symbols: Iterable[ElfSymbol],
    elf_symbols: Iterable[ElfSymbol],
    sections: Iterable[ElfSection],
) -> None:
    """Pin every append-only v2 veneer in both signed linker outputs."""
    _validate_additions(
        NSC_ABI_V2_ADDITIONS, import_symbols, elf_symbols, sections
    )


def validate_v3_additions(
    import_symbols: Iterable[ElfSymbol],
    elf_symbols: Iterable[ElfSymbol],
    sections: Iterable[ElfSection],
) -> None:
    """Pin every append-only v3 veneer in both signed linker outputs."""
    _validate_additions(
        NSC_ABI_V3_ADDITIONS, import_symbols, elf_symbols, sections
    )


def validate_v4_additions(
    import_symbols: Iterable[ElfSymbol],
    elf_symbols: Iterable[ElfSymbol],
    sections: Iterable[ElfSection],
) -> None:
    """Pin every append-only v4 veneer in both signed linker outputs."""
    _validate_additions(
        NSC_ABI_V4_ADDITIONS, import_symbols, elf_symbols, sections
    )


def _validate_additions(
    additions: Iterable[tuple[str, int]],
    import_symbols: Iterable[ElfSymbol],
    elf_symbols: Iterable[ElfSymbol],
    sections: Iterable[ElfSection],
) -> None:
    import_by_name = _symbols_by_name(import_symbols)
    elf_by_name = _symbols_by_name(elf_symbols)
    sg_sections = [section for section in sections if section.name == ".gnu.sgstubs"]
    if len(sg_sections) != 1:
        raise AbiCheckError(
            f"Secure ELF contains {len(sg_sections)} .gnu.sgstubs sections, expected 1"
        )
    sg_section = sg_sections[0]
    errors: list[str] = []
    for name, expected in additions:
        imports = import_by_name.get(name, [])
        definitions = elf_by_name.get(name, [])
        if len(imports) != 1:
            errors.append(
                f"import library contains {len(imports)} definitions of {name}, expected 1"
            )
            continue
        if len(definitions) != 1:
            errors.append(
                f"Secure ELF contains {len(definitions)} definitions of {name}, expected 1"
            )
            continue
        imported = imports[0]
        defined = definitions[0]
        errors.extend(_validate_common_symbol(imported, name, expected, "import"))
        errors.extend(_validate_common_symbol(defined, name, expected, "ELF"))
        if imported.section_index != "ABS":
            errors.append(
                f"import {name} section is {imported.section_index}, expected ABS"
            )
        if defined.section_index != str(sg_section.index):
            errors.append(
                f"ELF {name} section is {defined.section_index}, expected "
                f".gnu.sgstubs index {sg_section.index}"
            )
        code_address = defined.value & ~1
        if not (
            sg_section.address <= code_address <
            sg_section.address + sg_section.size
        ):
            errors.append(f"ELF {name} is outside .gnu.sgstubs")
    if errors:
        raise AbiCheckError("; ".join(errors))


def validate_output_pair(
    import_symbols: Iterable[ElfSymbol],
    elf_symbols: Iterable[ElfSymbol],
    sections: Iterable[ElfSection],
) -> None:
    """Ensure newly appended veneers also agree across both linker outputs."""
    import_by_name = _symbols_by_name(import_symbols)
    elf_by_name = _symbols_by_name(elf_symbols)
    public_names = sorted(
        {
            name
            for name in set(import_by_name) | set(elf_by_name)
            if name.startswith("SECURE_")
        }
    )
    sg_sections = [section for section in sections if section.name == ".gnu.sgstubs"]
    if len(sg_sections) != 1:
        raise AbiCheckError(
            f"Secure ELF contains {len(sg_sections)} .gnu.sgstubs sections, expected 1"
        )
    sg_section = sg_sections[0]
    errors: list[str] = []
    for name in public_names:
        imports = import_by_name.get(name, [])
        definitions = elf_by_name.get(name, [])
        if len(imports) != 1:
            errors.append(
                f"import library contains {len(imports)} definitions of {name}, "
                "expected 1"
            )
            continue
        if len(definitions) != 1:
            errors.append(
                f"Secure ELF contains {len(definitions)} definitions of {name}, "
                "expected 1"
            )
            continue
        imported = imports[0]
        defined = definitions[0]
        if imported.value != defined.value:
            errors.append(
                f"{name} differs between import library "
                f"({_format_address(imported.value)}) and Secure ELF "
                f"({_format_address(defined.value)})"
            )
        for label, symbol in (("import", imported), ("ELF", defined)):
            errors.extend(
                _validate_common_symbol(symbol, name, symbol.value, label)
            )
        if imported.section_index != "ABS":
            errors.append(
                f"import {name} section is {imported.section_index}, expected ABS"
            )
        if defined.section_index != str(sg_section.index):
            errors.append(
                f"ELF {name} section is {defined.section_index}, expected "
                f".gnu.sgstubs index {sg_section.index}"
            )
        code_address = defined.value & ~1
        if not (
            sg_section.address
            <= code_address
            < sg_section.address + sg_section.size
        ):
            errors.append(f"ELF {name} is outside .gnu.sgstubs")
    if not public_names:
        errors.append("no public SECURE_ veneer symbols found in linker outputs")
    if errors:
        raise AbiCheckError("; ".join(errors))


def validate_nonsecure_elf(
    import_symbols: Iterable[ElfSymbol],
    nonsecure_symbols: Iterable[ElfSymbol],
) -> None:
    """Prove the final NonSecure link resolved every NSC symbol identically."""
    import_by_name = _symbols_by_name(import_symbols)
    nonsecure_by_name = _symbols_by_name(nonsecure_symbols)
    public_names = sorted(
        name for name in import_by_name if name.startswith("SECURE_")
    )
    errors: list[str] = []
    for name in public_names:
        imports = import_by_name[name]
        resolved = nonsecure_by_name.get(name, [])
        if len(imports) != 1 or len(resolved) != 1:
            errors.append(
                f"NonSecure/import definition count for {name} is "
                f"{len(resolved)}/{len(imports)}, expected 1/1"
            )
            continue
        imported = imports[0]
        linked = resolved[0]
        if linked.value != imported.value:
            errors.append(
                f"NonSecure {name} is {_format_address(linked.value)}, "
                f"import is {_format_address(imported.value)}"
            )
        errors.extend(
            _validate_common_symbol(linked, name, imported.value, "NonSecure")
        )
        if linked.section_index != "ABS":
            errors.append(
                f"NonSecure {name} section is {linked.section_index}, expected ABS"
            )
    unexpected = sorted(
        name for name in nonsecure_by_name
        if name.startswith("SECURE_") and name not in import_by_name
    )
    if unexpected:
        errors.append(
            "NonSecure contains SECURE_ symbols absent from import library: "
            + ", ".join(unexpected)
        )
    if errors:
        raise AbiCheckError("; ".join(errors))


def _run_readelf(readelf: str, option: str, path: Path) -> str:
    try:
        completed = subprocess.run(
            [readelf, option, str(path)],
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as exc:
        raise AbiCheckError(f"cannot execute {readelf}: {exc}") from exc
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise AbiCheckError(
            f"{readelf} {option} failed for {path}: {detail}"
        )
    return completed.stdout


def _default_readelf() -> str:
    configured_dir = os.environ.get("ARM_GNU_BIN_DIR")
    if configured_dir:
        return str(Path(configured_dir) / "arm-none-eabi-readelf")
    discovered = shutil.which("arm-none-eabi-readelf")
    if discovered:
        return discovered
    return "arm-none-eabi-readelf"


def check_files(
    abi_source: Path, abi_v2_source: Path, abi_v3_source: Path,
    abi_v4_source: Path,
    import_library: Path, secure_elf: Path, readelf: str,
    nonsecure_elf: Path | None = None,
) -> None:
    for label, path in (
        ("ABI source", abi_source),
        ("ABI v2 source", abi_v2_source),
        ("ABI v3 source", abi_v3_source),
        ("ABI v4 source", abi_v4_source),
        ("import library", import_library),
        ("Secure ELF", secure_elf),
    ):
        if not path.is_file():
            raise AbiCheckError(f"{label} not found: {path}")

    try:
        source_text = abi_source.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise AbiCheckError(f"cannot read ABI source {abi_source}: {exc}") from exc
    validate_abi_source(source_text)
    try:
        v2_source_text = abi_v2_source.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise AbiCheckError(
            f"cannot read ABI v2 source {abi_v2_source}: {exc}"
        ) from exc
    validate_abi_v2_source(v2_source_text)
    try:
        v3_source_text = abi_v3_source.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise AbiCheckError(
            f"cannot read ABI v3 source {abi_v3_source}: {exc}"
        ) from exc
    validate_abi_v3_source(v3_source_text)
    try:
        v4_source_text = abi_v4_source.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise AbiCheckError(
            f"cannot read ABI v4 source {abi_v4_source}: {exc}"
        ) from exc
    validate_abi_v4_source(v4_source_text)

    import_symbols = parse_readelf_symbols(
        _run_readelf(readelf, "-sW", import_library)
    )
    validate_import_library(import_symbols)

    elf_symbols = parse_readelf_symbols(_run_readelf(readelf, "-sW", secure_elf))
    elf_sections = parse_readelf_sections(
        _run_readelf(readelf, "-SW", secure_elf)
    )
    validate_secure_elf(elf_symbols, elf_sections)
    validate_v2_additions(import_symbols, elf_symbols, elf_sections)
    validate_v3_additions(import_symbols, elf_symbols, elf_sections)
    validate_v4_additions(import_symbols, elf_symbols, elf_sections)
    validate_output_pair(import_symbols, elf_symbols, elf_sections)
    if nonsecure_elf is not None:
        if not nonsecure_elf.is_file():
            raise AbiCheckError(f"NonSecure ELF not found: {nonsecure_elf}")
        nonsecure_symbols = parse_readelf_symbols(
            _run_readelf(readelf, "-sW", nonsecure_elf)
        )
        validate_nonsecure_elf(import_symbols, nonsecure_symbols)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Verify firmware 1.0.12 NSC ABI v1 addresses in the frozen map, "
            "OEMiROT import library, and Secure ELF"
        )
    )
    parser.add_argument(
        "--abi-source",
        type=Path,
        default=PROJECT_ROOT / "Secure_nsclib" / "secure_nsc_abi_v1.s",
    )
    parser.add_argument(
        "--abi-v2-source",
        type=Path,
        default=PROJECT_ROOT / "Secure_nsclib" / "secure_nsc_abi_v2.s",
    )
    parser.add_argument(
        "--abi-v3-source",
        type=Path,
        default=PROJECT_ROOT / "Secure_nsclib" / "secure_nsc_abi_v3.s",
    )
    parser.add_argument(
        "--abi-v4-source",
        type=Path,
        default=PROJECT_ROOT / "Secure_nsclib" / "secure_nsc_abi_v4.s",
    )
    parser.add_argument(
        "--import-library",
        type=Path,
        help=(
            "OEMiROT import object; defaults to secure_nsclib.o beside "
            "--secure-elf"
        ),
    )
    parser.add_argument("--secure-elf", type=Path, required=True)
    parser.add_argument("--nonsecure-elf", type=Path)
    parser.add_argument("--readelf", default=_default_readelf())
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_argument_parser().parse_args(argv)
    import_library = (
        args.import_library
        if args.import_library is not None
        else args.secure_elf.parent / "secure_nsclib.o"
    )
    try:
        check_files(
            args.abi_source,
            args.abi_v2_source,
            args.abi_v3_source,
            args.abi_v4_source,
            import_library,
            args.secure_elf,
            args.readelf,
            args.nonsecure_elf,
        )
    except AbiCheckError as exc:
        print(f"NSC ABI check failed: {exc}", file=sys.stderr)
        return 1
    print(
        "NSC ABI check passed: 21 firmware 1.0.12 v1 veneers and the "
        "actuator-v3 steering, read-only J1939, and network E-stop veneers are stable in source, import library, "
        "Secure ELF, and paired NonSecure ELF when supplied."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
