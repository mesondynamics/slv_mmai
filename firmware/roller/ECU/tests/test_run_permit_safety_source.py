import pathlib
import unittest


PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE_PATH = PROJECT_ROOT / "Secure/App/Src/safety_service.c"


class RunPermitSafetySourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE_PATH.read_text(encoding="utf-8")

    def function(self, name, next_name):
        start = self.source.index(name)
        end = self.source.index(next_name, start)
        return self.source[start:end]

    def test_baseline_is_exactly_k12_and_fails_hard_on_tpic_error(self):
        function = self.function(
            "static bool Safety_ApplyRunPermitBaseline",
            "static void Safety_QuiesceControlledOutputs",
        )
        self.assertIn(
            "baseline_mask = SAFETY_RELAY_ESTOP_RUN_PERMIT", function
        )
        self.assertIn("Safety_RunPermitCanClose", function)
        self.assertIn("Safety_EnableTpicBaseline", function)
        self.assertIn("SAFETY_STATUS_TPIC_ERROR", function)
        self.assertIn("Safety_HardDisableOutputs", function)
        self.assertIn("requested_relay_mask = baseline_mask", function)
        self.assertIn("applied_relay_mask = baseline_mask", function)

    def test_startup_restore_is_secure_and_not_network_gated(self):
        function = self.function(
            "int32_t Safety_ServiceInit", "uint32_t Safety_GetStatus"
        )
        auth = function.index("SecurityIdentity_Authenticate")
        ready = function.index("SAFETY_STATUS_READY", auth)
        restore = function.index("run_permit_restore_pending = 1U", ready)
        self.assertLess(auth, ready)
        self.assertLess(ready, restore)
        self.assertNotIn("ECU_Network", function)

    def test_physical_release_restores_only_after_debounce(self):
        tick = self.function(
            "static void Safety_OneMillisecondTick",
            "int32_t Safety_ServiceInit",
        )
        self.assertIn("ESTOP_RELEASE_DEBOUNCE_MS", tick)
        self.assertIn("physical_estop_inactive_ms", tick)
        self.assertIn("Safety_ApplyRunPermitBaseline", tick)
        falling = self.function(
            "void HAL_GPIO_EXTI_Falling_Callback",
            "void HAL_TIM_PeriodElapsedCallback",
        )
        self.assertIn("run_permit_restore_pending = 1U", falling)
        self.assertNotIn("Safety_TpicShift", falling)
        self.assertNotIn("Safety_ApplyRunPermitBaseline", falling)

    def test_disconnect_and_isolated_faults_quiesce_but_critical_faults_hard_off(self):
        timeout_tick = self.function(
            "static void Safety_OneMillisecondTick",
            "int32_t Safety_ServiceInit",
        )
        timeout = timeout_tick[timeout_tick.index("SAFETY_STATUS_COMMAND_TIMEOUT"):]
        self.assertIn("Safety_QuiesceControlledOutputs", timeout)
        critical = self.function(
            "static void Safety_LatchCriticalFault",
            "static void Safety_LatchIsolatedFault",
        )
        isolated = self.function(
            "static void Safety_LatchIsolatedFault",
            "static bool Safety_RefreshStartupWatchdog",
        )
        self.assertIn("Safety_HardDisableOutputs", critical)
        self.assertIn("Safety_QuiesceControlledOutputs", isolated)

    def test_network_estop_is_assertion_dominant_and_explicitly_reset(self):
        asserted = self.function(
            "int32_t Safety_AssertNetworkEStop",
            "int32_t Safety_ResetNetworkEStop",
        )
        reset = self.function(
            "int32_t Safety_ResetNetworkEStop",
            "int32_t Safety_SubmitActuatorCommand",
        )
        submit = self.function(
            "int32_t Safety_SubmitActuatorCommand",
            "int32_t Safety_KickWatchdog",
        )
        self.assertIn("SAFETY_STATUS_NETWORK_ESTOP_LATCHED", asserted)
        self.assertIn("Safety_HardDisableOutputs", asserted)
        self.assertIn("SAFETY_NETWORK_ESTOP_RESET_TOKEN", reset)
        self.assertIn("SAFETY_SECURITY_AUTHENTICATED", reset)
        self.assertIn("SAFETY_STATUS_READY", reset)
        self.assertIn("Safety_PhysicalEStopActive", reset)
        self.assertIn("run_permit_restore_pending = 1U", reset)
        self.assertIn("physical_estop_inactive_ms = 0U", reset)
        self.assertNotIn("Safety_ApplyRunPermitBaseline", reset)
        validation = submit.index("Safety_ValidateCommand")
        dominant = submit.index("command->run_permit_on == 0U")
        result_gate = submit.index("if (result != SAFETY_RESULT_OK)")
        self.assertLess(validation, dominant)
        self.assertLess(dominant, result_gate)
        self.assertEqual(
            reset.count(
                "safety_status &= ~SAFETY_STATUS_NETWORK_ESTOP_LATCHED;"
            ),
            1,
        )
        self.assertNotIn(
            "safety_status &= ~SAFETY_STATUS_NETWORK_ESTOP_LATCHED;",
            submit,
        )

    def test_ota_requires_physical_estop_through_confirmation(self):
        begin = self.function(
            "int32_t Safety_OtaBegin", "int32_t Safety_OtaWrite"
        )
        write = self.function(
            "int32_t Safety_OtaWrite", "int32_t Safety_OtaFinish"
        )
        finish = self.function(
            "int32_t Safety_OtaFinish",
            "int32_t Safety_OtaConfirmRunningImages",
        )
        confirm = self.function(
            "int32_t Safety_OtaConfirmRunningImages",
            "#if defined(ECU_FACTORY_PROVISIONING)",
        )
        for function in (begin, write, finish, confirm):
            self.assertIn("Safety_PhysicalEStopActive", function)
        self.assertIn("Safety_EnterOtaQuarantine", begin)
        self.assertIn("Safety_EnterOtaQuarantine", write)
        self.assertIn("Safety_EnterOtaQuarantine", finish)
        self.assertIn("Safety_HardDisableOutputs", confirm)
        quarantine = self.function(
            "static void Safety_EnterOtaQuarantine",
            "static int32_t Safety_CompleteOtaOperation",
        )
        active = quarantine.index("SAFETY_STATUS_OTA_ACTIVE")
        hard_off = quarantine.index("Safety_HardDisableOutputs")
        self.assertLess(active, hard_off)


if __name__ == "__main__":
    unittest.main()
