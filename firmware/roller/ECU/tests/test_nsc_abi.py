import importlib.util
from pathlib import Path
import sys
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = PROJECT_ROOT / "tools" / "check_nsc_abi.py"
SPEC = importlib.util.spec_from_file_location("check_nsc_abi", MODULE_PATH)
ABI = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ABI
SPEC.loader.exec_module(ABI)


def symbol(name, value, section="ABS"):
    return ABI.ElfSymbol(
        value=value,
        size=8,
        symbol_type="FUNC",
        binding="GLOBAL",
        visibility="DEFAULT",
        section_index=section,
        name=name,
    )


class NscAbiSourceTest(unittest.TestCase):
    def test_repository_v1_map_matches_firmware_1_0_12(self):
        source = (
            PROJECT_ROOT / "Secure_nsclib" / "secure_nsc_abi_v1.s"
        ).read_text(encoding="utf-8")
        ABI.validate_abi_source(source)
        self.assertEqual(
            ABI.parse_abi_source(source), ABI.NSC_ABI_V1_BY_NAME
        )

    def test_address_drift_is_rejected(self):
        source = (
            PROJECT_ROOT / "Secure_nsclib" / "secure_nsc_abi_v1.s"
        ).read_text(encoding="utf-8")
        altered = source.replace("0x0c05dc01", "0x0c05dcc1", 1)
        with self.assertRaisesRegex(
            ABI.AbiCheckError, "SECURE_SafetyKickWatchdog"
        ):
            ABI.validate_abi_source(altered)

    def test_missing_duplicate_and_extra_symbols_are_rejected(self):
        source = (
            PROJECT_ROOT / "Secure_nsclib" / "secure_nsc_abi_v1.s"
        ).read_text(encoding="utf-8")
        invocation = (
            "nsc_v1_symbol SECURE_SafetyKickWatchdog,              "
            "0x0c05dc01\n"
        )
        with self.assertRaisesRegex(ABI.AbiCheckError, "missing frozen"):
            ABI.validate_abi_source(source.replace(invocation, "", 1))
        with self.assertRaisesRegex(ABI.AbiCheckError, "duplicate frozen"):
            ABI.validate_abi_source(source + invocation)
        with self.assertRaisesRegex(ABI.AbiCheckError, "unexpected v1"):
            ABI.validate_abi_source(
                source + "nsc_v1_symbol SECURE_NewApi, 0x0c05dcc1\n"
            )


class NscAbiBinaryTest(unittest.TestCase):
    def setUp(self):
        self.import_symbols = [
            symbol(name, address)
            for name, address in ABI.NSC_ABI_V1_SYMBOLS
        ]
        self.sg_section = ABI.ElfSection(
            index=11,
            name=".gnu.sgstubs",
            address=0x0C05DC00,
            size=0x400,
        )
        self.elf_symbols = [
            symbol(name, address, section="11")
            for name, address in ABI.NSC_ABI_V1_SYMBOLS
        ]

    def test_valid_import_library_and_secure_elf(self):
        ABI.validate_import_library(self.import_symbols)
        ABI.validate_secure_elf(self.elf_symbols, [self.sg_section])
        ABI.validate_output_pair(
            self.import_symbols, self.elf_symbols, [self.sg_section]
        )
        ABI.validate_nonsecure_elf(self.import_symbols, self.import_symbols)

    def test_import_address_drift_is_rejected(self):
        changed = list(self.import_symbols)
        changed[0] = symbol(changed[0].name, changed[0].value + 8)
        with self.assertRaisesRegex(ABI.AbiCheckError, "expected 0x0c05dc01"):
            ABI.validate_import_library(changed)

    def test_elf_symbol_must_be_in_sg_section(self):
        changed = list(self.elf_symbols)
        changed[0] = symbol(changed[0].name, changed[0].value, section="2")
        with self.assertRaisesRegex(ABI.AbiCheckError, "gnu.sgstubs index 11"):
            ABI.validate_secure_elf(changed, [self.sg_section])

    def test_appended_veneers_must_match_both_outputs(self):
        appended_name = "SECURE_AppendedV2Service"
        imports = self.import_symbols + [
            symbol(appended_name, 0x0C05DCC1)
        ]
        definitions = self.elf_symbols + [
            symbol(appended_name, 0x0C05DCC1, section="11")
        ]
        ABI.validate_output_pair(imports, definitions, [self.sg_section])
        definitions[-1] = symbol(
            appended_name, 0x0C05DCE1, section="11"
        )
        with self.assertRaisesRegex(ABI.AbiCheckError, "differs between"):
            ABI.validate_output_pair(imports, definitions, [self.sg_section])

    def test_nonsecure_must_resolve_private_import_addresses(self):
        changed = list(self.import_symbols)
        changed[0] = symbol(changed[0].name, changed[0].value + 8)
        with self.assertRaisesRegex(ABI.AbiCheckError, "NonSecure"):
            ABI.validate_nonsecure_elf(self.import_symbols, changed)

    def test_readelf_parsers_preserve_thumb_bit_and_section(self):
        symbol_output = """
Symbol table '.symtab' contains 2 entries:
   Num:    Value  Size Type    Bind   Vis      Ndx Name
     1: 0c05dc01     8 FUNC    GLOBAL DEFAULT   11 SECURE_SafetyKickWatchdog
"""
        section_output = """
  [Nr] Name              Type            Address  Off    Size   ES Flg Lk Inf Al
  [11] .gnu.sgstubs      PROGBITS        0c05dc00 00cc00 000400 00  AX  0   0 32
"""
        parsed_symbols = ABI.parse_readelf_symbols(symbol_output)
        parsed_sections = ABI.parse_readelf_sections(section_output)
        self.assertEqual(parsed_symbols[0].value, 0x0C05DC01)
        self.assertEqual(parsed_symbols[0].section_index, "11")
        self.assertEqual(parsed_sections, [self.sg_section])


if __name__ == "__main__":
    unittest.main()
