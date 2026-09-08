"""Explicit reviewed bench targets; not an IP-only authorization mechanism.

Legacy scripts still default to SN-EJAHGJI. Selecting the new board requires
--device-serial SN-EJAHGJQ; an inconsistent --ecu-ip is rejected, not inferred.
Only GET requests are used by the entry guard, before an actuator client exists.
"""
from dataclasses import dataclass

from verify_pairing_store import REVIEWED_IDENTITIES


@dataclass(frozen=True)
class BenchTarget:
    serial: str
    ecu_ip: str
    domain_ip: str


TARGETS = {
    "SN-EJAHGJI": BenchTarget("SN-EJAHGJI", "172.16.0.11", "172.16.0.12"),
    "SN-EJAHGJQ": BenchTarget("SN-EJAHGJQ", "172.16.0.21", "172.16.0.22"),
}

# Accepted OTA sequence is part of the reviewed installed state, not inferred
# from the address or from whichever release is newest in a build directory.
ACCEPTED_SEQUENCES = {"SN-EJAHGJI": 20, "SN-EJAHGJQ": 22}


def add_target_arguments(parser):
    parser.add_argument("--device-serial", choices=tuple(TARGETS), default="SN-EJAHGJI")
    parser.add_argument("--ecu-ip", default=None,
                        help="optional assertion; must match the selected serial")


def resolve_target(serial, ecu_ip=None):
    if serial not in TARGETS:
        raise ValueError("unreviewed bench serial")
    target = TARGETS[serial]
    if ecu_ip is not None and ecu_ip != target.ecu_ip:
        raise ValueError(f"{serial} requires ECU {target.ecu_ip}; refusing {ecu_ip}")
    return target


def security_matches(security, target):
    identity = REVIEWED_IDENTITIES[target.serial]
    expected = dict(mcu_uid="".join(f"{word:08x}" for word in identity.mcu_uid),
                    serial=identity.serial.hex(), config_crc32c=identity.config_crc32c,
                    revision="00006005", config_locked=1, data_locked=1,
                    atecc_result=0, auth_result=0, pairing_generation=1)
    return isinstance(security, dict) and all(security.get(k) == v for k, v in expected.items())


def require_passive_entry(state, target):
    if (state.get("device_serial") != target.serial or state.get("ecu_ip") != target.ecu_ip or
            not security_matches(state.get("security"), target)):
        raise RuntimeError("bench MCU/ATECC identity mismatch; no control was sent")
    diag, status, steering, ota = (state.get(k, {}) for k in
                                   ("diagnostic", "status", "steering_status", "ota"))
    safety = diag.get("safety_status", 0)
    if (not safety & (1 << 22) or safety & ((1 << 1) | (7 << 23) | (1 << 28)) or
            diag.get("requested_relay_mask") != 1 << 22 or
            diag.get("applied_relay_mask") != 1 << 22 or diag.get("valve_fault_flags") != 0 or
            status.get("control_mode") != 0 or
            any(status.get(k) != 0 for k in ("valve_requested_target_ma", "valve_applied_target_ma",
                                             "forward_duty_permille", "reverse_duty_permille")) or
            not all(0 <= status.get(k, 9999) < 50 for k in ("forward_current_ma", "reverse_current_ma")) or
            steering.get("command_enable") != 0 or steering.get("speed_command_permille") != 0 or
            any(ota.get(k) != 0 for k in ("result", "ota_result", "state"))):
        raise RuntimeError("bench requires authenticated, confirmed, IDLE/K12-only zero-output entry")


def passive_preflight(target):
    # Lazy import avoids bringing GUI/protocol dependencies into offline target tests.
    from ecu_readonly_snapshot import capture
    state = capture(target.serial)
    require_passive_entry(state, target)
    return state
