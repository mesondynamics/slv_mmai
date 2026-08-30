import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
import zipfile
from dataclasses import asdict
from pathlib import Path


PROJECT = Path(__file__).resolve().parents[1]
TOOLS = PROJECT / "tools"
CREATE = TOOLS / "create_full_regression_recovery.sh"
CONSUMER = TOOLS / "provision_oemirot_open.sh"
RELEASE_OPEN = (
    PROJECT / "Bootloader" / "OEMiROT" / "build" / "ReleaseOpen" /
    "ECU_OEMiROT.bin"
)
RELEASE_CLOSED = (
    PROJECT / "Bootloader" / "OEMiROT" / "build" / "ReleaseClosed" /
    "ECU_OEMiROT.bin"
)
FIRMWARE = PROJECT / "artifacts" / "firmware" / "1.0.15"
SECURE_INITIAL = FIRMWARE / "secure-initial.bin"
NONSECURE_INITIAL = FIRMWARE / "nonsecure-initial.bin"
OTA_PACKAGE = FIRMWARE / "roller-ecu-1.0.15.recu"
PROBE = "066BFF565456857187210935"
HISTORICAL_OPEN_LOADER_1_0_14_OTA_SHA256 = (
    "a052f51a7ef14bbbec57f412950edad7f44f92a2323227d74c606d8dd866324d"
)

sys.path.insert(0, str(TOOLS))
import verify_closed_preflight as closed  # noqa: E402
import verify_open_loader_backup as backup  # noqa: E402
import verify_pairing_store as pairing  # noqa: E402


