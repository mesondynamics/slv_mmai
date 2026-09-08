#!/usr/bin/env bash
set -euo pipefail

# Build a Full Regression recovery package only from the immutable evidence
# directory created by finalize_oemirot_closed.sh. The pre-CLOSED Flash image
# is provenance; recovery installs the pinned ReleaseOpen loader and signed
# 1.0.15 initial pair while preserving the captured application records.

task_source_dir="${1:-}"
task_label="${2:-pre-regression}"
task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_backup_root="${ECU_BACKUP_DIR:-${task_project_dir}/artifacts/device-backups}"
task_probe="${STLINK_SERIAL:-066BFF565456857187210935}"
task_purpose="${ECU_RECOVERY_PURPOSE:-Recover the reviewed ECU through authenticated Full Regression}"
task_version="1.0.15"
task_firmware_dir="${task_project_dir}/artifacts/firmware/${task_version}"
# Allow an explicitly selected immutable legacy archive when the working
# build directory belongs to another board. All original size/SHA/UID gates
# below remain mandatory; these variables do not select a new recovery target.
task_release_open="${ECU_LEGACY_RELEASE_OPEN_IMAGE:-${task_project_dir}/Bootloader/OEMiROT/build/ReleaseOpen/ECU_OEMiROT.bin}"
task_release_closed="${ECU_LEGACY_RELEASE_CLOSED_IMAGE:-${task_project_dir}/Bootloader/OEMiROT/build/ReleaseClosed/ECU_OEMiROT.bin}"
task_secure_image="${task_firmware_dir}/secure-initial.bin"
task_nonsecure_image="${task_firmware_dir}/nonsecure-initial.bin"
task_ota_package="${task_firmware_dir}/roller-ecu-${task_version}.recu"
task_closed_verifier="${task_script_dir}/verify_closed_preflight.py"
task_backup_verifier="${task_script_dir}/verify_open_loader_backup.py"
task_pairing_verifier="${task_script_dir}/verify_pairing_store.py"

if [[ -z "${task_source_dir}" ]]; then
  printf 'Usage: %s CLOSED_TRANSACTION_DIR [OUTPUT_LABEL]\n' "$0" >&2
  exit 2
fi
if [[ ! -d "${task_source_dir}" ]]; then
  printf '%s\n' \
    'Legacy SOURCE_BEFORE_CLOSED_PREFIX backups are retained as evidence but are' \
    'not accepted for destructive recovery. Safely migrate by using the exact' \
    '...-closed-transition directory produced by finalize_oemirot_closed.sh;' \
    'it must contain phase.txt, manifest.json, full-flash.bin,' \
    'secure-persistent.bin, pairing-store.bin, and option-bytes-before.txt.' >&2
  exit 2
fi
task_source_dir="$(realpath -e -- "${task_source_dir}")"

