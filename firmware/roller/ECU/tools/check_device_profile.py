#!/usr/bin/env python3
"""Preprocess the actual C headers and verify the reviewed device identities.

Offline only: does not connect to an ECU, modify keys, or write configuration.
The legacy CLOSED/recovery scripts remain pinned to SN-EJAHGJI; this checker
does not grant permission to run them against a different MCU.
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess

PROJECT = Path(__file__).resolve().parents[1]
ARM_BIN = Path(os.environ.get(
    "ARM_GNU_BIN_DIR",
    "/home/plac/.local/share/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin"))
EXPECTED = {
    1: ("SN-EJAHGJI", "172.16.0.11", "172.16.0.12",
        "003800613434511232383537", "8a:ea:b5:00:00:02"),
    2: ("SN-EJAHGJQ", "172.16.0.21", "172.16.0.22",
        "003900443434511232383537", "8a:ea:b5:00:00:03"),
}
SOURCE = '''
#include "NonSecure/App/Inc/ecu_network.h"
#include "Bootloader/OEMiROT/Config/roller_rss_manifest.h"
PROFILE_ID ECU_DEVICE_ID
PROFILE_SERIAL ECU_PRODUCT_SERIAL
PROFILE_IP ECU_IP_ADDRESS_0 ECU_IP_ADDRESS_1 ECU_IP_ADDRESS_2 ECU_IP_ADDRESS_3
PROFILE_DOMAIN ECU_DOMAIN_HOST_ADDRESS_0 ECU_DOMAIN_HOST_ADDRESS_1 ECU_DOMAIN_HOST_ADDRESS_2 ECU_DOMAIN_HOST_ADDRESS_3
PROFILE_UID ROLLER_DEVICE_UID0 ROLLER_DEVICE_UID1 ROLLER_DEVICE_UID2
PROFILE_MAC 0x8A 0xEA 0xB5 0 0 ECU_DEVICE_MAC_OCTET5
PROFILE_SERVICE ECU_SERVICE_HOST_ADDRESS_0 ECU_SERVICE_HOST_ADDRESS_1 ECU_SERVICE_HOST_ADDRESS_2 ECU_SERVICE_HOST_ADDRESS_3
PROFILE_REMOTE ECU_REMOTE_HOST_ADDRESS_0 ECU_REMOTE_HOST_ADDRESS_1 ECU_REMOTE_HOST_ADDRESS_2 ECU_REMOTE_HOST_ADDRESS_3
PROFILE_PORTS ECU_STATUS_PORT ECU_CONTROL_PORT ECU_DIAGNOSTIC_PORT ECU_TELEMETRY_PORT ECU_TUNING_PORT ECU_OTA_PORT
'''


def read_profile(device_id=None):
    command = [str(ARM_BIN / "arm-none-eabi-gcc"), "-E", "-P", "-x", "c",
               "-I", str(PROJECT)]
    if device_id is not None:
        command.append(f"-DECU_DEVICE_ID={int(device_id)}")
    result = subprocess.run(command + ["-"], input=SOURCE, text=True,
                            capture_output=True, check=True)
    fields = dict(re.findall(r"^PROFILE_(\w+)\s+(.+)$", result.stdout, re.M))

    def numbers(key):
        return [int(re.sub(r"[uUlL]+$", "", x), 0) for x in fields[key].split()]

    actual_id = numbers("ID")[0]
    values = (json.loads(fields["SERIAL"]),
              ".".join(map(str, numbers("IP"))),
              ".".join(map(str, numbers("DOMAIN"))),
              "".join(f"{x:08x}" for x in numbers("UID")),
              ":".join(f"{x:02x}" for x in numbers("MAC")))
    if EXPECTED.get(actual_id) != values:
        raise ValueError(f"unreviewed manufacturing profile: {actual_id}: {values}")
    if numbers("SERVICE") != [172, 16, 0, 10] or numbers("REMOTE") != [172, 16, 0, 9]:
        raise ValueError("fleet service/remote reservations changed")
    if numbers("PORTS") != list(range(50001, 50007)):
        raise ValueError("protocol port compatibility changed")
    return dict(zip(("serial", "ecu_ip", "domain_ip", "mcu_uid", "mac"), values),
                device_id=actual_id, service_ip="172.16.0.10", remote_ip="172.16.0.9")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-id", type=int)
    parser.add_argument("--all", action="store_true")
    args = parser.parse_args()
    profiles = [read_profile(i) for i in EXPECTED] if args.all else [read_profile(args.device_id)]
    print(json.dumps(profiles, indent=2))


if __name__ == "__main__":
    main()
