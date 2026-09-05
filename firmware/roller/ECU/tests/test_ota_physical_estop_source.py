import pathlib
import unittest


PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]


class OtaPhysicalEStopSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = (
            PROJECT_ROOT / "tools/ethernet_ota.py"
        ).read_text(encoding="utf-8")

    def test_host_gate_precedes_begin_and_covers_every_chunk(self):
        transfer_start = self.tool.index("def transfer(")
        transfer_end = self.tool.index("\ndef main()", transfer_start)
        transfer = self.tool[transfer_start:transfer_end]
        arm = transfer.index("monitor.arm()")
        begin = transfer.index("client.request(MSG_BEGIN")
        chunk_loop = transfer.index("while offset < len(image)")
        chunk_guard = transfer.index("monitor.require_held()", chunk_loop)
        chunk_request = transfer.index("client.request(MSG_CHUNK", chunk_loop)
        finish_guard = transfer.rindex(
            "monitor.require_held()", 0, transfer.index("MSG_FINISH")
        )
        self.assertLess(arm, begin)
        self.assertLess(chunk_guard, chunk_request)
        self.assertLess(finish_guard, transfer.index("MSG_FINISH"))

    def test_tool_waits_for_reset_confirmation_before_reporting_complete(self):
        transfer_start = self.tool.index("def transfer(")
        transfer_end = self.tool.index("\ndef main()", transfer_start)
        transfer = self.tool[transfer_start:transfer_end]
        finish = transfer.index("MSG_FINISH")
        status = transfer.index("MSG_STATUS", finish)
        runtime = transfer.index("wait_for_confirmed_runtime", status)
        complete = transfer.index('report("complete"', runtime)
        self.assertLess(finish, status)
        self.assertLess(status, runtime)
        self.assertLess(runtime, complete)
        self.assertIn("ECU_CAP_LATCHED_ESTOP_RESET", self.tool)
        self.assertIn("SAFETY_STATUS_OTA_UNCONFIRMED", self.tool)
        self.assertIn("SAFETY_STATUS_ATECC_AUTHENTICATED", self.tool)

    def test_ui_reuses_its_validated_diagnostic_stream_for_ota(self):
        ui = (PROJECT_ROOT / "tools/ecu_debug_ui.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("def _ota_diagnostic_observation", ui)
        self.assertIn(
            "diagnostic_provider=self._ota_diagnostic_observation", ui
        )
        self.assertIn("sock is self.diagnostic_socket", ui)
        self.assertIn("source[1] == STATUS_PORT", ui)
        self.assertIn("diagnostic_provider: DiagnosticProvider", self.tool)


if __name__ == "__main__":
    unittest.main()
