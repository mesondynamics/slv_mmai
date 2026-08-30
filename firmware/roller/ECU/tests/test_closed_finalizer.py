import os
import re
import subprocess
import unittest
from pathlib import Path


PROJECT = Path(__file__).resolve().parents[1]
SCRIPT = PROJECT / "tools" / "finalize_oemirot_closed.sh"


class ClosedFinalizerStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = SCRIPT.read_text(encoding="utf-8")

    def shell_function_source(self, name):
        match = re.search(
            rf"^{re.escape(name)}\(\) \{{.*?^\}}$",
            self.text,
            re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(match, f"missing shell function: {name}")
        return match.group(0)

    def run_shell_predicate(self, functions, invocation, sample,
                            include_state_pattern=False):
        parts = []
        if include_state_pattern:
            match = re.search(
                r"^task_provisioning_state_pattern='[^']+'$",
                self.text,
                re.MULTILINE)
            self.assertIsNotNone(match)
            parts.append(match.group(0))
        parts.extend(self.shell_function_source(name) for name in functions)
        parts.append(invocation)
        environment = os.environ.copy()
        environment["ECU_TEST_TEXT"] = sample
        return subprocess.run(
            ["bash", "-c", "\n\n".join(parts)],
            env=environment,
            check=False,
            capture_output=True,
            text=True)

    def test_shell_parses(self):
        subprocess.run(["bash", "-n", str(SCRIPT)], check=True)

    def test_resume_uid_upload_uses_a_programmer_supported_suffix(self):
        start = self.text.index("check_open_resume_identity()")
        end = self.text.index("boot_options_are_unprotected()", start)
        body = self.text[start:end]
        temporary = 'task_uid="$(mktemp --suffix=.bin)"'
        upload = 'check_programmer_mcu_uid "${task_uid}"'
        self.assertIn(temporary, body)
        self.assertIn(upload, body)
        self.assertLess(body.index(temporary), body.index(upload))

    def test_mutations_use_only_staged_transaction_inputs(self):
        self.assertIn('-d "${task_staged_boot}" 0x0C000000 -v', self.text)
        self.assertIn('-sdp "${task_obk}"', self.text)
        self.assertNotRegex(self.text, r'-d "\$\{task_boot_image\}" 0x0C000000')
        self.assertNotRegex(self.text, r'-sdp "\$\{task_assets\}')

    def test_resume_rebinds_staged_inputs_to_reproduced_pki(self):
        self.assertIn("verify_staged_against_reviewed_inputs()", self.text)
        start = self.text.index("verify_staged_against_reviewed_inputs()")
        end = self.text.index("create_closed_transaction()", start)
        body = self.text[start:end]
        self.assertIn("verify_reproducible_security_inputs", body)
        for name in (
                "DA_Config.obk", "OEMiRoT_Config.obk", "OEMiRoT_Data.obk",
                "security-assets.json", "cert-leaf-chain.b64",
                "oemirot-auth-s-public.pem", "oemirot-auth-ns-public.pem"):
            with self.subTest(name=name):
                self.assertIn(name, body)
        verify_start = self.text.index("verify_closed_transaction()")
        verify_end = self.text.index("check_programmer_version()", verify_start)
        verify_body = self.text[verify_start:verify_end]
        self.assertIn("verify_staged_against_reviewed_inputs", verify_body)

    def test_pre_mutation_fsync_and_phase_precede_first_write(self):
        capture = self.text.index("capture_closed_transaction_evidence()")
        fsync = self.text.index('sync -f "${task_transaction_dir}"', capture)
        evidence = self.text.index(
            "record_phase pre_mutation_evidence_verified", fsync)
        first_write = self.text.index("-ob WRPSGn1=0xFFFFFFFF", evidence)
        self.assertLess(fsync, evidence)
        self.assertLess(evidence, first_write)

    def test_release_closed_loader_is_exactly_pinned(self):
        self.assertIn('task_expected_boot_size=52292', self.text)
        self.assertIn(
            'task_expected_boot_sha256="4073a9faa15eed7ca6014f168ae91efc35ba1e26d8e74651552c9a0a3d8ef884"',
            self.text)
        self.assertIn(
            'require_size "${task_boot_image}" "${task_expected_boot_size}"',
            self.text)

    def test_installed_gate_is_exactly_release_1_0_15(self):
        self.assertIn(
            'task_installed_release_dir="${task_project_dir}/artifacts/firmware/1.0.15"',
            self.text)
        self.assertIn(
            '"${task_installed_release_dir}/roller-ecu-1.0.15.recu"',
            self.text)
        self.assertGreaterEqual(
            self.text.count('--installed-exact-1.0.15'), 2)
        self.assertNotIn('--installed-exact-1.0.14', self.text)
        for field in (
                '"version": "1.0.15"',
                '"security_counter": 15',
                '"accepted_sequence": 15'):
            with self.subTest(field=field):
                self.assertIn(field, self.text)

    def test_pairing_store_is_exactly_extracted_and_dual_verified(self):
        extract_start = self.text.index("extract_pairing_store_evidence()")
        extract_end = self.text.index(
            "verify_pairing_store_evidence()", extract_start)
        extract = self.text[extract_start:extract_end]
        self.assertIn('flash[0xE0000:0xE0000 + 0x4000]', extract)
        self.assertIn(
            'require_size "${task_transaction_dir}/pairing-store.bin" 16384',
            extract)

        verify_start = extract_end
        verify_end = self.text.index(
            "capture_closed_transaction_evidence()", verify_start)
        verify = self.text[verify_start:verify_end]
        self.assertIn('flash[0xE0000:0xE0000 + 0x4000] != pairing', verify)
        self.assertIn('persistent[:0x4000] != pairing', verify)
        self.assertIn('"${task_pairing_verifier}" dual', verify)
        self.assertIn('"${task_transaction_dir}/pairing-store.bin"', verify)

        capture_start = verify_end
        capture_end = self.text.index("create_closed_manifest()", capture_start)
        capture = self.text[capture_start:capture_end]
        self.assertLess(capture.index("extract_pairing_store_evidence"),
                        capture.index("verify_pairing_store_evidence"))
        self.assertLess(capture.index("verify_closed_transaction"),
                        capture.index("record_phase pairing_store_dual_verified"))
        self.assertLess(capture.index("record_phase pairing_store_dual_verified"),
                        capture.index("record_phase pre_mutation_evidence_verified"))

    def test_pairing_gate_is_manifest_bound_and_rechecked_on_resume(self):
        manifest_start = self.text.index("create_closed_manifest()")
        manifest_end = self.text.index("select_closed_transaction()", manifest_start)
        manifest = self.text[manifest_start:manifest_end]
        self.assertIn(
            "from verify_closed_preflight import TRANSACTION_FILE_SIZES",
            manifest)

        verify_start = self.text.index("verify_closed_transaction()")
        verify_end = self.text.index("check_programmer_version()", verify_start)
        verify = self.text[verify_start:verify_end]
        self.assertIn("transaction-manifest", verify)
        self.assertIn("verify_pairing_store_evidence", verify)

        continue_start = self.text.index("continue_closed_transaction()")
        continue_end = self.text.index("preflight()", continue_start)
        continuation = self.text[continue_start:continue_end]
        self.assertLess(continuation.index("verify_closed_transaction"),
                        continuation.index("phase_done pairing_store_dual_verified"))
        self.assertLess(
            continuation.index("record_phase pairing_store_dual_verified"),
            continuation.index("record_phase pre_mutation_evidence_verified"))
    def test_phase_commit_is_atomic_and_directory_synced(self):
        start = self.text.index("record_phase()")
        end = self.text.index("phase_done()", start)
        body = self.text[start:end]
        self.assertIn("tempfile.mkstemp", body)
        self.assertIn("os.fsync(stream.fileno())", body)
        self.assertIn("os.replace(temporary_name, path)", body)
        self.assertIn("os.fsync(directory_fd)", body)
        self.assertNotIn(">> \"${task_phase_file}\"", body)

    def test_resume_is_exact_serialized_and_has_no_latest_selection(self):
        self.assertIn('flock -n 9', self.text)
        self.assertIn('ECU_CLOSED_TRANSACTION_DIR', self.text)
        self.assertIn('--accept-same-physical-target', self.text)
        self.assertNotRegex(self.text, r'find .*closed-transition.*sort')

    def test_obkey_ambiguity_cannot_advance(self):
        self.assertIn('AMBIGUOUS SDP:', self.text)
        self.assertIn('exact-package replay is intentionally disabled', self.text)
        started_branch = re.search(
            r'if phase_done "\$\{task_started_phase\}"; then(?P<body>.*?)\n  fi',
            self.text, re.DOTALL)
        self.assertIsNotNone(started_branch)
        body = started_branch.group("body")
        self.assertIn('verify_obkey_evidence', body)
        self.assertIn('return 1', body)
        self.assertNotIn('-sdp', body)

    def test_provisioning_and_close_have_authoritative_gates(self):
        self.assertRegex(
            self.text,
            r'PRODUCT_STATE\[\[:space:\]\]\*: 0x17.*Provisioning')
        self.assertIn('ST_LIFECYCLE_PROVISIONING', self.text)
        self.assertIn('0xEAEAEAEA', self.text)
        self.assertIn('record_phase closed_request_started', self.text)
        self.assertIn('resume-decision', self.text)
        self.assertIn('ST_LIFECYCLE_CLOSED', self.text)
        self.assertIn('integrity status message:[[:space:]]*VALID', self.text)

    def test_provisioning_state_accepts_reviewed_cubeprogrammer_labels_only(self):
        match = re.search(
            r"^task_provisioning_state_pattern='([^']+)'$",
            self.text,
            re.MULTILINE)
        self.assertIsNotNone(match)
        pattern = match.group(1)
        accepted = (
            "PRODUCT_STATE: 0x17 (Provisioning)",
            "PRODUCT_STATE: 0x17 (Provisioning, Debug partially opened "
            "(only non-secure)) ",
        )
        rejected = (
            "PRODUCT_STATE: 0xED (Open)",
            "PRODUCT_STATE: 0x72 (Closed)",
            "PRODUCT_STATE: 0x17 (Provisioning, unknown debug policy)",
        )
        for line in accepted:
            with self.subTest(accepted=line):
                subprocess.run(
                    ["grep", "-Eq", pattern], input=line + "\n", text=True,
                    check=True)
        for line in rejected:
            with self.subTest(rejected=line):
                result = subprocess.run(
                    ["grep", "-Eq", pattern], input=line + "\n", text=True,
                    check=False)
                self.assertNotEqual(result.returncode, 0)

        detect_start = self.text.index("detect_lifecycle()")
        detect_end = self.text.index("wait_provisioning_rss_ready()", detect_start)
        wait_end = self.text.index("enter_provisioning()", detect_end)
        self.assertIn('provisioning_rss_options_are_exact',
                      self.text[detect_start:detect_end])
        self.assertIn('provisioning_rss_options_are_exact',
                      self.text[detect_end:wait_end])

    def test_post_discovery_reset_truth_table_is_output_authoritative(self):
        exact = (
            "Device ID   : 0x484\n"
            "SFSP Version: v2.5.0\n"
            "PRODUCT_STATE: 0x17 (Provisioning, Debug partially opened "
            "(only non-secure)) \n"
        )
        # HWRSTPULSE is a pulse followed by a connection attempt. Its return
        # code can therefore be either zero or DEV_CONNECT_ERR after the pulse;
        # the fresh exact RSS read is the authoritative postcondition.
        for reset_status in (0, 1):
            with self.subTest(reset_status=reset_status, evidence="exact"):
                result = self.run_shell_predicate(
                    ("provisioning_rss_options_are_exact",),
                    'provisioning_rss_options_are_exact "${ECU_TEST_TEXT}"',
                    exact,
                    include_state_pattern=True)
                self.assertEqual(result.returncode, 0, result.stderr)

        invalid = {
            "device": exact.replace("0x484", "0x485", 1),
            "sfsp": exact.replace("v2.5.0", "v2.4.0", 1),
            "state": exact.replace("0x17", "0x72", 1),
            "label": exact.replace("only non-secure", "unknown policy", 1),
        }
        for reset_status in (0, 1):
            for field, sample in invalid.items():
                with self.subTest(
                        reset_status=reset_status, bad_evidence=field):
                    result = self.run_shell_predicate(
                        ("provisioning_rss_options_are_exact",),
                        'provisioning_rss_options_are_exact "${ECU_TEST_TEXT}"',
                        sample,
                        include_state_pattern=True)
                    self.assertNotEqual(result.returncode, 0)

        fresh_start = self.text.index("run_fresh_preclose_da()")
        fresh_end = self.text.index("verify_preclose_da_log()", fresh_start)
        fresh_body = self.text[fresh_start:fresh_end]
        reset = fresh_body.index("mode=HWRSTPULSE")
        ready = fresh_body.index(
            'wait_provisioning_rss_ready "CLOSED lifecycle write"', reset)
        reset_to_ready = fresh_body[reset:ready]
        self.assertIn("task_reset_status=$?", reset_to_ready)
        self.assertNotRegex(
            reset_to_ready,
            r'(if|\[\[).*task_reset_status|return\s+[1-9]')

    def test_da_discovery_requires_all_identity_and_integrity_fields(self):
        common = [
            "discovery: target ID.......................:0x484",
            "discovery: SDA version.....................:2.4.0",
            "discovery: Vendor ID.......................:STMicroelectronics",
            "discovery: cryptosystems...................:Ecdsa-P256 SHA256",
            "discovery: ST provisioning integrity status:0xEAEAEAEA",
            "discovery: ST provisioning integrity status message:VALID",
        ]
        provisioning = "\n".join(
            common + [
                "discovery: PSA lifecycle...................:"
                "ST_LIFECYCLE_PROVISIONING",
            ]) + "\n"
        closed = provisioning.replace(
            "ST_LIFECYCLE_PROVISIONING", "ST_LIFECYCLE_CLOSED")
        functions = (
            "da_discovery_common_is_exact",
            "provisioning_da_discovery_is_exact",
            "closed_da_discovery_is_exact",
        )
        provision_result = self.run_shell_predicate(
            functions,
            'provisioning_da_discovery_is_exact "${ECU_TEST_TEXT}"',
            provisioning)
        self.assertEqual(provision_result.returncode, 0,
                         provision_result.stderr)
        closed_result = self.run_shell_predicate(
            functions,
            'closed_da_discovery_is_exact "${ECU_TEST_TEXT}"',
            closed)
        self.assertEqual(closed_result.returncode, 0, closed_result.stderr)

        wrong_lifecycle = self.run_shell_predicate(
            functions,
            'provisioning_da_discovery_is_exact "${ECU_TEST_TEXT}"',
            closed)
        self.assertNotEqual(wrong_lifecycle.returncode, 0)
        for index, field in enumerate(common):
            sample = "\n".join(
                common[:index] + common[index + 1:] + [
                    "discovery: PSA lifecycle...................:"
                    "ST_LIFECYCLE_PROVISIONING",
                ]) + "\n"
            with self.subTest(missing=field):
                result = self.run_shell_predicate(
                    functions,
                    'provisioning_da_discovery_is_exact '
                    '"${ECU_TEST_TEXT}"',
                    sample)
                self.assertNotEqual(result.returncode, 0)

        contradictory = provisioning.replace(
            "status:0xEAEAEAEA", "status:0xF5F5F5F5")
        result = self.run_shell_predicate(
            functions,
            'provisioning_da_discovery_is_exact "${ECU_TEST_TEXT}"',
            contradictory)
        self.assertNotEqual(result.returncode, 0)

    def test_da_fallback_and_verifiers_use_strict_predicates(self):
        detect_start = self.text.index("detect_lifecycle()")
        detect_end = self.text.index("wait_provisioning_rss_ready()", detect_start)
        detect = self.text[detect_start:detect_end]
        self.assertGreaterEqual(detect.count("task_status=$?"), 2)
        self.assertIn("provisioning_da_discovery_is_exact", detect)
        self.assertIn("closed_da_discovery_is_exact", detect)

        preclose_start = self.text.index("verify_preclose_da_log()")
        preclose_end = self.text.index("verify_closed_da()", preclose_start)
        preclose = self.text[preclose_start:preclose_end]
        self.assertIn("provisioning_da_discovery_is_exact", preclose)
        self.assertNotRegex(
            preclose,
            r'integrity status[^\n]+\|[^\n]+VALID')

        closed_start = preclose_end
        closed_end = self.text.index("request_closed()", closed_start)
        closed_verify = self.text[closed_start:closed_end]
        self.assertIn("closed_da_discovery_is_exact", closed_verify)

    def test_final_open_ob_check_and_volatile_halt_are_unconditional(self):
        start = self.text.index("continue_open_boot_install()")
        end = self.text.index("detect_lifecycle()", start)
        body = self.text[start:end]
        ob_check = body.rindex(
            'check_open_target > "${task_transaction_dir}/option-bytes-pre-provisioning.txt"')
        halt = body.rindex(
            '"${task_cli}" "${task_connect_reset[@]}" -halt -score')
        halted_phase_test = body.rindex(
            'if ! phase_done release_closed_halted; then')
        self.assertLess(ob_check, halt)
        self.assertLess(halt, halted_phase_test)
        for field in ("WRPSGn2", "HDP2_STRT", "HDP2_END"):
            self.assertIn(field, body)

    def test_every_closed_write_requires_fresh_preclose_discovery(self):
        start = self.text.index("request_closed()")
        end = self.text.index("continue_provisioning()", start)
        body = self.text[start:end]
        self.assertLess(body.index("verify_preclose_da"),
                        body.index("-ob PRODUCT_STATE=0x72"))
        fresh_start = self.text.index("run_fresh_preclose_da()")
        fresh_end = self.text.index("verify_preclose_da_log()", fresh_start)
        fresh_body = self.text[fresh_start:fresh_end]
        self.assertIn('debugauth=2', fresh_body)
        discovery = fresh_body.index('debugauth=2')
        reset = fresh_body.index('mode=HWRSTPULSE', discovery)
        ready = fresh_body.index(
            'wait_provisioning_rss_ready "CLOSED lifecycle write"', reset)
        self.assertLess(discovery, reset)
        self.assertLess(reset, ready)
        self.assertIn('set +e', fresh_body[discovery:reset])
        self.assertIn('task_reset_status=$?', fresh_body[reset:ready])
        self.assertIn('set -e', fresh_body[reset:ready])
        self.assertNotIn(
            '"${task_connect_hotplug[@]}" -hardRst', fresh_body[discovery:])
        self.assertIn('wait_provisioning_rss_ready "CLOSED lifecycle write"',
                      fresh_body)

    def test_closed_write_follows_all_durable_evidence_checks(self):
        start = self.text.index("request_closed()")
        end = self.text.index("continue_provisioning()", start)
        body = self.text[start:end]
        write = body.index("-ob PRODUCT_STATE=0x72")
        for required in (
                "da_config_complete", "oemirot_config_complete",
                "oemirot_data_complete", "preclose_da_integrity_verified",
                "verify_obkey_evidence da_config",
                "verify_obkey_evidence oemirot_config",
                "verify_obkey_evidence oemirot_data",
                "verify_preclose_da"):
            with self.subTest(required=required):
                self.assertLess(body.index(required), write)


if __name__ == "__main__":
    unittest.main()
