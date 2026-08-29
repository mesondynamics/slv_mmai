"""Fail-closed shim for unused MCUboot CBOR boot-record support."""


def dumps(*args, **kwargs):  # pragma: no cover - boot records are not enabled
    del args, kwargs
    raise RuntimeError("ECU packaging does not enable CBOR boot records")