EXPECTED_ARTIFACTS = {
    RELEASE_OPEN: (52276,
        "5fdaaa3f5e2d2a6c01ed527282ba08b3d8d5e579c61ec3c4e097cdf2c0fd7761"),
    RELEASE_CLOSED: (52292,
        "4073a9faa15eed7ca6014f168ae91efc35ba1e26d8e74651552c9a0a3d8ef884"),
    SECURE_INITIAL: (196608,
        "1a50b9106f798fb64c1a766d81d194b8829e07497941b4af7efe41bb38b98d75"),
    NONSECURE_INITIAL: (327680,
        "4f96d510f06cda3de1a88de8be5a7516b09f1ed539b05e0d6be2eeaaa571d8ed"),
    OTA_PACKAGE: (525371,
        "9980acbf248f242c77c7044f48d627ca862d6714221f5835c0a21b7a24bf9e54"),
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def physical_uid_bytes():
    words = [int(closed.EXPECTED_MCU_UID[index:index + 8], 16)
             for index in range(0, 24, 8)]
    return struct.pack("<III", *words)


def installed_slot(name, initial, image_index):
    with zipfile.ZipFile(OTA_PACKAGE) as archive:
        slot = bytearray(archive.read(f"{name}.bin"))
    header_size = struct.unpack_from("<H", slot, 8)[0]
    protected_size = struct.unpack_from("<H", slot, 10)[0]
    image_size = struct.unpack_from("<I", slot, 12)[0]
    payload_end = header_size + image_size
    slot[header_size:payload_end] = initial[header_size:payload_end]
    unprotected = payload_end + protected_size
    record_size = unprotected + struct.unpack_from("<H", slot, unprotected + 2)[0]
    trailer = len(slot) - backup.BOOT_TRAILER_SIZE
    status_count = ((record_size + backup.SCRATCH_SIZE - 1) //
                    backup.SCRATCH_SIZE) * backup.BOOT_STATUS_STATE_COUNT
    for entry in range(status_count):
        start = trailer + entry * backup.BOOT_MAX_ALIGN
        slot[start:start + backup.BOOT_MAX_ALIGN] = \
            bytes((entry % backup.BOOT_STATUS_STATE_COUNT + 1,)) + b"\xff" * 15
    status_size = (backup.BOOT_STATUS_MAX_ENTRIES *
                   backup.BOOT_STATUS_STATE_COUNT * backup.BOOT_MAX_ALIGN)
    slot[trailer + status_size:trailer + status_size +
         2 * backup.BOOT_ENC_KEY_SIZE] = bytes.fromhex(
             backup.PINNED_ENCRYPTED_KEY_AREAS[name])
    slot[-backup.SWAP_SIZE_OFFSET_FROM_END:
         -backup.SWAP_SIZE_OFFSET_FROM_END + 16] = \
        struct.pack("<I", record_size) + b"\xff" * 12
    slot[-backup.SWAP_INFO_OFFSET_FROM_END:
         -backup.SWAP_INFO_OFFSET_FROM_END + 16] = \
        bytes(((image_index << 4) | 2,)) + b"\xff" * 15
    slot[-backup.COPY_DONE_OFFSET_FROM_END:
         -backup.COPY_DONE_OFFSET_FROM_END + 16] = b"\x01" + b"\xff" * 15
    slot[-backup.IMAGE_OK_OFFSET_FROM_END:
         -backup.IMAGE_OK_OFFSET_FROM_END + 16] = b"\x01" + b"\xff" * 15
    return bytes(slot)


def pairing_store():
    record = bytearray(b"\xff" * pairing.RECORD_SIZE)
    struct.pack_into(
        "<4I", record, 0, pairing.MAGIC, pairing.SCHEMA,
        pairing.EXPECTED_GENERATION, pairing.LAYOUT_VERSION)
    struct.pack_into("<3I", record, 16, *pairing.EXPECTED_MCU_UID)
    struct.pack_into("<I", record, 28, pairing.EXPECTED_CONFIG_CRC32C)
    record[32:41] = pairing.EXPECTED_SERIAL
    record[41:45] = pairing.EXPECTED_REVISION
    record[45:48] = bytes((pairing.EXPECTED_I2C_ADDRESS,
                           pairing.EXPECTED_PRIVATE_KEY_SLOT, 0))
    record[48:112] = pairing.EXPECTED_PUBLIC_KEY
    struct.pack_into("<I", record, 112, pairing.RECORD_SIZE)
    struct.pack_into("<I", record, 116, pairing.crc32c(record[:116]))
    struct.pack_into("<I", record, 128, pairing.COMMIT)
    sector = bytes(record) + b"\xff" * (pairing.SECTOR_SIZE - len(record))
    store = sector + sector
    pairing.verify_dual_store(store)
    return store


def create_closed_transaction(root):
    root.mkdir()
    flash = bytearray(b"\xff" * backup.FLASH_SIZE)
    secure = installed_slot("secure", SECURE_INITIAL.read_bytes(), 0)
    nonsecure = installed_slot("nonsecure", NONSECURE_INITIAL.read_bytes(), 1)
    flash[backup.SECURE_OFFSET:backup.SECURE_OFFSET + backup.SECURE_SIZE] = secure
    flash[backup.NONSECURE_OFFSET:
          backup.NONSECURE_OFFSET + backup.NONSECURE_SIZE] = nonsecure
    paired = pairing_store()
    flash[0xE0000:0xE0000 + len(paired)] = paired
    secure_report, nonsecure_report, _, _ = backup.validate_installed_flash(
        bytes(flash), FIRMWARE)
    audit = {
        "schema": "roller-ecu-installed-primary-audit-v1",
        "release_identity": asdict(secure_report.identity),
        "images": {
            "secure": asdict(secure_report),
            "nonsecure": asdict(nonsecure_report),
        },
        "installed_format": {
            "format": "mcuboot-scratch-swap-decrypted-payload-encrypted-header-v1",
            "release_artifacts": {
                name: {"size": size, "sha256": digest}
                for name, (size, digest) in backup.PINNED_RELEASE_ARTIFACTS.items()
            },
        },
    }

    content = {
        "ReleaseClosed-ECU_OEMiROT.bin": RELEASE_CLOSED.read_bytes(),
        "DA_Config.obk": b"D" * 108,
        "OEMiRoT_Config.obk": b"C" * 300,
        "OEMiRoT_Data.obk": b"O" * 204,
        "security-assets.json": b"{}\n",
        "cert-root.b64": b"root\n",
        "cert-intermediate.b64": b"intermediate\n",
        "cert-leaf.b64": b"leaf\n",
        "cert-leaf-chain.b64": b"chain\n",
        "oemirot-auth-s-public.pem": b"secure-public\n",
        "oemirot-auth-ns-public.pem": b"nonsecure-public\n",
        "option-bytes-before.txt": (
            b"Device ID : 0x484\nPRODUCT_STATE : 0xED (Open)\n"
        ),
        "full-flash.bin": bytes(flash),
        "secure-persistent.bin": bytes(flash[0xE0000:0x100000]),
        "programmer-mcu-uid.bin": physical_uid_bytes(),
        "pairing-store.bin": paired,
        "runtime-before.json": b"{}\n",
        "ota-before.json": b"{}\n",
        "secure-primary.bin": secure,
        "nonsecure-primary.bin": nonsecure,
        "primary-audit.json": (
            json.dumps(audit, indent=2, sort_keys=True) + "\n"
        ).encode(),
    }
    assert set(content) == set(closed.TRANSACTION_FILE_SIZES)
    for name, data in content.items():
        (root / name).write_bytes(data)

    files = {
        name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        for name, data in content.items()
    }
    transaction_id = str(uuid.uuid4())
    manifest = {
        "schema": "roller-ecu-closed-transition-v2",
        "transaction_uuid": transaction_id,
        "created_utc": "2026-08-30T00:00:00+00:00",
        "target": {
            "probe_serial": PROBE,
            "device_id": "0x484",
            "product_state": "OPEN",
            "mcu_uid": closed.EXPECTED_MCU_UID,
            "atecc608_serial": closed.EXPECTED_ATECC_SERIAL,
            "atecc_config_crc32c": closed.EXPECTED_ATECC_CONFIG_CRC32C,
            "pairing_generation": closed.EXPECTED_PAIRING_GENERATION,
        },
        "release": {
            "version": "1.0.15",
            "build": 0,
            "security_counter": 15,
            "accepted_sequence": 15,
            "oemirot_size": 52292,
            "oemirot_sha256": closed.EXPECTED_RELEASE_CLOSED_SHA256,
        },
        "tool": {
            "stm32_programmer_realpath": "/opt/st/STM32_Programmer_CLI",
            "stm32_programmer_sha256": "a" * 64,
            "stm32_programmer_version": "2.23.0",
        },
        "files": files,
    }
    (root / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    (root / "phase.txt").write_text(
        "\n".join(closed.CLOSED_PHASES) + "\n", encoding="utf-8")
    return transaction_id


def run_create(source, output_root):
    environment = os.environ.copy()
    environment.update({"ECU_BACKUP_DIR": str(output_root),
                        "STLINK_SERIAL": PROBE})
    return subprocess.run(
        [str(CREATE), str(source), "host-test"],
        cwd=PROJECT, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def run_consumer(package, version="1.0.15"):
    environment = os.environ.copy()
    environment.update({
        "ECU_RECOVERY_BACKUP_DIR": str(package),
        "ECU_INITIAL_VERSION": version,
        "STLINK_SERIAL": PROBE,
    })
    return subprocess.run(
        [str(CONSUMER), "validate-full-regression-package"],
        cwd=PROJECT, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)


class FullRegressionRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        cls.source = cls.root / "closed-transaction"
        cls.transaction_id = create_closed_transaction(cls.source)
        cls.output_root = cls.root / "packages"
        result = run_create(cls.source, cls.output_root)
        if result.returncode != 0:
            raise RuntimeError(
                f"fixture recovery creation failed:\n{result.stdout}\n{result.stderr}")
        cls.package = Path(result.stdout.splitlines()[-1])

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_reviewed_artifact_hashes_and_initial_identity_are_exact(self):
        for path, (expected_size, expected_hash) in EXPECTED_ARTIFACTS.items():
            with self.subTest(path=path.name):
                self.assertEqual(path.stat().st_size, expected_size)
                self.assertEqual(sha256(path), expected_hash)
        expected_identity = backup.ReleaseIdentity(1, 0, 15, 0, 15)
        secure = backup.parse_primary(
            "secure", SECURE_INITIAL.read_bytes(),
            backup.SECURE_OFFSET, backup.SECURE_SIZE)
        nonsecure = backup.parse_primary(
            "nonsecure", NONSECURE_INITIAL.read_bytes(),
            backup.NONSECURE_OFFSET, backup.NONSECURE_SIZE)
        self.assertEqual(secure.identity, expected_identity)
        self.assertEqual(nonsecure.identity, expected_identity)

    def test_creator_emits_canonical_v4_bound_to_closed_transaction(self):
        manifest = json.loads(
            (self.package / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"],
                         "roller-ecu-full-regression-backup-v4")
        self.assertEqual(manifest["capture"]["transaction_uuid"],
                         self.transaction_id)
        self.assertEqual(manifest["capture"]["final_phase"],
                         "product_state_closed_verified")
        self.assertEqual(manifest["installed_firmware"], {
            "version": "1.0.15",
            "build": 0,
            "security_counter": 15,
            "accepted_sequence": 15,
            "layout_version": 65536,
            "ota_package_sha256": EXPECTED_ARTIFACTS[OTA_PACKAGE][1],
        })
        self.assertEqual(manifest["recovery_inputs"]["release_open_oemirot"], {
            "size": 52276,
            "sha256": EXPECTED_ARTIFACTS[RELEASE_OPEN][1],
        })
        self.assertEqual(
            (self.package / "closed-transition-phase.txt").read_text(),
            "\n".join(closed.CLOSED_PHASES) + "\n")

    def test_v4_carries_exact_manifest_bound_dual_pairing_evidence(self):
        manifest = json.loads(
            (self.package / "manifest.json").read_text(encoding="utf-8"))
        paired_path = self.package / "pairing-store.bin"
        paired = paired_path.read_bytes()
        self.assertEqual(manifest["files"]["pairing-store.bin"], {
            "size": 0x4000,
            "sha256": hashlib.sha256(paired).hexdigest(),
        })
        flash = (self.package / "full-flash.bin").read_bytes()
        persistent = (self.package / "secure-persistent.bin").read_bytes()
        self.assertEqual(paired, flash[0xE0000:0xE0000 + 0x4000])
        self.assertEqual(paired, persistent[:0x4000])
        report = pairing.verify_dual_store(paired)
        self.assertEqual(report["redundant_copies"], 2)
        self.assertIs(report["byte_identical"], True)

    def test_recovery_package_stages_no_private_key_or_obk_file(self):
        manifest = json.loads(
            (self.package / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(
            {path.name for path in self.package.iterdir()},
            set(manifest["files"]) | {"manifest.json"})
        for name in manifest["files"]:
            with self.subTest(name=name):
                self.assertNotIn("private", name.lower())
                self.assertFalse(name.endswith((".key", ".pem", ".obk")))

    def test_offline_consumer_accepts_exact_v4_package(self):
        result = run_consumer(self.package)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("no target or private PKI was accessed", result.stdout)

    def test_consumer_rejects_manifest_or_file_tampering_and_stale_version(self):
        for mutation in ("sequence", "flash", "version"):
            with self.subTest(mutation=mutation):
                copy = self.root / f"mutated-{mutation}"
                if copy.exists():
                    shutil.rmtree(copy)
                shutil.copytree(self.package, copy)
                requested_version = "1.0.15"
                if mutation == "sequence":
                    manifest = json.loads(
                        (copy / "manifest.json").read_text(encoding="utf-8"))
                    manifest["installed_firmware"]["accepted_sequence"] = 14
                    (copy / "manifest.json").write_text(json.dumps(manifest))
                elif mutation == "flash":
                    with (copy / "full-flash.bin").open("r+b") as stream:
                        stream.seek(0)
                        stream.write(b"X")
                else:
                    requested_version = "1.0.14"
                result = run_consumer(copy, requested_version)
                self.assertNotEqual(result.returncode, 0)

    def test_creator_rejects_incomplete_or_tampered_source_transaction(self):
        for mutation in ("phase", "flash"):
            with self.subTest(mutation=mutation):
                source = self.root / f"source-{mutation}"
                if source.exists():
                    shutil.rmtree(source)
                shutil.copytree(self.source, source)
                if mutation == "phase":
                    (source / "phase.txt").write_text(
                        "\n".join(closed.CLOSED_PHASES[:-1]) + "\n")
                else:
                    with (source / "full-flash.bin").open("r+b") as stream:
                        stream.seek(0)
                        stream.write(b"X")
                result = run_create(source, self.root / f"output-{mutation}")
                self.assertNotEqual(result.returncode, 0)

    def test_creator_rejects_manifest_rehashed_nonidentical_pairing_copies(self):
        source = self.root / "source-correlated-pairing-tamper"
        if source.exists():
            shutil.rmtree(source)
        shutil.copytree(self.source, source)
        pairing_offset = pairing.SECTOR_SIZE + pairing.RECORD_SIZE
        mutations = {
            "full-flash.bin": 0xE0000 + pairing_offset,
            "secure-persistent.bin": pairing_offset,
            "pairing-store.bin": pairing_offset,
        }
        for name, offset in mutations.items():
            path = source / name
            data = bytearray(path.read_bytes())
            self.assertEqual(data[offset], 0xFF)
            data[offset] = 0xFE
            path.write_bytes(data)
        manifest_path = source / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for name in mutations:
            data = (source / name).read_bytes()
            manifest["files"][name] = {
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        result = run_create(source, self.root / "output-correlated-pairing")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Pairing-store verification failed",
                      result.stdout + result.stderr)

    def test_legacy_packages_have_explicit_safe_migration(self):
        for version in (1, 2, 3):
            with self.subTest(version=version):
                legacy = self.root / f"legacy-v{version}"
                legacy.mkdir(exist_ok=True)
                (legacy / "manifest.json").write_text(json.dumps({
                    "schema": f"roller-ecu-full-regression-backup-v{version}",
                }))
                result = run_consumer(legacy)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("remain evidence", result.stderr)
        result = run_create(self.root / "legacy-prefix", self.root / "unused")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("retained as evidence", result.stderr)

    def test_open_loader_preflight_pins_exact_releaseopen(self):
        text = CONSUMER.read_text(encoding="utf-8")
        start = text.index("check_open_loader_replacement_inputs()")
        end = text.index("open_loader_phase_done()", start)
        body = text[start:end]
        self.assertIn('"${task_boot_size}" -ne 52276', body)
        self.assertIn(EXPECTED_ARTIFACTS[RELEASE_OPEN][1], body)

    def test_open_loader_resume_rechecks_exact_staged_release_artifacts(self):
        text = CONSUMER.read_text(encoding="utf-8")
        start = text.index("verify_open_loader_transaction_manifest()")
        end = text.index("continue_open_loader_replacement()", start)
        body = text[start:end]
        self.assertIn('"size": 52276', body)
        self.assertIn(EXPECTED_ARTIFACTS[RELEASE_OPEN][1], body)
        self.assertIn('"size": 525371', body)
        self.assertIn(HISTORICAL_OPEN_LOADER_1_0_14_OTA_SHA256, body)
        self.assertIn('"roller-ecu-1.0.14.recu"', body)
        self.assertIn("OPEN loader transaction file set is not canonical", body)
        self.assertIn("transaction file is missing or a symbolic link", body)

    def test_open_loader_runtime_gate_uses_fresh_exact_security_validator(self):
        text = CONSUMER.read_text(encoding="utf-8")
        start = text.index("check_runtime_authentication()")
        end = text.index("check_open_loader_replacement_inputs()", start)
        body = text[start:end]
        self.assertIn(
            '"${task_runtime_preflight_verifier}" runtime-state', body)
        self.assertNotIn('status_age_ms', body)
        replacement_start = end
        replacement_end = text.index("open_loader_phase_done()", replacement_start)
        replacement = text[replacement_start:replacement_end]
        self.assertIn(
            'require_file "${task_runtime_preflight_verifier}"', replacement)

        verifier = (TOOLS / "verify_closed_preflight.py").read_text(
            encoding="utf-8")
        for gate in (
                '"security_age_ms"', 'security.get("atecc_result")',
                'security.get("auth_result")',
                'EXPECTED_ATECC_CONFIG_CRC32C',
                'EXPECTED_PAIRING_GENERATION',
                'SAFETY_SECURITY_AUTHENTICATED',
                'SAFETY_SECURITY_QUARANTINE',
                'SAFETY_STATUS_OTA_UNCONFIRMED',
                'diagnostic.get("requested_relay_mask")',
                'diagnostic.get("applied_relay_mask")',
                'status.get("forward_duty_permille")',
                'status.get("reverse_duty_permille")',
                'state.get("tuning_active") is False'):
            with self.subTest(gate=gate):
                self.assertIn(gate, verifier)


if __name__ == "__main__":
    unittest.main()
