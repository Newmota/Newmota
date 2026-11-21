"""Deterministic gzip streams for firmware assets and update images."""

from __future__ import annotations


def zopfli_gzip(data: bytes) -> bytes:
    """Return Zopfli's gzip encoding of @p data, identical on every host.

    The stream is ordinary deflate with a 32 KiB window, so any gzip decoder reads it.
    """

    from zopfli.gzip import compress

    # Pin the encoder settings and normalize gzip's OS byte, which Zopfli takes from the host.
    compressed = compress(data, numiterations=15)
    return compressed[:9] + b"\xff" + compressed[10:]
