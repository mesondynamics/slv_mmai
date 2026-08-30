import contextlib
import io
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import ecu_debug_auth as da


CANONICAL_DISCOVERY_LINES = (
    "discovery: target ID.......................:0x484",
    "discovery: SDA version.....................:2.4.0",
    "discovery: Vendor ID.......................:STMicroelectronics",
    "discovery: PSA lifecycle...................:ST_LIFECYCLE_CLOSED",
    "discovery: cryptosystems...................:Ecdsa-P256 SHA256",
    "discovery: ST provisioning integrity status:0xEAEAEAEA",
    "discovery: ST provisioning integrity status message:VALID",
)
CANONICAL_DISCOVERY = "\n".join(CANONICAL_DISCOVERY_LINES) + "\n"


class StrictDiscoveryParserTests(unittest.TestCase):
    def test_accepts_exact_closed_h563_evidence_with_ansi(self):
        evidence = "\x1b[39;49m" + CANONICAL_DISCOVERY.replace(
            "\n", "\x1b[0m\r\n\x1b[32m"
        )
        da.require_strict_closed_discovery(evidence)

    def test_rejects_each_missing_required_field(self):
        for omitted in range(len(CANONICAL_DISCOVERY_LINES)):
            with self.subTest(omitted=CANONICAL_DISCOVERY_LINES[omitted]):
                evidence = "\n".join(
                    line for index, line in enumerate(CANONICAL_DISCOVERY_LINES)
                    if index != omitted
                )
                with self.assertRaisesRegex(RuntimeError, "exactly one"):
                    da.require_strict_closed_discovery(evidence)

    def test_rejects_each_incorrect_required_value(self):
        replacements = (
            ("0x484", "0x483"),
            ("2.4.0", "2.5.0"),
            ("STMicroelectronics", "OtherVendor"),
            ("ST_LIFECYCLE_CLOSED", "ST_LIFECYCLE_PROVISIONING"),
            ("Ecdsa-P256 SHA256", "Ecdsa-P384 SHA384"),
            ("0xEAEAEAEA", "0xFFFFFFFF"),
            ("VALID", "INVALID"),
        )
        for expected, incorrect in replacements:
            with self.subTest(field=expected):
                evidence = CANONICAL_DISCOVERY.replace(expected, incorrect, 1)
                with self.assertRaisesRegex(RuntimeError, "expected"):
                    da.require_strict_closed_discovery(evidence)

    def test_rejects_duplicate_or_conflicting_target_records(self):
        for duplicate in (
            CANONICAL_DISCOVERY_LINES[0],
            "discovery: target ID.......................:0x483",
        ):
            with self.subTest(duplicate=duplicate):
                with self.assertRaisesRegex(RuntimeError, "exactly one target ID"):
                    da.require_strict_closed_discovery(
                        CANONICAL_DISCOVERY + duplicate + "\n"
                    )


class CliCaptureTests(unittest.TestCase):
    @mock.patch("tools.ecu_debug_auth.subprocess.run")
    def test_captures_combined_output_strips_ansi_and_echoes(self, run_mock):
        run_mock.return_value = subprocess.CompletedProcess(
            ["programmer"], 7, stdout="\x1b[31mproof\r\n\x1b[0m"
        )
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            result = da.run_cli(["programmer"])

        self.assertEqual(result, da.CliResult(7, "proof\n"))
        self.assertEqual(stream.getvalue(), "proof\n")
        kwargs = run_mock.call_args.kwargs
        self.assertEqual(kwargs["stdout"], subprocess.PIPE)
        self.assertEqual(kwargs["stderr"], subprocess.STDOUT)
        self.assertEqual(kwargs["errors"], "replace")


class DebugAuthActionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.programmer = self.root / "STM32_Programmer_CLI"
        self.programmer.write_text("not executed\n", encoding="utf-8")
        self.assets = self.root / "assets"
        self.assets.mkdir()
        self.pki = self.root / "pki"
        self.pki.mkdir()

    def arguments(self, action, *extra):
        return [
            action,
            "--programmer", str(self.programmer),
            "--assets-dir", str(self.assets),
            "--pki-dir", str(self.pki),
            *extra,
        ]

    def run_main(self, action, results, *extra):
        with mock.patch.object(da, "verify_assets", return_value=(b"", object())), \
                mock.patch.object(da, "clear_private_key"), \
                mock.patch.object(da, "run_cli", side_effect=results) as runner:
            outcome = da.main(self.arguments(action, *extra))
        return outcome, runner

    def test_closed_acceptance_is_required_before_any_programmer_access(self):
        for action in (
                "discover", "open-app-debug", "close-debug",
                "full-regression-to-open"):
            with self.subTest(action=action), \
                    mock.patch.object(da, "verify_assets") as verify, \
                    mock.patch.object(da, "run_cli") as runner, \
                    contextlib.redirect_stderr(io.StringIO()):
                arguments = self.arguments(action)
                if action == "full-regression-to-open":
                    arguments.append("--accept-full-device-erase")
                with self.assertRaises(SystemExit) as raised:
                    da.main(arguments)
                self.assertEqual(raised.exception.code, 2)
                verify.assert_not_called()
                runner.assert_not_called()

    def test_full_regression_requires_erase_acceptance_independently(self):
        with mock.patch.object(da, "verify_assets") as verify, \
                mock.patch.object(da, "run_cli") as runner, \
                contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                da.main(self.arguments(
                    "full-regression-to-open", "--accept-closed-target"
                ))
            self.assertEqual(raised.exception.code, 2)
            verify.assert_not_called()
            runner.assert_not_called()

    def test_discover_requires_exact_closed_evidence(self):
        outcome, runner = self.run_main(
            "discover",
            [da.CliResult(0, CANONICAL_DISCOVERY)],
            "--accept-closed-target",
        )
        self.assertEqual(outcome, 0)
        self.assertEqual(runner.call_count, 1)
        self.assertEqual(runner.call_args.args[0][-1], "debugauth=2")

        bad = CANONICAL_DISCOVERY.replace("0xEAEAEAEA", "0xFFFFFFFF")
        with self.assertRaisesRegex(RuntimeError, "expected"):
            self.run_main(
                "discover", [da.CliResult(0, bad)], "--accept-closed-target"
            )

    def test_discovery_exit_code_is_checked_before_evidence(self):
        with self.assertRaisesRegex(RuntimeError, "exit code 9"):
            self.run_main(
                "discover",
                [da.CliResult(9, CANONICAL_DISCOVERY)],
                "--accept-closed-target",
            )

    def test_open_preflights_closed_then_uses_permission_c_and_success(self):
        outcome, runner = self.run_main(
            "open-app-debug",
            [
                da.CliResult(0, CANONICAL_DISCOVERY),
                da.CliResult(0, "Debug Authentication: Authentication Success\n"),
            ],
            "--accept-closed-target",
        )
        self.assertEqual(outcome, 0)
        calls = [call.args[0] for call in runner.call_args_list]
        self.assertEqual(calls[0][-1], "debugauth=2")
        self.assertIn("per=c", calls[1])
        self.assertNotIn("per=a", calls[1])
        self.assertEqual(calls[1][-1], "debugauth=1")

    def test_open_rejects_missing_success_evidence(self):
        with self.assertRaisesRegex(RuntimeError, "Authentication Success"):
            self.run_main(
                "open-app-debug",
                [
                    da.CliResult(0, CANONICAL_DISCOVERY),
                    da.CliResult(0, "authentication command completed\n"),
                ],
                "--accept-closed-target",
            )

    def test_close_requires_locking_then_strict_post_close_discovery(self):
        outcome, runner = self.run_main(
            "close-debug",
            [
                da.CliResult(0, "Debug Authentication: Locking Debug\n"),
                da.CliResult(0, CANONICAL_DISCOVERY),
            ],
            "--accept-closed-target",
        )
        self.assertEqual(outcome, 0)
        calls = [call.args[0] for call in runner.call_args_list]
        self.assertEqual(calls[0][-1], "debugauth=3")
        self.assertEqual(calls[1][-1], "debugauth=2")

    def test_close_rejects_missing_locking_or_bad_postcondition(self):
        with self.assertRaisesRegex(RuntimeError, "Locking Debug"):
            self.run_main(
                "close-debug",
                [da.CliResult(0, "close command completed\n")],
                "--accept-closed-target",
            )

        not_closed = CANONICAL_DISCOVERY.replace(
            "ST_LIFECYCLE_CLOSED", "ST_LIFECYCLE_PROVISIONING"
        )
        with self.assertRaisesRegex(RuntimeError, "PSA lifecycle"):
            self.run_main(
                "close-debug",
                [
                    da.CliResult(0, "Locking Debug\n"),
                    da.CliResult(0, not_closed),
                ],
                "--accept-closed-target",
            )

    def test_full_regression_preflights_closed_and_uses_permission_a(self):
        outcome, runner = self.run_main(
            "full-regression-to-open",
            [
                da.CliResult(0, CANONICAL_DISCOVERY),
                da.CliResult(0, "full regression complete\n"),
            ],
            "--accept-closed-target",
            "--accept-full-device-erase",
        )
        self.assertEqual(outcome, 0)
        calls = [call.args[0] for call in runner.call_args_list]
        self.assertEqual(calls[0][-1], "debugauth=2")
        self.assertIn("per=a", calls[1])
        self.assertNotIn("per=c", calls[1])
        self.assertEqual(calls[1][-1], "debugauth=1")


if __name__ == "__main__":
    unittest.main()
