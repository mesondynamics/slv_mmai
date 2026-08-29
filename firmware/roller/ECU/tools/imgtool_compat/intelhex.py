"""Fail-closed compatibility shim for the project's BIN-only imgtool use.

The CubeH5 imgtool imports IntelHex unconditionally even when all inputs and
outputs are binary.  Production packaging deliberately rejects HEX paths, so
the optional third-party IntelHex package is not needed on the build host.
"""


class IntelHex:  # pragma: no cover - any use is a packaging configuration bug
    def __init__(self, *args, **kwargs):
        del args, kwargs
        raise RuntimeError("ECU packaging supports BIN input/output only")


class AddressOverlapError(RuntimeError):
    pass