if [[ ! "${task_label}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ ]] ||
   [[ ! "${task_probe}" =~ ^[A-Za-z0-9]{1,64}$ ]]; then
  printf 'Recovery label or probe serial is not a safe canonical identifier.\n' >&2
  exit 2
fi

task_source_flash="${task_source_dir}/full-flash.bin"
task_source_persistent="${task_source_dir}/secure-persistent.bin"
task_source_options="${task_source_dir}/option-bytes-before.txt"
task_source_journal="${task_source_dir}/phase.txt"
task_source_manifest="${task_source_dir}/manifest.json"
for task_file in \
    "${task_source_flash}" "${task_source_persistent}" \
    "${task_source_options}" "${task_source_journal}" \
    "${task_source_manifest}" "${task_source_dir}/programmer-mcu-uid.bin" \
    "${task_source_dir}/pairing-store.bin" \
    "${task_source_dir}/primary-audit.json" \
    "${task_release_open}" "${task_release_closed}" \
    "${task_secure_image}" "${task_nonsecure_image}" "${task_ota_package}" \
    "${task_closed_verifier}" "${task_backup_verifier}" \
    "${task_pairing_verifier}"; do
  if [[ ! -f "${task_file}" || -L "${task_file}" ]]; then
    printf 'Missing, non-regular, or symbolic-link recovery input: %s\n' \
      "${task_file}" >&2
    exit 2
  fi
done

# Bind every source file, the exact target, ReleaseClosed, transaction UUID,
# and the complete fail-closed lifecycle journal before packaging anything.
python3 "${task_closed_verifier}" transaction-manifest \
  "${task_source_dir}" --probe "${task_probe}"
python3 "${task_closed_verifier}" phase-journal "${task_source_journal}"
if [[ "$(tail -n 1 -- "${task_source_journal}")" != \
      "product_state_closed_verified" ]]; then
  printf 'Source transaction has not reached verified CLOSED.\n' >&2
  exit 2
fi
python3 "${task_closed_verifier}" programmer-uid \
  "${task_source_dir}/programmer-mcu-uid.bin"
python3 "${task_closed_verifier}" installed-primary-audit \
  "${task_source_dir}/primary-audit.json"

verify_pairing_store_evidence() {
  local task_root="$1"
  python3 - "${task_root}/full-flash.bin" \
      "${task_root}/secure-persistent.bin" \
      "${task_root}/pairing-store.bin" <<'PY'
import pathlib
import sys

flash, persistent, pairing = (
    pathlib.Path(value).read_bytes() for value in sys.argv[1:])
if len(flash) != 0x200000:
    raise SystemExit("recovery full Flash evidence is not exactly 2 MiB")
if len(persistent) != 0x20000:
    raise SystemExit("recovery persistent evidence is not exactly 0x20000 bytes")
if len(pairing) != 0x4000:
    raise SystemExit("recovery pairing-store evidence is not exactly 0x4000 bytes")
if flash[0xE0000:0xE0000 + 0x4000] != pairing:
    raise SystemExit(
        "pairing-store differs from physical full-Flash offset 0xE0000")
if persistent[:0x4000] != pairing:
    raise SystemExit(
        "pairing-store differs from the independent persistent readback")
PY
  python3 "${task_pairing_verifier}" dual \
    "${task_root}/pairing-store.bin"
}

verify_pairing_store_evidence "${task_source_dir}"

# Independently parse the captured Flash and both installable initial images.
# Static hashes prevent a later rebuild from silently changing a recovery
# binary even if its human-readable version remains 1.0.15.
PYTHONPATH="${task_script_dir}" python3 - \
    "${task_source_flash}" "${task_release_open}" "${task_release_closed}" \
    "${task_secure_image}" "${task_nonsecure_image}" "${task_ota_package}" <<'PY'
import hashlib
import pathlib
import sys

from verify_open_loader_backup import (
    NONSECURE_OFFSET,
    NONSECURE_SIZE,
    SECURE_OFFSET,
    SECURE_SIZE,
    ReleaseIdentity,
    parse_primary,
    validate_installed_flash,
)

paths = [pathlib.Path(value) for value in sys.argv[1:]]
flash, release_open, release_closed, secure, nonsecure, ota = paths
expected = {
    release_open: (52276, "5fdaaa3f5e2d2a6c01ed527282ba08b3d8d5e579c61ec3c4e097cdf2c0fd7761"),
    release_closed: (52292, "4073a9faa15eed7ca6014f168ae91efc35ba1e26d8e74651552c9a0a3d8ef884"),
    secure: (196608, "1a50b9106f798fb64c1a766d81d194b8829e07497941b4af7efe41bb38b98d75"),
    nonsecure: (327680, "4f96d510f06cda3de1a88de8be5a7516b09f1ed539b05e0d6be2eeaaa571d8ed"),
    ota: (525371, "9980acbf248f242c77c7044f48d627ca862d6714221f5835c0a21b7a24bf9e54"),
}
for path, (size, digest) in expected.items():
    data = path.read_bytes()
    if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
        raise SystemExit(f"stale or unreviewed Full Regression input: {path}")

identity = ReleaseIdentity(1, 0, 15, 0, 15)
captured_secure, captured_nonsecure, _, _ = validate_installed_flash(
    flash.read_bytes(), secure.parent)
if captured_secure.identity != identity or captured_nonsecure.identity != identity:
    raise SystemExit("captured Flash is not the confirmed 1.0.15+0/counter15 pair")
secure_report = parse_primary(
    "secure-initial", secure.read_bytes(), SECURE_OFFSET, SECURE_SIZE)
nonsecure_report = parse_primary(
    "nonsecure-initial", nonsecure.read_bytes(), NONSECURE_OFFSET, NONSECURE_SIZE)
if secure_report.identity != identity or nonsecure_report.identity != identity:
    raise SystemExit("signed recovery initial images are not 1.0.15+0/counter15")
print("Pinned ReleaseOpen/ReleaseClosed, OTA, and signed initial pair verified")
PY

task_timestamp="$(date -u +'%Y%m%dT%H%M%SZ')"
task_created_utc="$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
task_output_dir="${task_backup_root}/stm32h563-${task_probe}-${task_timestamp}-${task_label}"
umask 077
mkdir -p -- "${task_backup_root}"
if [[ -e "${task_output_dir}" ]]; then
  printf 'Recovery output already exists: %s\n' "${task_output_dir}" >&2
  exit 2
fi
mkdir -- "${task_output_dir}"
install -m 600 -- "${task_source_flash}" "${task_output_dir}/full-flash.bin"
install -m 600 -- "${task_source_persistent}" "${task_output_dir}/secure-persistent.bin"
install -m 600 -- "${task_source_options}" "${task_output_dir}/before-closed-option-bytes.txt"
install -m 600 -- "${task_source_journal}" "${task_output_dir}/closed-transition-phase.txt"
install -m 600 -- "${task_source_manifest}" "${task_output_dir}/closed-transaction-manifest.json"
install -m 600 -- "${task_source_dir}/programmer-mcu-uid.bin" \
  "${task_output_dir}/programmer-mcu-uid.bin"
install -m 600 -- "${task_source_dir}/pairing-store.bin" \
  "${task_output_dir}/pairing-store.bin"
install -m 600 -- "${task_source_dir}/primary-audit.json" \
  "${task_output_dir}/primary-audit.json"

python3 - "${task_output_dir}" "${task_created_utc}" "${task_probe}" \
    "${task_purpose}" "${task_release_open}" "${task_release_closed}" \
    "${task_secure_image}" "${task_nonsecure_image}" "${task_ota_package}" <<'PY'
import hashlib
import json
import os
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
created_utc, probe, purpose = sys.argv[2:5]
release_open, release_closed, secure_image, nonsecure_image, ota_package = (
    pathlib.Path(value) for value in sys.argv[5:10]
)

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

source_manifest_path = root / "closed-transaction-manifest.json"
source_manifest = json.loads(source_manifest_path.read_text(encoding="utf-8"))
transaction_uuid = source_manifest["transaction_uuid"]
if re.fullmatch(r"[0-9a-f-]{36}", transaction_uuid) is None:
    raise SystemExit("source transaction UUID is not canonical")

files = {}
for name, address in (
    ("full-flash.bin", "0x08000000"),
    ("secure-persistent.bin", "0x0C0E0000"),
    ("before-closed-option-bytes.txt", None),
    ("closed-transition-phase.txt", None),
    ("closed-transaction-manifest.json", None),
    ("programmer-mcu-uid.bin", None),
    ("pairing-store.bin", None),
    ("primary-audit.json", None),
):
    path = root / name
    entry = {"size": path.stat().st_size, "sha256": digest(path)}
    if address is not None:
        entry = {"address": address, **entry}
    files[name] = entry

def recovery_input(path):
    return {"size": path.stat().st_size, "sha256": digest(path)}

manifest = {
    # v1/v2 predate an immutable CLOSED transaction; v3 remains the exact
    # historical 1.0.14 format. Only v4 carries the 1.0.15 dual-pairing gate.
    "schema": "roller-ecu-full-regression-backup-v4",
    "created_utc": created_utc,
    "purpose": purpose,
    "persistent_format": (
        "Application-owned Secure Flash records; not hardware DHUK-wrapped "
        "secure storage"
    ),
    "target": {
        "probe_serial": probe,
        "device_id": "0x484",
        "mcu_uid": "003800613434511232383537",
        "atecc608_serial": "0123d47eb2ee0e9bee",
        "product_state": "0x72 CLOSED",
        "da_permission": "0x00004040",
    },
    "capture": {
        "source_schema": "roller-ecu-closed-transition-v2",
        "transaction_uuid": transaction_uuid,
        "transaction_manifest_sha256": digest(source_manifest_path),
        "product_state": "0xED OPEN",
        "relationship": "immutable evidence captured before this verified CLOSED transition",
        "final_phase": "product_state_closed_verified",
    },
    "installed_firmware": {
        "version": "1.0.15",
        "build": 0,
        "security_counter": 15,
        "accepted_sequence": 15,
        "layout_version": 65536,
        "ota_package_sha256": digest(ota_package),
    },
    "files": files,
    "recovery_inputs": {
        "release_open_oemirot": recovery_input(release_open),
        "release_closed_oemirot": recovery_input(release_closed),
        "secure_initial": recovery_input(secure_image),
        "nonsecure_initial": recovery_input(nonsecure_image),
        "ota_package": recovery_input(ota_package),
    },
}
temporary = root / ".manifest.json.tmp"
with temporary.open("w", encoding="utf-8") as stream:
    json.dump(manifest, stream, indent=2, sort_keys=True)
    stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())
os.replace(temporary, root / "manifest.json")
directory_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
try:
    os.fsync(directory_fd)
finally:
    os.close(directory_fd)
PY

# Re-extract and re-run the dual-copy verifier on the copied recovery evidence,
# so packaging cannot silently diverge from the CLOSED source transaction.
verify_pairing_store_evidence "${task_output_dir}"

for task_file in "${task_output_dir}"/*; do
  sync -f "${task_file}"
done
sync -f "${task_output_dir}"
sync -f "${task_backup_root}"
printf '%s\n' "${task_output_dir}"
