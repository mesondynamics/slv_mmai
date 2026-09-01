import pathlib
import re
import unittest


PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[1]
NETWORK_SOURCE = PROJECT_ROOT / "NonSecure/App/Src/ecu_network.c"
NETWORK_HEADER = PROJECT_ROOT / "NonSecure/App/Inc/ecu_network.h"
AUTHORITY_POLICY_HEADER = (
    PROJECT_ROOT / "NonSecure/App/Network/control_authority_policy.h"
)


class NetworkPolicySourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = NETWORK_SOURCE.read_text(encoding="utf-8")
        cls.header = NETWORK_HEADER.read_text(encoding="utf-8")
        cls.authority_policy = AUTHORITY_POLICY_HEADER.read_text(encoding="utf-8")

    def test_control_hosts_follow_serial_derived_network_plan(self):
        expected_hosts = {
            "REMOTE": (172, 16, 0, 9),
            "SERVICE": (172, 16, 0, 10),
            "DOMAIN": (172, 16, 0, 12),
        }
        self.assertRegex(
            self.header,
            r'#define ECU_PRODUCT_SERIAL\s+"SN-EJAHGJI"',
        )
        for role, address in expected_hosts.items():
            for octet, value in enumerate(address):
                self.assertRegex(
                    self.header,
                    rf"#define ECU_{role}_HOST_ADDRESS_{octet}\s+{value}U",
                )

        control_policy = self.source[
            self.source.index("static bool Network_ControlHostAuthorized"):
            self.source.index("static bool Network_SenderCanControlSteering")
        ]
        for role in ("REMOTE", "DOMAIN"):
            self.assertIn(f"ECU_{role}_HOST_ADDRESS_3", control_policy)
        self.assertIn("Network_ServiceHostAuthorized(address)", control_policy)

        service_policy = self.source[
            self.source.index("static bool Network_ServiceHostAuthorized"):
            self.source.index("static bool Network_ControlHostAuthorized")
        ]
        for octet in range(4):
            self.assertIn(
                f"ECU_SERVICE_HOST_ADDRESS_{octet}",
                service_policy,
            )

    def test_emergency_dominates_before_host_filter(self):
        start = self.source.index("static bool Network_AcceptControl")
        end = self.source.index("static void Network_ReceiveControl", start)
        function = self.source[start:end]
        emergency = function.index("if (emergency_requested)")
        accepted = function.index("return true;", emergency)
        trusted = function.index("Network_ControlHostAuthorized", accepted)
        semantic = function.index("ECU_ProtocolControlValuesValid", trusted)
        self.assertLess(emergency, accepted)
        self.assertLess(accepted, trusted)
        self.assertLess(trusted, semantic)

    def test_live_sender_session_is_bound_to_its_source_address(self):
        start = self.source.index("static bool Network_AcceptControl")
        end = self.source.index("static void Network_ReceiveControl", start)
        function = self.source[start:end]
        self.assertRegex(
            function,
            r"(?s)slot != NULL\).*?!ip_addr_cmp\(address,\s*"
            r"&slot->source_address\)",
        )
        self.assertIn("ip_addr_copy(slot->source_address, *address);", function)

    def test_custom_sender_cannot_alias_a_secure_steering_identity(self):
        start = self.source.index("static bool Network_AcceptControl")
        end = self.source.index("static void Network_ReceiveControl", start)
        function = self.source[start:end]
        emergency = function.index("if (emergency_requested)")
        emergency_accept = function.index("return true;", emergency)
        restriction = function.index("Network_SenderCanControlSteering")
        self.assertLess(emergency_accept, restriction)
        self.assertRegex(
            self.source,
            r"static bool Network_SenderCanControlSteering\(uint8_t sender_id\)"
            r"(?s:.*?)return \(sender_id >= 1U\) && \(sender_id <= 3U\);",
        )
        self.assertRegex(
            function,
            r"Network_ControlRequestsSteering\(flags,\s*"
            r"&datagram->control\)\s*&&\s*"
            r"!Network_SenderCanControlSteering\(datagram->sender_id\)",
        )
        request_helper = self.source[
            self.source.index("static bool Network_ControlRequestsSteering"):
            self.source.index("static uint8_t Network_EffectivePriority")
        ]
        for field in (
            "steering_enable",
            "steering_target_tdeg",
            "steering_speed_tdeg_per_s",
        ):
            self.assertIn(field, request_helper)
        self.assertIn("ECU_CONTROL_FLAG_STEERING_RATE", request_helper)

        model = (PROJECT_ROOT / "NonSecure/App/Src/ecu_data_model.c").read_text(
            encoding="utf-8"
        )
        self.assertIn("ECU_SenderCanControlSteering(sender_id)", model)
        self.assertIn("return SAFETY_RESULT_BAD_ARGUMENT;", model)

    def test_expired_authority_cannot_be_hidden_by_same_slot_reuse(self):
        expire_start = self.source.index("static void Network_ExpireSlots")
        expire_end = self.source.index("static ControlSlot *Network_FindActiveSlot")
        expire = self.source[expire_start:expire_end]
        apply_start = self.source.index("static void Network_ApplyAuthority")
        apply_end = self.source.index("static bool Network_SendV2", apply_start)
        apply = self.source[apply_start:apply_end]
        accept_start = self.source.index("static bool Network_AcceptControl")
        accept_end = self.source.index("static void Network_ReceiveControl", accept_start)
        accept = self.source[accept_start:accept_end]

        self.assertIn("session_generation", self.source)
        self.assertIn("active_session_generation", apply)
        self.assertRegex(
            apply,
            r"selected->session_generation\s*!=\s*"
            r"active_session_generation",
        )
        self.assertIn("ControlAuthorityPolicy_MarkAuthorityExpired", expire)
        self.assertIn("ControlAuthorityPolicy_RequireRearm", expire)
        self.assertIn("ControlAuthorityPolicy_CanEstablish", accept)
        rearm_gate = accept.index("ControlAuthorityPolicy_CanEstablish")
        ordinary_acquire = accept.index("Network_AcquireSlot", rearm_gate)
        self.assertLess(rearm_gate, ordinary_acquire)
        safe_round = apply.index("ControlAuthorityPolicy_BeginSafetyRound")
        control_lost = apply.index("ECU_DataModelControlLost", safe_round)
        secure_apply = apply.index("ECU_DataModelApplyControl", control_lost)
        neutral_rearm = apply.index(
            "ControlAuthorityPolicy_CompleteNeutralRearm", secure_apply
        )
        self.assertLess(safe_round, control_lost)
        self.assertLess(control_lost, secure_apply)
        self.assertLess(secure_apply, neutral_rearm)
        self.assertIn("ControlAuthorityPolicy_SafetyHoldActive", apply)

    def test_authority_policy_locks_reviewed_timeout_boundaries(self):
        self.assertIn(
            "return (int32_t)(now - last) > (int32_t)timeout;",
            self.authority_policy,
        )
        self.assertIn("safety_round_pending", self.authority_policy)
        self.assertIn("safety_hold_deadline", self.authority_policy)
        self.assertIn("sender_rearm_mask", self.authority_policy)

    def test_tuning_and_ota_do_not_reply_to_other_hosts(self):
        tuning = self.source.index("static void Network_ReceiveTuning")
        trusted = self.source.index("Network_ServiceHostAuthorized(address)", tuning)
        switch = self.source.index("switch (header.message_type)", tuning)
        self.assertLess(trusted, switch)
        self.assertIn(
            "return Network_ServiceHostAuthorized(address);",
            self.source[self.source.index("Network_OtaHostAuthorized"):tuning],
        )

    def test_explicit_sender_priority_drives_deterministic_arbitration(self):
        priority = self.source[
            self.source.index("static uint8_t Network_EffectivePriority"):
            self.source.index("static ECU_ControlMode Network_ModeForSlot")
        ]
        self.assertIn("if (requested_priority != 0U)", priority)
        self.assertIn("return requested_priority;", priority)
        self.assertIn("requested_priority == CONTROL_PRIORITY_EMERGENCY", priority)

        selection = self.source[
            self.source.index("static ControlSlot *Network_SelectAuthority"):
            self.source.index("static void Network_ApplyAuthority")
        ]
        self.assertRegex(
            selection,
            r"slot->priority > selected->priority",
        )
        self.assertRegex(
            selection,
            r"slot->priority == selected->priority(?s:.*?)"
            r"slot->sender_id < selected->sender_id",
        )

    def test_periodic_udp_classes_have_distinct_reviewed_phases(self):
        expected = {
            "STEERING_STATUS_PHASE_MS": 10,
            "LEGACY_STATUS_PHASE_MS": 25,
            "DIAGNOSTIC_PHASE_MS": 35,
            "SECURITY_STATUS_PHASE_MS": 60,
        }
        values = []
        for name, value in expected.items():
            self.assertRegex(self.source, rf"#define {name}\s+{value}UL")
            values.append(value)
        self.assertEqual(len(values), len(set(values)))
        self.assertIn('#include "ethernet_tx_scheduler.h"', self.source)
        self.assertIn("EthernetTxScheduler_Update", self.source)
        self.assertIn("EthernetTxScheduler_Pop", self.source)
        self.assertNotRegex(self.source, r"last_(?:status|legacy_status|"
                            r"steering_status|diagnostic|security_status|"
                            r"telemetry)_tick")
        for category in (
            "STATUS", "STEERING", "DIAGNOSTIC", "SECURITY", "LEGACY",
            "TELEMETRY",
        ):
            self.assertIn(f"case ETHERNET_TX_CATEGORY_{category}:", self.source)
        schedule = {
            "STATUS": ("STATUS_PERIOD_MS", "0U"),
            "STEERING": ("STEERING_STATUS_PERIOD_MS",
                         "STEERING_STATUS_PHASE_MS"),
            "DIAGNOSTIC": ("DIAGNOSTIC_PERIOD_MS", "DIAGNOSTIC_PHASE_MS"),
            "SECURITY": ("SECURITY_STATUS_PERIOD_MS",
                         "SECURITY_STATUS_PHASE_MS"),
            "LEGACY": ("LEGACY_STATUS_PERIOD_MS", "LEGACY_STATUS_PHASE_MS"),
            "TELEMETRY": ("TELEMETRY_SEND_PERIOD_MS", "0U"),
        }
        for category, (period, phase) in schedule.items():
            self.assertRegex(
                self.source,
                rf"(?s)\[ETHERNET_TX_CATEGORY_{category}\]\s*=\s*\{{.*?"
                rf"\.period_ms\s*=\s*{period},.*?"
                rf"\.phase_ms\s*=\s*{phase}",
            )

    def test_transmit_resource_failures_are_counted(self):
        self.assertIn("uint32_t transmit_failures;", self.header)
        send_v2 = self.source[
            self.source.index("static bool Network_SendV2"):
            self.source.index("static bool Network_SendLegacyStatus")
        ]
        self.assertGreaterEqual(send_v2.count("++counters.transmit_failures;"), 4)


if __name__ == "__main__":
    unittest.main()
