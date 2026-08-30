import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = PROJECT_ROOT / "tools" / "package_firmware.py"
SPEC = importlib.util.spec_from_file_location("package_firmware", MODULE_PATH)
PACKAGE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = PACKAGE
SPEC.loader.exec_module(PACKAGE)


def release_metadata(version: str, security_counter: int,
                     update_sequence: int) -> dict[str, object]:
    return {
        "format": "roller-ecu-ota-v1",
        "version": version,
        "security_counter": security_counter,
        "update_sequence": update_sequence,
        "layout_version": 0x10000,
        "secure_sha256": "0" * 64,
        "nonsecure_sha256": "1" * 64,
        "secure_size": 0x30000,
        "nonsecure_size": 0x50000,
    }


def add_release(history: Path, version: str, security_counter: int,
                update_sequence: int | None = None) -> None:
    release_dir = history / version
    release_dir.mkdir(parents=True)
    (release_dir / "metadata.json").write_text(
        json.dumps(release_metadata(
            version, security_counter,
            security_counter if update_sequence is None else update_sequence,
        )),
        encoding="utf-8",
    )


class ReleaseIdentityTest(unittest.TestCase):
    def test_next_unique_identity_is_allowed(self):
        with tempfile.TemporaryDirectory() as temp_name:
            history = Path(temp_name) / "artifacts" / "firmware"
            add_release(history, "1.0.12", 12)
            add_release(history, "1.0.13", 13)
            PACKAGE.validate_release_identity(history, "1.0.14", 14, 14)

    def test_existing_or_lower_version_is_rejected_even_with_newer_counters(self):
        with tempfile.TemporaryDirectory() as temp_name:
            history = Path(temp_name)
            add_release(history, "1.0.13", 13)
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError, "version 1.0.13 must be strictly greater"
            ):
                PACKAGE.validate_release_identity(history, "1.0.13", 14, 14)
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError, "version 1.0.12 must be strictly greater"
            ):
                PACKAGE.validate_release_identity(history, "1.0.12", 14, 14)

    def test_existing_or_lower_security_counter_is_rejected_for_new_version(self):
        with tempfile.TemporaryDirectory() as temp_name:
            history = Path(temp_name)
            add_release(history, "1.0.13", 13)
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError,
                "security_counter 13 must be strictly greater",
            ):
                PACKAGE.validate_release_identity(history, "1.0.14", 13, 14)
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError,
                "security_counter 12 must be strictly greater",
            ):
                PACKAGE.validate_release_identity(history, "1.0.14", 12, 14)

    def test_existing_or_lower_update_sequence_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp_name:
            history = Path(temp_name)
            add_release(history, "1.0.13", 13, 13)
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError,
                "update_sequence 13 must be strictly greater",
            ):
                PACKAGE.validate_release_identity(history, "1.0.14", 14, 13)
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError,
                "update_sequence 12 must be strictly greater",
            ):
                PACKAGE.validate_release_identity(history, "1.0.14", 14, 12)

    def test_unrelated_legacy_duplicate_does_not_block_new_identity(self):
        with tempfile.TemporaryDirectory() as temp_name:
            history = Path(temp_name)
            add_release(history, "1.0.1", 2, 1)
            add_release(history, "1.0.3", 2, 3)
            PACKAGE.validate_release_identity(history, "1.0.14", 14, 14)

    def test_incomplete_or_untrusted_history_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp_name:
            history = Path(temp_name)
            (history / "1.0.12").mkdir()
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError, "history is incomplete"
            ):
                PACKAGE.validate_release_identity(history, "1.0.14", 14, 14)

        with tempfile.TemporaryDirectory() as temp_name:
            history = Path(temp_name)
            release_dir = history / "1.0.12"
            release_dir.mkdir()
            (release_dir / "metadata.json").write_text("{broken", encoding="utf-8")
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError, "cannot trust release metadata"
            ):
                PACKAGE.validate_release_identity(history, "1.0.14", 14, 14)

    def test_metadata_schema_must_be_exact(self):
        for missing in PACKAGE.RELEASE_METADATA_TYPES:
            with self.subTest(missing=missing), \
                    tempfile.TemporaryDirectory() as temp_name:
                history = Path(temp_name)
                add_release(history, "1.0.13", 13)
                metadata_path = history / "1.0.13" / "metadata.json"
                metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
                del metadata[missing]
                metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
                with self.assertRaisesRegex(
                    PACKAGE.ReleaseIdentityError, "metadata schema is not exact"
                ):
                    PACKAGE.validate_release_identity(history, "1.0.14", 14, 14)

        with tempfile.TemporaryDirectory() as temp_name:
            history = Path(temp_name)
            add_release(history, "1.0.13", 13)
            metadata_path = history / "1.0.13" / "metadata.json"
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            metadata["unexpected"] = 0
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError, "metadata schema is not exact"
            ):
                PACKAGE.validate_release_identity(history, "1.0.14", 14, 14)

    def test_duplicate_metadata_field_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp_name:
            history = Path(temp_name)
            add_release(history, "1.0.13", 13)
            metadata_path = history / "1.0.13" / "metadata.json"
            text = metadata_path.read_text(encoding="utf-8")
            metadata_path.write_text(
                text.replace(
                    '"version": "1.0.13",',
                    '"version": "1.0.13", "version": "1.0.99",',
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError, "duplicate JSON field"
            ):
                PACKAGE.validate_release_identity(history, "1.0.14", 14, 14)

    def test_bool_is_rejected_for_every_metadata_field(self):
        for field in PACKAGE.RELEASE_METADATA_TYPES:
            with self.subTest(field=field), \
                    tempfile.TemporaryDirectory() as temp_name:
                history = Path(temp_name)
                add_release(history, "1.0.13", 13)
                metadata_path = history / "1.0.13" / "metadata.json"
                metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
                metadata[field] = True
                metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
                with self.assertRaisesRegex(
                    PACKAGE.ReleaseIdentityError, "has invalid type"
                ):
                    PACKAGE.validate_release_identity(history, "1.0.14", 14, 14)

    def test_requested_bool_values_fail_closed(self):
        with tempfile.TemporaryDirectory() as temp_name:
            history = Path(temp_name)
            add_release(history, "1.0.13", 13)
            for requested in (
                (True, 14, 14),
                ("1.0.14", True, 14),
                ("1.0.14", 14, True),
            ):
                with self.subTest(requested=requested), self.assertRaises(
                    PACKAGE.ReleaseIdentityError
                ):
                    PACKAGE.validate_release_identity(history, *requested)

    def test_invalid_metadata_values_fail_closed(self):
        changes = {
            "format": "unknown",
            "security_counter": 0,
            "update_sequence": 0,
            "layout_version": 0,
            "secure_sha256": "A" * 64,
            "nonsecure_sha256": "0" * 63,
            "secure_size": 0,
            "nonsecure_size": 0,
        }
        for field, invalid in changes.items():
            with self.subTest(field=field), \
                    tempfile.TemporaryDirectory() as temp_name:
                history = Path(temp_name)
                add_release(history, "1.0.13", 13)
                metadata_path = history / "1.0.13" / "metadata.json"
                metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
                metadata[field] = invalid
                metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
                with self.assertRaises(PACKAGE.ReleaseIdentityError):
                    PACKAGE.validate_release_identity(history, "1.0.14", 14, 14)

    def test_noncanonical_release_history_entries_fail_closed(self):
        with tempfile.TemporaryDirectory() as temp_name:
            history = Path(temp_name)
            (history / "stray-file").write_text("not a release", encoding="utf-8")
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError, "not a real directory"
            ):
                PACKAGE.validate_release_identity(history, "1.0.14", 14, 14)

        with tempfile.TemporaryDirectory() as temp_name:
            root = Path(temp_name)
            real = root / "real"
            add_release(real, "1.0.13", 13)
            history = root / "history"
            history.symlink_to(real, target_is_directory=True)
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError, "not a real directory"
            ):
                PACKAGE.validate_release_identity(history, "1.0.14", 14, 14)

        with tempfile.TemporaryDirectory() as temp_name:
            history = Path(temp_name)
            add_release(history, "1.0.13", 13)
            (history / "1.0.13").rename(history / "not-canonical")
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError, "directory/version mismatch"
            ):
                PACKAGE.validate_release_identity(history, "1.0.14", 14, 14)

    def test_output_directory_must_be_canonical_history_path(self):
        with tempfile.TemporaryDirectory() as temp_name:
            history = Path(temp_name) / "firmware"
            add_release(history, "1.0.13", 13)
            with self.assertRaisesRegex(
                PACKAGE.ReleaseIdentityError, "canonical history path"
            ):
                PACKAGE.validate_release_identity(
                    history, "1.0.14", 14, 14,
                    Path(temp_name) / "outside" / "1.0.14",
                )

    def test_force_cannot_bypass_identity_gate_or_start_build(self):
        with tempfile.TemporaryDirectory() as temp_name:
            project = Path(temp_name)
            history = project / "artifacts" / "firmware"
            add_release(history, "1.0.13", 13)
            with mock.patch.object(PACKAGE, "PROJECT", project), \
                    mock.patch.object(PACKAGE, "run") as run_mock, \
                    mock.patch("sys.stderr"):
                with self.assertRaises(SystemExit) as raised:
                    PACKAGE.main([
                        "--version", "1.0.13",
                        "--security-counter", "13",
                        "--update-sequence", "14",
                        "--force",
                    ])
            self.assertEqual(raised.exception.code, 2)
            run_mock.assert_not_called()

    def test_force_cannot_target_an_existing_release_directory(self):
        with tempfile.TemporaryDirectory() as temp_name:
            project = Path(temp_name)
            history = project / "artifacts" / "firmware"
            add_release(history, "1.0.13", 13)
            with mock.patch.object(PACKAGE, "PROJECT", project), \
                    mock.patch.object(PACKAGE, "run") as run_mock, \
                    mock.patch("sys.stderr"):
                with self.assertRaises(SystemExit) as raised:
                    PACKAGE.main([
                        "--version", "1.0.14",
                        "--security-counter", "14",
                        "--update-sequence", "14",
                        "--output-dir", str(history / "1.0.13"),
                        "--force",
                    ])
            self.assertEqual(raised.exception.code, 2)
            run_mock.assert_not_called()


if __name__ == "__main__":
    unittest.main()
