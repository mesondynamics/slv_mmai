#!/usr/bin/env bash
set -euo pipefail

# Create a non-overwriting, self-verifying backup of one exact ECU development
# state. The source archive is authoritative for dirty/untracked content; the
# complete Git bundle and binary patch provide independent provenance. A
# backup is published only after an offline restore drill and final stability
# check both pass.

task_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
task_project_dir="$(cd -- "${task_script_dir}/.." && pwd)"
task_repo_dir="$(git -C "${task_project_dir}" rev-parse --show-toplevel)"
task_backup_root="${ECU_PKI_BACKUP_DIR:-/home/plac/Documents/ECU_PKI}"
task_pki_dir="${ECU_PKI_DIR:-/home/plac/.local/share/roller-ecu-pki}"
task_baseline_dir="${ECU_BASELINE_RECOVERY_DIR:-${task_backup_root}/roller-ecu-recovery-20260830T095026Z}"
task_security_assets_dir="${task_project_dir}/artifacts/security-provisioning"
task_label="development-source"
task_release_version=""
task_staging_dir=""
task_drill_dir=""
umask 077

usage() {
  printf 'Usage: %s [--label SAFE_LABEL] [--release VERSION]\n' "$0" >&2
}

fail() {
  printf 'Development backup refused: %s\n' "$*" >&2
  exit 2
}

cleanup_private_dir() {
  local task_path="$1"
  [[ -n "${task_path}" && -d "${task_path}" ]] || return 0
  case "${task_path}" in
    "${task_backup_root}"/.roller-ecu-*.tmp.*)
      rm -rf -- "${task_path}"
      ;;
    *)
      printf 'Refusing unsafe temporary-directory cleanup: %s\n' \
        "${task_path}" >&2
      ;;
  esac
}

cleanup() {
  cleanup_private_dir "${task_drill_dir:-}"
  cleanup_private_dir "${task_staging_dir:-}"
}
trap cleanup EXIT

tree_fingerprint() {
  # profile=source matches ECU-source.tar.gz. profile=content hashes every
  # entry but ignores modes so public release files can become private 0600.
  local task_root="$1"
  local task_profile="$2"
  python3 - "${task_root}" "${task_profile}" <<'PY'
import hashlib
import os
import stat
import sys
from pathlib import Path

root = Path(sys.argv[1])
profile = sys.argv[2]
if profile not in {"source", "content"}:
    raise SystemExit("unknown tree-fingerprint profile")
excluded_roots = {
    "build", "Secure/build", "NonSecure/build",
    "Bootloader/OEMiROT/build", "artifacts", "mx.scratch",
}


def excluded(relative: str) -> bool:
    if profile != "source":
        return False
    if any(relative == item or relative.startswith(item + "/")
           for item in excluded_roots):
        return True
    parts = relative.split("/")
    return "__pycache__" in parts or parts[-1].endswith(".pyc")


def record(digest, kind: bytes, relative: str,
           mode: int | None, payload: bytes) -> None:
    fields = [kind, os.fsencode(relative)]
    if mode is not None:
        fields.append(f"{mode:04o}".encode("ascii"))
    fields.append(payload)
    for field in fields:
        digest.update(len(field).to_bytes(8, "big"))
        digest.update(field)


def scan(directory: Path, prefix: str, digest) -> None:
    before = directory.stat(follow_symlinks=False)
    entries = sorted(os.scandir(directory), key=lambda item: os.fsencode(item.name))
    for entry in entries:
        relative = entry.name if not prefix else prefix + "/" + entry.name
        if excluded(relative):
            continue
        path = Path(entry.path)
        first = path.stat(follow_symlinks=False)
        mode = stat.S_IMODE(first.st_mode) if profile == "source" else None
        if mode is not None and mode & 0o7000:
            raise SystemExit(f"source contains unsafe special mode bits: {path}")
        if stat.S_ISDIR(first.st_mode):
            record(digest, b"d", relative, mode, b"")
            scan(path, relative, digest)
        elif stat.S_ISREG(first.st_mode):
            content = hashlib.sha256()
            with path.open("rb") as stream:
                while chunk := stream.read(1024 * 1024):
                    content.update(chunk)
            second = path.stat(follow_symlinks=False)
            left = (first.st_dev, first.st_ino, first.st_mode, first.st_size,
                    first.st_mtime_ns, first.st_ctime_ns)
            right = (second.st_dev, second.st_ino, second.st_mode, second.st_size,
                     second.st_mtime_ns, second.st_ctime_ns)
            if left != right:
                raise SystemExit(f"file changed while hashing: {path}")
            record(digest, b"f", relative, mode, content.digest())
        elif stat.S_ISLNK(first.st_mode):
            target = os.fsencode(os.readlink(path))
            second = path.stat(follow_symlinks=False)
            left = (first.st_dev, first.st_ino, first.st_mode,
                    first.st_mtime_ns, first.st_ctime_ns)
            right = (second.st_dev, second.st_ino, second.st_mode,
                     second.st_mtime_ns, second.st_ctime_ns)
            if left != right:
                raise SystemExit(f"symbolic link changed while hashing: {path}")
            record(digest, b"l", relative, mode, target)
        else:
            raise SystemExit(f"unsupported source file type: {path}")
    after = directory.stat(follow_symlinks=False)
    left = (before.st_dev, before.st_ino, before.st_mode,
            before.st_mtime_ns, before.st_ctime_ns)
    right = (after.st_dev, after.st_ino, after.st_mode,
             after.st_mtime_ns, after.st_ctime_ns)
    if left != right:
        raise SystemExit(f"directory changed while hashing: {directory}")


if not root.is_dir() or root.is_symlink():
    raise SystemExit(f"tree root is not a real directory: {root}")
result = hashlib.sha256()
scan(root, "", result)
print(result.hexdigest())
PY
}

validate_regular_tree() {
  local task_root="$1"
  local task_description="$2"
  python3 - "${task_root}" "${task_description}" <<'PY'
import os
import re
import stat
import sys
from pathlib import Path

root = Path(sys.argv[1])
description = sys.argv[2]
safe_name = re.compile(r"[A-Za-z0-9._-]+")
if not root.is_dir() or root.is_symlink():
    raise SystemExit(f"{description} root is not a real directory: {root}")


def visit(directory: Path) -> None:
    for entry in os.scandir(directory):
        if safe_name.fullmatch(entry.name) is None:
            raise SystemExit(
                f"{description} contains an unsafe member name: {entry.path}")
        info = entry.stat(follow_symlinks=False)
        path = Path(entry.path)
        if stat.S_ISDIR(info.st_mode):
            visit(path)
        elif not stat.S_ISREG(info.st_mode):
            raise SystemExit(
                f"{description} contains a symlink or special file: {path}")


visit(root)
print(f"{description}: real directory/regular-file tree verified")
PY
}

git_stream_sha256() {
  sha256sum | cut -d ' ' -f 1
}

capture_git_state() {
  local task_head task_branch task_refs task_index task_status
  task_head="$(git -C "${task_repo_dir}" rev-parse HEAD)"
  task_branch="$(git -C "${task_repo_dir}" branch --show-current)"
  task_refs="$(git -C "${task_repo_dir}" show-ref --head | \
    LC_ALL=C sort | git_stream_sha256)"
  task_index="$(git -C "${task_repo_dir}" ls-files --stage -z | \
    git_stream_sha256)"
  task_status="$(git -C "${task_repo_dir}" status --porcelain=v1 -z -- \
    "${task_project_relative}" | git_stream_sha256)"
  printf 'head=%s\nbranch=%s\nrefs_sha256=%s\nindex_sha256=%s\nstatus_sha256=%s\n' \
    "${task_head}" "${task_branch}" "${task_refs}" "${task_index}" \
    "${task_status}"
}

validate_exact_pki() {
  local task_dir="$1"
  python3 - "${task_dir}" <<'PY'
import os
import stat
import sys
from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

root = Path(sys.argv[1])
required = {
    "da-intermediate-public.pem", "da-intermediate.pem",
    "da-leaf-public.pem", "da-leaf.pem", "da-root-public.pem", "da-root.pem",
    "key-passphrase.txt", "oemirot-auth-ns-public.pem", "oemirot-auth-ns.pem",
    "oemirot-auth-s-public.pem", "oemirot-auth-s.pem",
    "oemirot-encryption-public.pem", "oemirot-encryption.pem",
    "ota-transport-public.der", "ota-transport-public.pem", "ota-transport.pem",
}
entries = {entry.name: entry for entry in os.scandir(root)}
if set(entries) != required:
    raise SystemExit(
        f"PKI member set is not exact; missing={sorted(required-set(entries))}, "
        f"extra={sorted(set(entries)-required)}")
if stat.S_IMODE(root.stat().st_mode) != 0o700:
    raise SystemExit("PKI directory mode must be 0700")
for name, entry in entries.items():
    info = entry.stat(follow_symlinks=False)
    if not stat.S_ISREG(info.st_mode) or entry.is_symlink():
        raise SystemExit(f"PKI member is not a real regular file: {name}")
    if stat.S_IMODE(info.st_mode) != 0o600:
        raise SystemExit(f"PKI member mode must be 0600: {name}")
password = (root / "key-passphrase.txt").read_bytes().strip()
if not password:
    raise SystemExit("PKI passphrase is empty")
pairs = (
    "da-intermediate", "da-leaf", "da-root", "oemirot-auth-ns",
    "oemirot-auth-s", "oemirot-encryption", "ota-transport",
)
public_keys = {}
for stem in pairs:
    private_key = serialization.load_pem_private_key(
        (root / f"{stem}.pem").read_bytes(), password=password)
    public_key = serialization.load_pem_public_key(
        (root / f"{stem}-public.pem").read_bytes())
    if (not isinstance(private_key, ec.EllipticCurvePrivateKey)
            or not isinstance(public_key, ec.EllipticCurvePublicKey)
            or private_key.curve.name != "secp256r1"
            or public_key.curve.name != "secp256r1"):
        raise SystemExit(f"PKI key pair is not P-256: {stem}")
    if private_key.public_key().public_numbers() != public_key.public_numbers():
        raise SystemExit(f"PKI private/public key mismatch: {stem}")
    public_keys[stem] = public_key
der_key = serialization.load_der_public_key(
    (root / "ota-transport-public.der").read_bytes())
if (not isinstance(der_key, ec.EllipticCurvePublicKey)
        or der_key.public_numbers() != public_keys["ota-transport"].public_numbers()):
    raise SystemExit("OTA transport DER/PEM public keys do not match")
print("Exact encrypted PKI set and all seven P-256 key pairs verified")
PY
}

validate_exact_security_assets() {
  local task_dir="$1"
  python3 - "${task_dir}" <<'PY'
import os
import stat
import sys
from pathlib import Path

root = Path(sys.argv[1])
required = {
    "DA_Config.obk", "OEMiRoT_Config.obk", "OEMiRoT_Data.obk",
    "cert-intermediate.b64", "cert-leaf-chain.b64", "cert-leaf.b64",
    "cert-root.b64", "security-assets.json",
}
entries = {entry.name: entry for entry in os.scandir(root)}
if set(entries) != required:
    raise SystemExit(
        f"security-provisioning member set is not exact; "
        f"missing={sorted(required-set(entries))}, extra={sorted(set(entries)-required)}")
for name, entry in entries.items():
    info = entry.stat(follow_symlinks=False)
    if not stat.S_ISREG(info.st_mode) or entry.is_symlink():
        raise SystemExit(f"security asset is not a real regular file: {name}")
print("Exact security-provisioning member set verified")
PY
}

validate_release() {
  local task_dir="$1"
  local task_version="$2"
  local task_public_key="$3"
  PYTHONDONTWRITEBYTECODE=1 python3 - \
      "${task_project_dir}" "${task_dir}" "${task_version}" \
      "${task_public_key}" <<'PY'
import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

from cryptography.hazmat.primitives import hashes, hmac, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

project = Path(sys.argv[1])
root = Path(sys.argv[2])
version = sys.argv[3]
public_key = Path(sys.argv[4])
sys.path.insert(0, str(project))
from tools.ethernet_ota import load_package  # noqa: E402
from tools.package_firmware import IMGTOOL  # noqa: E402
from tools.verify_closed_ota_readback import (  # noqa: E402
    _parse_image_record,
    _validate_reference_trailer,
)

package_name = f"roller-ecu-{version}.recu"
required = {
    "metadata.json", "secure-initial.bin", "nonsecure-initial.bin", package_name,
}
entries = {entry.name: entry for entry in os.scandir(root)}
if set(entries) != required:
    raise SystemExit(
        f"release member set is not exact; missing={sorted(required-set(entries))}, "
        f"extra={sorted(set(entries)-required)}")
for name, entry in entries.items():
    info = entry.stat(follow_symlinks=False)
    if not stat.S_ISREG(info.st_mode) or entry.is_symlink():
        raise SystemExit(f"release member is not a real regular file: {name}")


def strict_object(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError(f"duplicate JSON field: {name}")
        result[name] = value
    return result


metadata_bytes = (root / "metadata.json").read_bytes()
metadata = json.loads(metadata_bytes.decode("utf-8"), object_pairs_hook=strict_object)
if type(metadata) is not dict or metadata.get("version") != version:
    raise SystemExit("release metadata version does not match its directory")
_begin, secure, nonsecure, signed_metadata = load_package(
    root / package_name, public_key)
if signed_metadata != metadata:
    raise SystemExit("external and signed OTA metadata differ")
with zipfile.ZipFile(root / package_name, "r") as archive:
    if archive.read("metadata.json") != metadata_bytes:
        raise SystemExit("external and packaged metadata bytes differ")
if (root / "secure-initial.bin").stat().st_size != metadata["secure_size"]:
    raise SystemExit("secure initial image size differs from signed metadata")
if (root / "nonsecure-initial.bin").stat().st_size != metadata["nonsecure_size"]:
    raise SystemExit("nonsecure initial image size differs from signed metadata")
if len(secure) != metadata["secure_size"] or len(nonsecure) != metadata["nonsecure_size"]:
    raise SystemExit("signed OTA payload sizes differ from metadata")

try:
    major, minor, revision = (int(value) for value in version.split("."))
except ValueError as exc:
    raise SystemExit("release version is not canonical X.Y.Z") from exc
identity = (major, minor, revision, 0, metadata["security_counter"])
pki = public_key.parent
imgtool_env = dict(os.environ)
compat = str(project / "tools/imgtool_compat")
imgtool_env["PYTHONPATH"] = compat + os.pathsep + imgtool_env.get("PYTHONPATH", "")


def decrypt_update(name, image, encryption_key):
    _magic, _load, header_size, protected_size, image_size, _flags = \
        struct.unpack_from("<IIHHII", image, 0)
    unprotected_offset = header_size + image_size + protected_size
    tlv_magic, tlv_total = struct.unpack_from(
        "<HH", image, unprotected_offset)
    if tlv_magic != 0x6907 or tlv_total < 4 or \
            unprotected_offset + tlv_total > len(image):
        raise SystemExit(f"invalid update TLV area: {name}")
    cursor = unprotected_offset + 4
    encrypted_keys = []
    while cursor < unprotected_offset + tlv_total:
        tlv_type, tlv_length = struct.unpack_from("<HH", image, cursor)
        cursor += 4
        end = cursor + tlv_length
        if end > unprotected_offset + tlv_total:
            raise SystemExit(f"truncated update TLV: {name}")
        if tlv_type == 0x32:
            encrypted_keys.append(image[cursor:end])
        cursor = end
    if cursor != unprotected_offset + tlv_total or \
            len(encrypted_keys) != 1 or len(encrypted_keys[0]) != 113:
        raise SystemExit(f"update has no exact EC256 encryption TLV: {name}")

    value = encrypted_keys[0]
    ephemeral = ec.EllipticCurvePublicKey.from_encoded_point(
        ec.SECP256R1(), value[:65])
    shared = encryption_key.exchange(ec.ECDH(), ephemeral)
    derived = HKDF(
        algorithm=hashes.SHA256(), length=48, salt=None,
        info=b"MCUBoot_ECIES_v1").derive(shared)
    authenticator = hmac.HMAC(derived[16:], hashes.SHA256())
    authenticator.update(value[97:])
    authenticator.verify(value[65:97])
    key_decryptor = Cipher(
        algorithms.AES(derived[:16]), modes.CTR(bytes(16))).decryptor()
    image_key = key_decryptor.update(value[97:]) + key_decryptor.finalize()
    payload_decryptor = Cipher(
        algorithms.AES(image_key), modes.CTR(bytes(16))).decryptor()
    result = bytearray(image)
    result[header_size:header_size + image_size] = \
        payload_decryptor.update(
            image[header_size:header_size + image_size]) + \
        payload_decryptor.finalize()
    return bytes(result)


password = (pki / "key-passphrase.txt").read_bytes().strip()
encryption_key = serialization.load_pem_private_key(
    (pki / "oemirot-encryption.pem").read_bytes(), password=password)
if not isinstance(encryption_key, ec.EllipticCurvePrivateKey) or \
        encryption_key.curve.name != "secp256r1":
    raise SystemExit("OEMiROT encryption private key is not P-256")

with tempfile.TemporaryDirectory(prefix="ecu-backup-release-") as temp_name:
  temp = Path(temp_name)
  for stem, key_name, expected_size, update in (
          ("secure", "oemirot-auth-s-public.pem", metadata["secure_size"], secure),
          ("nonsecure", "oemirot-auth-ns-public.pem",
           metadata["nonsecure_size"], nonsecure),
  ):
    initial_name = f"{stem}-initial.bin"
    update_name = f"{stem}-update.bin"
    initial_path = root / initial_name
    initial = initial_path.read_bytes()
    initial_record = _parse_image_record(
        initial_name, initial, expected_size, 0,
        expected_identity=identity, expected_version=version)
    update_record = _parse_image_record(
        update_name, update, expected_size, 4,
        expected_identity=identity, expected_version=version)
    if (initial_record.header_size, initial_record.image_size,
            initial_record.protected_size) != \
            (update_record.header_size, update_record.image_size,
             update_record.protected_size):
        raise SystemExit(f"initial/update MCUboot layouts differ: {stem}")
    _validate_reference_trailer(
        initial_name, initial, confirmed=True,
        record_end=initial_record.record_end)
    _validate_reference_trailer(
        update_name, update, confirmed=False,
        record_end=update_record.record_end)
    decrypted_update = decrypt_update(update_name, update, encryption_key)
    payload_start = initial_record.header_size
    payload_end = payload_start + initial_record.image_size
    initial_header = bytearray(initial[:payload_start])
    update_header = bytearray(decrypted_update[:payload_start])
    struct.pack_into("<I", update_header, 16, 0)
    if initial_header != update_header:
        raise SystemExit(f"initial/update MCUboot headers differ beyond flags: {stem}")
    if initial[payload_start:payload_end] != \
            decrypted_update[payload_start:payload_end]:
        raise SystemExit(f"initial/update plaintext firmware differs: {stem}")
    protected_end = payload_end + initial_record.protected_size
    if initial[payload_end:protected_end] != \
            decrypted_update[payload_end:protected_end]:
        raise SystemExit(f"initial/update signed protected TLVs differ: {stem}")
    decrypted_path = temp / f"{stem}-update-decrypted.bin"
    decrypted_path.write_bytes(decrypted_update)
    decrypted_path.chmod(0o600)
    for form, image_path in (("initial", initial_path),
                             ("update", decrypted_path)):
      result = subprocess.run(
        [sys.executable, str(IMGTOOL), "verify", "-k", str(pki / key_name),
         str(image_path)],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", env=imgtool_env,
      )
      if result.returncode != 0 or \
              "Image was correctly validated" not in result.stdout:
        raise SystemExit(
            f"{form} OEM image signature verification failed: {stem}")
print(
    f"Release {version}: exact four-file set, paired plaintext identities, "
    "initial CONFIRMED/update TEST trailers, OEM image signatures, payload "
    "hashes, and OTA ECDSA signature verified")
PY
}

write_file_modes() {
  local task_root="$1"
  python3 - "${task_root}" <<'PY'
import json
import os
import stat
import sys
from pathlib import Path

root = Path(sys.argv[1])
records = []


def visit(path: Path, relative: str) -> None:
    info = path.stat(follow_symlinks=False)
    if stat.S_ISDIR(info.st_mode):
        kind = "directory"
    elif stat.S_ISREG(info.st_mode):
        kind = "file"
    else:
        raise SystemExit(f"backup contains unsupported file type: {relative}")
    records.append({"path": relative, "type": kind,
                    "mode": f"{stat.S_IMODE(info.st_mode):04o}"})
    if kind == "directory":
        for entry in sorted(os.scandir(path), key=lambda item: os.fsencode(item.name)):
            child = entry.name if relative == "." else relative + "/" + entry.name
            visit(Path(entry.path), child)


visit(root, ".")
for item in records:
    print(json.dumps(item, ensure_ascii=True, sort_keys=True,
                     separators=(",", ":")))
PY
}

verify_file_modes() {
  local task_root="$1"
  python3 - "${task_root}" <<'PY'
import json
import os
import stat
import sys
from pathlib import Path

root = Path(sys.argv[1])
expected = {}
for line in (root / "FILE_MODES.jsonl").read_text(encoding="ascii").splitlines():
    item = json.loads(line)
    if set(item) != {"path", "type", "mode"} or item["path"] in expected:
        raise SystemExit("invalid or duplicate FILE_MODES.jsonl record")
    expected[item["path"]] = (item["type"], item["mode"])
actual = {}


def visit(path: Path, relative: str) -> None:
    info = path.stat(follow_symlinks=False)
    if stat.S_ISDIR(info.st_mode):
        kind = "directory"
    elif stat.S_ISREG(info.st_mode):
        kind = "file"
    else:
        raise SystemExit(f"backup contains unsupported file type: {relative}")
    mode = f"{stat.S_IMODE(info.st_mode):04o}"
    required = "0700" if kind == "directory" else "0600"
    if mode != required:
        raise SystemExit(
            f"unsafe backup mode for {relative}: {mode}, expected {required}")
    actual[relative] = (kind, mode)
    if kind == "directory":
        for entry in os.scandir(path):
            child = entry.name if relative == "." else relative + "/" + entry.name
            visit(Path(entry.path), child)


visit(root, ".")
if actual != expected:
    raise SystemExit("FILE_MODES.jsonl does not exactly describe the backup tree")
print(f"Backup type/mode manifest verified: {len(actual)} entries")
PY
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --label)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      task_label="$2"
      shift 2
      ;;
    --release)
      [[ $# -ge 2 ]] || { usage; exit 2; }
      task_release_version="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage
      exit 2
      ;;
  esac
done

[[ "${task_label}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ ]] || \
  fail "backup label is not a safe canonical identifier: ${task_label}"
if [[ -n "${task_release_version}" ]] &&
   [[ ! "${task_release_version}" =~ ^[0-9]+[.][0-9]+[.][0-9]+$ ]]; then
  fail "release version must use X.Y.Z form: ${task_release_version}"
fi
for task_command in chmod cp find flock git mktemp mv python3 realpath \
    sha256sum stat sync tar; do
  command -v "${task_command}" >/dev/null 2>&1 || \
    fail "required command is unavailable: ${task_command}"
done
for task_dir in "${task_project_dir}" "${task_repo_dir}" \
    "${task_backup_root}" "${task_pki_dir}" "${task_baseline_dir}" \
    "${task_security_assets_dir}"; do
  [[ -d "${task_dir}" && ! -L "${task_dir}" ]] || \
    fail "missing, non-directory, or symbolic-link input: ${task_dir}"
done
[[ -f "${task_baseline_dir}/MANIFEST.sha256" &&
   ! -L "${task_baseline_dir}/MANIFEST.sha256" ]] || \
  fail "baseline recovery manifest is missing or unsafe: ${task_baseline_dir}/MANIFEST.sha256"

task_project_dir="$(realpath -e -- "${task_project_dir}")"
task_repo_dir="$(realpath -e -- "${task_repo_dir}")"
task_backup_root="$(realpath -e -- "${task_backup_root}")"
task_pki_dir="$(realpath -e -- "${task_pki_dir}")"
task_baseline_dir="$(realpath -e -- "${task_baseline_dir}")"
task_security_assets_dir="$(realpath -e -- "${task_security_assets_dir}")"
[[ "${task_security_assets_dir}" == \
   "${task_project_dir}/artifacts/security-provisioning" ]] || \
  fail "security-provisioning path escapes the ECU project artifacts directory"
case "${task_project_dir}/" in
  "${task_repo_dir}/"*) ;;
  *) fail "ECU project is not inside its Git repository" ;;
esac
[[ "${task_project_dir}" != "${task_repo_dir}" ]] || \
  fail "ECU project must be a protected subdirectory of the repository"
case "${task_backup_root}/" in
  "${task_project_dir}/"*)
    fail "backup root must not be inside the ECU project: ${task_backup_root}"
    ;;
esac
task_project_relative="${task_project_dir#"${task_repo_dir}/"}"
[[ -n "${task_project_relative}" &&
   "${task_project_relative}" != /* &&
   "${task_project_relative}" != *../* &&
   "${task_project_relative}" != ../* ]] || \
  fail "unsafe project-relative path: ${task_project_relative}"
[[ "$(git -C "${task_repo_dir}" rev-parse --is-shallow-repository)" == false ]] || \
  fail "a shallow repository cannot produce the required complete Git bundle"

task_release_dir=""
task_release_root=""
if [[ -n "${task_release_version}" ]]; then
  task_release_root="${task_project_dir}/artifacts/firmware"
  [[ -d "${task_release_root}" && ! -L "${task_release_root}" ]] || \
    fail "release root is missing or unsafe: ${task_release_root}"
  task_release_root="$(realpath -e -- "${task_release_root}")"
  [[ "${task_release_root}" == "${task_project_dir}/artifacts/firmware" ]] || \
    fail "release root escapes the ECU project artifacts directory"
  task_release_dir="${task_release_root}/${task_release_version}"
  [[ -d "${task_release_dir}" && ! -L "${task_release_dir}" ]] || \
    fail "release directory is missing or unsafe: ${task_release_dir}"
  task_release_dir="$(realpath -e -- "${task_release_dir}")"
  [[ "$(dirname -- "${task_release_dir}")" == "${task_release_root}" &&
     "$(basename -- "${task_release_dir}")" == "${task_release_version}" ]] || \
    fail "release directory is not the canonical project release path"
  validate_regular_tree "${task_release_root}" "Firmware release ledger"
fi

# Serialize publishers on the backup-root directory inode. No lock pathname
# exists that could be replaced with a symbolic link.
exec {task_lock_fd}<"${task_backup_root}"
flock -n "${task_lock_fd}" || \
  fail "another development backup transaction is active in ${task_backup_root}"

printf 'Verifying referenced physical-device recovery baseline...\n'
(
  cd -- "${task_baseline_dir}"
  sha256sum --strict --quiet -c MANIFEST.sha256
)
task_baseline_manifest_sha="$(sha256sum -- \
  "${task_baseline_dir}/MANIFEST.sha256" | cut -d ' ' -f 1)"
task_baseline_pki_dir="${task_baseline_dir}/pki/roller-ecu-pki"
task_baseline_assets_dir="${task_baseline_dir}/project/artifacts/security-provisioning"
[[ -d "${task_baseline_pki_dir}" && ! -L "${task_baseline_pki_dir}" ]] || \
  fail "verified baseline does not contain its canonical PKI directory"
[[ -d "${task_baseline_assets_dir}" && ! -L "${task_baseline_assets_dir}" ]] || \
  fail "verified baseline does not contain canonical security-provisioning assets"
validate_exact_pki "${task_baseline_pki_dir}"
validate_exact_security_assets "${task_baseline_assets_dir}"
validate_exact_pki "${task_pki_dir}"
validate_exact_security_assets "${task_security_assets_dir}"
[[ "$(tree_fingerprint "${task_pki_dir}" content)" == \
   "$(tree_fingerprint "${task_baseline_pki_dir}" content)" ]] || \
  fail "working PKI differs from the verified physical-device recovery baseline"
[[ "$(tree_fingerprint "${task_security_assets_dir}" content)" == \
   "$(tree_fingerprint "${task_baseline_assets_dir}" content)" ]] || \
  fail "security-provisioning data differs from the verified recovery baseline"
if ! task_da_validation="$(PYTHONDONTWRITEBYTECODE=1 \
    python3 "${task_script_dir}/ecu_debug_auth.py" verify-assets \
      --pki-dir "${task_pki_dir}" \
      --assets-dir "${task_security_assets_dir}" 2>&1)"; then
  printf '%s\n' "${task_da_validation}" >&2
  fail "local DA/security-provisioning verification failed"
fi
printf '%s\n' "${task_da_validation}"
if [[ -n "${task_release_dir}" ]]; then
  validate_release "${task_release_dir}" "${task_release_version}" \
    "${task_pki_dir}/ota-transport-public.pem"
fi

task_timestamp="$(date -u +'%Y%m%dT%H%M%SZ')"
task_created_utc="$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
task_output_name="roller-ecu-${task_label}-${task_timestamp}"
task_output_dir="${task_backup_root}/${task_output_name}"
[[ ! -e "${task_output_dir}" && ! -L "${task_output_dir}" ]] || \
  fail "backup output already exists: ${task_output_dir}"
task_staging_dir="$(mktemp -d -- \
  "${task_backup_root}/.roller-ecu-backup.tmp.XXXXXX")"
task_drill_dir="$(mktemp -d -- \
  "${task_backup_root}/.roller-ecu-restore.tmp.XXXXXX")"

task_source_fingerprint="$(tree_fingerprint "${task_project_dir}" source)"
task_git_state="$(capture_git_state)"
task_git_head="$(git -C "${task_repo_dir}" rev-parse HEAD)"
task_git_branch="$(git -C "${task_repo_dir}" branch --show-current)"
task_repo_refs_sha="$(git -C "${task_repo_dir}" show-ref --head | \
  LC_ALL=C sort | git_stream_sha256)"
task_git_status_sha="$(git -C "${task_repo_dir}" \
  status --porcelain=v1 -z -- "${task_project_relative}" | git_stream_sha256)"
task_pki_fingerprint="$(tree_fingerprint "${task_pki_dir}" content)"
task_assets_fingerprint="$(tree_fingerprint \
  "${task_security_assets_dir}" content)"
task_release_fingerprint=""
task_release_history_fingerprint=""
if [[ -n "${task_release_dir}" ]]; then
  task_release_fingerprint="$(tree_fingerprint "${task_release_dir}" content)"
  task_release_history_fingerprint="$(
    tree_fingerprint "${task_release_root}" content)"
fi

mkdir -m 700 -- "${task_staging_dir}/source" \
  "${task_staging_dir}/pki" "${task_staging_dir}/security-provisioning"
tar --create --gzip --file "${task_staging_dir}/source/ECU-source.tar.gz" \
  --directory "${task_project_dir}" \
  --exclude='./build' \
  --exclude='./Secure/build' \
  --exclude='./NonSecure/build' \
  --exclude='./Bootloader/OEMiROT/build' \
  --exclude='./artifacts' \
  --exclude='./mx.scratch' \
  --exclude='./__pycache__' \
  --exclude='*/__pycache__' \
  --exclude='*.pyc' \
  .
git -C "${task_repo_dir}" bundle create \
  "${task_staging_dir}/source/repository.bundle" --all
git -C "${task_repo_dir}" bundle verify \
  "${task_staging_dir}/source/repository.bundle" > \
  "${task_staging_dir}/source/bundle-verify.txt" 2>&1
task_bundle_refs_sha="$(git bundle list-heads \
  "${task_staging_dir}/source/repository.bundle" | \
  LC_ALL=C sort | git_stream_sha256)"
[[ "${task_bundle_refs_sha}" == "${task_repo_refs_sha}" ]] || \
  fail "Git bundle ref set differs from the captured repository"
git -C "${task_repo_dir}" status --short --branch -- \
  "${task_project_relative}" > "${task_staging_dir}/source/git-status.txt"
git -C "${task_repo_dir}" diff --binary --full-index --no-ext-diff HEAD -- \
  "${task_project_relative}" > \
  "${task_staging_dir}/source/tracked-working-tree.patch"
git -C "${task_repo_dir}" ls-files --others --exclude-standard -- \
  "${task_project_relative}" > \
  "${task_staging_dir}/source/untracked-files.txt"
printf '%s\n' "${task_git_head}" > "${task_staging_dir}/source/git-head.txt"
printf '%s\n' "${task_git_branch}" > "${task_staging_dir}/source/git-branch.txt"
printf '%s\n' "${task_git_state}" > "${task_staging_dir}/source/git-state.txt"
printf '%s  ECU-source.tar.gz restored-tree\n' \
  "${task_source_fingerprint}" > "${task_staging_dir}/source/source-tree.sha256"

cp -a -- "${task_pki_dir}/." "${task_staging_dir}/pki/"
cp -a -- "${task_security_assets_dir}/." \
  "${task_staging_dir}/security-provisioning/"
if [[ -n "${task_release_dir}" ]]; then
  mkdir -m 700 -- "${task_staging_dir}/release" \
    "${task_staging_dir}/release-history"
  cp -a -- "${task_release_dir}/." "${task_staging_dir}/release/"
  cp -a -- "${task_release_root}/." \
    "${task_staging_dir}/release-history/"
fi
find "${task_staging_dir}" -type d -exec chmod 700 -- {} +
find "${task_staging_dir}" -type f -exec chmod 600 -- {} +

validate_exact_pki "${task_staging_dir}/pki"
validate_exact_security_assets "${task_staging_dir}/security-provisioning"
if ! task_staged_da_validation="$(PYTHONDONTWRITEBYTECODE=1 \
    python3 "${task_script_dir}/ecu_debug_auth.py" verify-assets \
      --pki-dir "${task_staging_dir}/pki" \
      --assets-dir "${task_staging_dir}/security-provisioning" 2>&1)"; then
  printf '%s\n' "${task_staged_da_validation}" >&2
  fail "copied DA/security-provisioning verification failed"
fi
printf '%s\n%s\n' \
  "Exact encrypted PKI set and all seven P-256 key pairs verified" \
  "${task_staged_da_validation}" > "${task_staging_dir}/PKI_VALIDATION.txt"
[[ "$(tree_fingerprint "${task_staging_dir}/pki" content)" == \
   "${task_pki_fingerprint}" ]] || fail "copied PKI differs from its source"
[[ "$(tree_fingerprint "${task_staging_dir}/security-provisioning" content)" == \
   "${task_assets_fingerprint}" ]] || \
  fail "copied security-provisioning data differs from its source"
if [[ -n "${task_release_dir}" ]]; then
  validate_release "${task_staging_dir}/release" "${task_release_version}" \
    "${task_staging_dir}/pki/ota-transport-public.pem"
  [[ "$(tree_fingerprint "${task_staging_dir}/release" content)" == \
     "${task_release_fingerprint}" ]] || fail "copied release differs from its source"
  validate_regular_tree "${task_staging_dir}/release-history" \
    "Copied firmware release ledger"
  [[ "$(tree_fingerprint "${task_staging_dir}/release-history" content)" == \
     "${task_release_history_fingerprint}" ]] || \
    fail "copied firmware release ledger differs from its source"
fi

# Offline restore drill: the only inputs are the staged bundle and archive.
task_drill_repo="${task_drill_dir}/repository"
git clone --no-checkout --no-hardlinks \
  "${task_staging_dir}/source/repository.bundle" "${task_drill_repo}" \
  > "${task_staging_dir}/source/restore-clone.txt" 2>&1
git -C "${task_drill_repo}" cat-file -e "${task_git_head}^{commit}"
git -C "${task_drill_repo}" checkout --detach "${task_git_head}" \
  >> "${task_staging_dir}/source/restore-clone.txt" 2>&1
git -C "${task_drill_repo}" fsck --full --strict \
  > "${task_staging_dir}/source/restore-fsck.txt" 2>&1
task_drill_project="${task_drill_repo}/${task_project_relative}"
[[ -d "${task_drill_project}" && ! -L "${task_drill_project}" ]] || \
  fail "offline drill did not create the protected ECU project path"
case "$(realpath -e -- "${task_drill_project}")/" in
  "$(realpath -e -- "${task_drill_repo}")/"*) ;;
  *) fail "offline drill project escaped its temporary repository" ;;
esac
find "${task_drill_project}" -mindepth 1 -delete
tar --extract --gzip --file \
  "${task_staging_dir}/source/ECU-source.tar.gz" \
  --directory "${task_drill_project}" --no-same-owner --same-permissions
task_restored_fingerprint="$(tree_fingerprint "${task_drill_project}" source)"
if [[ "${task_restored_fingerprint}" != "${task_source_fingerprint}" ]]; then
  printf 'Captured source fingerprint: %s\n' \
    "${task_source_fingerprint}" >&2
  printf 'Restored source fingerprint: %s\n' \
    "${task_restored_fingerprint}" >&2
  fail "offline restored source tree differs from the captured source"
fi
task_restored_status_sha="$(git -C "${task_drill_repo}" \
  status --porcelain=v1 -z -- "${task_project_relative}" | git_stream_sha256)"
[[ "${task_restored_status_sha}" == "${task_git_status_sha}" ]] || \
  fail "offline restored Git worktree status differs from the captured state"
printf '%s\n' \
  'result=PASS' \
  "git_head=${task_git_head}" \
  "source_tree_sha256=${task_restored_fingerprint}" \
  "git_status_sha256=${task_restored_status_sha}" \
  'bundle_clone=PASS' \
  'git_fsck_full_strict=PASS' \
  'source_overlay=PASS' > \
  "${task_staging_dir}/source/OFFLINE_RESTORE_DRILL.txt"
cleanup_private_dir "${task_drill_dir}"
task_drill_dir=""

cat > "${task_staging_dir}/VERIFY_BACKUP.py" <<'PY'
#!/usr/bin/env python3
"""Standalone strict verifier for one Roller ECU development backup."""

from __future__ import annotations

import hashlib
import json
import os
import re
import stat
import sys
from pathlib import Path


SAFE_COMPONENT = re.compile(r"[A-Za-z0-9._-]+")
HASH_LINE = re.compile(r"([0-9a-f]{64})  [.]\/(.+)")
SCRIPT = Path(__file__).absolute()
if SCRIPT.is_symlink():
    raise SystemExit("VERIFY_BACKUP.py must not be a symbolic link")
ROOT = SCRIPT.parent
if not ROOT.is_dir() or ROOT.is_symlink():
    raise SystemExit("backup root must be a real directory")


def safe_path(relative: str) -> bool:
    if relative == ".":
        return True
    return (relative != "" and not relative.startswith("/")
            and all(part not in {"", ".", ".."}
                    and SAFE_COMPONENT.fullmatch(part) is not None
                    for part in relative.split("/")))


def scan_tree() -> dict[str, tuple[str, str]]:
    result: dict[str, tuple[str, str]] = {}

    def visit(path: Path, relative: str) -> None:
        if not safe_path(relative):
            raise SystemExit(f"unsafe backup member path: {relative!r}")
        info = path.stat(follow_symlinks=False)
        if stat.S_ISDIR(info.st_mode):
            kind = "directory"
        elif stat.S_ISREG(info.st_mode):
            kind = "file"
        else:
            raise SystemExit(f"unsupported backup member type: {relative}")
        mode = f"{stat.S_IMODE(info.st_mode):04o}"
        required_mode = "0700" if kind == "directory" else "0600"
        if mode != required_mode:
            raise SystemExit(
                f"unsafe mode for {relative}: {mode}, expected {required_mode}")
        if relative in result:
            raise SystemExit(f"duplicate backup member: {relative}")
        result[relative] = (kind, mode)
        if kind == "directory":
            for entry in sorted(
                    os.scandir(path), key=lambda item: os.fsencode(item.name)):
                child = (entry.name if relative == "."
                         else relative + "/" + entry.name)
                visit(Path(entry.path), child)

    visit(ROOT, ".")
    return result


def load_modes() -> dict[str, tuple[str, str]]:
    result: dict[str, tuple[str, str]] = {}
    try:
        lines = (ROOT / "FILE_MODES.jsonl").read_text(
            encoding="ascii").splitlines()
    except (OSError, UnicodeError) as exc:
        raise SystemExit(f"cannot read FILE_MODES.jsonl: {exc}") from exc
    if not lines:
        raise SystemExit("FILE_MODES.jsonl is empty")
    for line in lines:
        try:
            item = json.loads(line)
        except json.JSONDecodeError as exc:
            raise SystemExit(f"invalid FILE_MODES.jsonl: {exc}") from exc
        if (type(item) is not dict
                or set(item) != {"path", "type", "mode"}
                or type(item["path"]) is not str
                or item["type"] not in {"directory", "file"}
                or item["mode"] not in {"0700", "0600"}
                or not safe_path(item["path"])
                or item["path"] in result):
            raise SystemExit("invalid or duplicate FILE_MODES.jsonl record")
        required_mode = "0700" if item["type"] == "directory" else "0600"
        if item["mode"] != required_mode:
            raise SystemExit(f"non-private recorded mode: {item['path']}")
        result[item["path"]] = (item["type"], item["mode"])
    return result


def load_hashes() -> dict[str, str]:
    result: dict[str, str] = {}
    try:
        lines = (ROOT / "MANIFEST.sha256").read_text(
            encoding="ascii").splitlines()
    except (OSError, UnicodeError) as exc:
        raise SystemExit(f"cannot read MANIFEST.sha256: {exc}") from exc
    if not lines:
        raise SystemExit("MANIFEST.sha256 is empty")
    for line in lines:
        match = HASH_LINE.fullmatch(line)
        if match is None or not safe_path(match.group(2)):
            raise SystemExit("MANIFEST.sha256 is not canonical")
        digest, relative = match.groups()
        if relative in result:
            raise SystemExit(f"duplicate hash manifest member: {relative}")
        result[relative] = digest
    if list(result) != sorted(result, key=os.fsencode):
        raise SystemExit("MANIFEST.sha256 member order is not canonical")
    return result


actual = scan_tree()
recorded = load_modes()
if actual != recorded:
    raise SystemExit("FILE_MODES.jsonl does not exactly match the backup tree")
hashes = load_hashes()
regular_files = {
    relative for relative, (kind, _mode) in actual.items() if kind == "file"
}
if set(hashes) != regular_files - {"MANIFEST.sha256"}:
    raise SystemExit("MANIFEST.sha256 does not cover the exact regular-file set")
for relative, expected in hashes.items():
    path = ROOT / relative
    before = path.stat(follow_symlinks=False)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    after = path.stat(follow_symlinks=False)
    left = (before.st_dev, before.st_ino, before.st_mode, before.st_size,
            before.st_mtime_ns, before.st_ctime_ns)
    right = (after.st_dev, after.st_ino, after.st_mode, after.st_size,
             after.st_mtime_ns, after.st_ctime_ns)
    if left != right:
        raise SystemExit(f"backup member changed while hashing: {relative}")
    if digest.hexdigest() != expected:
        raise SystemExit(f"SHA-256 mismatch: {relative}")
print(
    f"Roller ECU backup verified: {len(actual)} objects, "
    f"{len(hashes)} authenticated payload files")
PY

{
  printf 'schema=roller-ecu-development-backup-v2\n'
  printf 'created_utc=%s\n' "${task_created_utc}"
  printf 'project_path=%s\n' "${task_project_dir}"
  printf 'repository_path=%s\n' "${task_repo_dir}"
  printf 'project_relative_path=%s\n' "${task_project_relative}"
  printf 'git_head=%s\n' "${task_git_head}"
  printf 'git_branch=%s\n' "${task_git_branch}"
  printf 'source_tree_sha256=%s\n' "${task_source_fingerprint}"
  printf 'pki_tree_sha256=%s\n' "${task_pki_fingerprint}"
  printf 'security_assets_tree_sha256=%s\n' "${task_assets_fingerprint}"
  printf 'release_version=%s\n' "${task_release_version:-none}"
  printf 'release_tree_sha256=%s\n' "${task_release_fingerprint:-none}"
  printf 'release_history_tree_sha256=%s\n' \
    "${task_release_history_fingerprint:-none}"
  printf 'baseline_recovery_path=%s\n' "${task_baseline_dir}"
  printf 'baseline_recovery_name=%s\n' "$(basename -- "${task_baseline_dir}")"
  printf 'baseline_manifest_sha256=%s\n' "${task_baseline_manifest_sha}"
  printf 'offline_restore_drill=PASS\n'
} > "${task_staging_dir}/BACKUP_METADATA.txt"

cat > "${task_staging_dir}/RESTORE.md" <<EOF
# Roller ECU development backup

This non-overwriting snapshot contains the exact dirty/untracked ECU source
overlay, a complete Git bundle, an independent binary patch, the encrypted
working PKI, matching security-provisioning assets, and an optional signed
release. Release snapshots also carry the complete immutable firmware release
ledger needed to preserve consumed identities, revoked evidence, and previous
swap references. The automatic offline restore drill passed before publication.

Verify hashes and private modes first:

    python3 VERIFY_BACKUP.py

The standalone verifier requires only Python's standard library. For an
independent hash-tool cross-check, also run:

    sha256sum --strict -c MANIFEST.sha256

Clone without network access, check out the captured commit, and overlay the
exact source state:

    mkdir -m 700 offline-restore
    git clone --no-checkout source/repository.bundle offline-restore/repository
    git -C offline-restore/repository checkout --detach ${task_git_head}
    restored_project=offline-restore/repository/${task_project_relative}
    find "\${restored_project}" -mindepth 1 -delete
    tar -xzf source/ECU-source.tar.gz -C "\${restored_project}" \\
      --no-same-owner --same-permissions
    git -C offline-restore/repository status --short -- ${task_project_relative}

The source archive is authoritative. \`source/tracked-working-tree.patch\` and
\`source/untracked-files.txt\` are provenance aids. Review
\`source/OFFLINE_RESTORE_DRILL.txt\` for bundle/fsck/overlay evidence.

Validate the copied CLOSED/DA authority locally (this does not contact ECU):

    python3 "\${restored_project}/tools/ecu_debug_auth.py" verify-assets \\
      --pki-dir "\$PWD/pki" \\
      --assets-dir "\$PWD/security-provisioning"

The physical-device baseline referenced in BACKUP_METADATA.txt is still needed
for full CLOSED-target disaster recovery; verify its manifest separately. This
snapshot stores key-passphrase.txt with encrypted keys, so possession grants
signing and DA authority. Keep at least two encrypted offline/off-site copies
and protect the media password separately.
EOF

touch "${task_staging_dir}/MANIFEST.sha256" \
  "${task_staging_dir}/FILE_MODES.jsonl"
find "${task_staging_dir}" -type d -exec chmod 700 -- {} +
find "${task_staging_dir}" -type f -exec chmod 600 -- {} +
write_file_modes "${task_staging_dir}" > \
  "${task_staging_dir}/FILE_MODES.jsonl"
(
  cd -- "${task_staging_dir}"
  find . -type f ! -name MANIFEST.sha256 -print0 |
    LC_ALL=C sort -z |
    xargs -0 sha256sum -- > MANIFEST.sha256
  sha256sum --strict -c MANIFEST.sha256
)
verify_file_modes "${task_staging_dir}"
python3 "${task_staging_dir}/VERIFY_BACKUP.py"

# Recheck every mutable input immediately before atomic publication.
(
  cd -- "${task_baseline_dir}"
  sha256sum --strict --quiet -c MANIFEST.sha256
)
[[ "$(sha256sum -- "${task_baseline_dir}/MANIFEST.sha256" | cut -d ' ' -f 1)" == \
   "${task_baseline_manifest_sha}" ]] || fail "baseline manifest changed during backup"
[[ "$(tree_fingerprint "${task_project_dir}" source)" == \
   "${task_source_fingerprint}" ]] || fail "ECU source changed during backup"
[[ "$(capture_git_state)" == "${task_git_state}" ]] || \
  fail "Git HEAD, refs, index, or ECU status changed during backup"
[[ "$(tree_fingerprint "${task_pki_dir}" content)" == \
   "${task_pki_fingerprint}" ]] || fail "working PKI changed during backup"
[[ "$(tree_fingerprint "${task_security_assets_dir}" content)" == \
   "${task_assets_fingerprint}" ]] || \
  fail "security-provisioning assets changed during backup"
if [[ -n "${task_release_dir}" ]]; then
  [[ "$(tree_fingerprint "${task_release_dir}" content)" == \
     "${task_release_fingerprint}" ]] || fail "release changed during backup"
  [[ "$(tree_fingerprint "${task_release_root}" content)" == \
     "${task_release_history_fingerprint}" ]] || \
    fail "firmware release ledger changed during backup"
fi

[[ ! -e "${task_output_dir}" && ! -L "${task_output_dir}" ]] || \
  fail "backup output appeared during transaction: ${task_output_dir}"
task_staging_identity="$(stat -c '%d:%i' -- "${task_staging_dir}")"
mv -T -n -- "${task_staging_dir}" "${task_output_dir}"
[[ ! -e "${task_staging_dir}" && ! -L "${task_staging_dir}" &&
   -d "${task_output_dir}" && ! -L "${task_output_dir}" &&
   "$(stat -c '%d:%i' -- "${task_output_dir}")" == \
     "${task_staging_identity}" ]] || \
  fail "atomic non-overwriting backup publication did not consume staging"
task_staging_dir=""
sync -f "${task_output_dir}"
sync -f "${task_backup_root}"

printf 'Development backup created: %s\n' "${task_output_dir}"
printf 'Verify with: (cd %q && python3 VERIFY_BACKUP.py)\n' \
  "${task_output_dir}"
