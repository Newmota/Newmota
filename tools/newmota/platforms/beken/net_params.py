"""Factory-only NET reset while retaining bounded native vendor records."""

from __future__ import annotations

import struct

NET_SECTOR_SIZE = 4096
NET_RESET_POLICY = "reset-newmota-preserve-native-v1"
TABLE_TYPE = 0x00564C54
NEWMOTA_RECORD_TYPE = 0x4E4D4352
# Match platform::kCriticalSettingsCapacity, plus the native TLV header.
CRITICAL_RECORD_SPACE = 8 + 512
HEADER = struct.Struct("<II")


def factory_net_sector(source: bytes) -> bytes:
    """Remove Newmota credentials, retain native data, and require writable space.

    A foreign format is replaced during explicit factory installation. A damaged
    native table is rejected because its vendor records cannot be bounded safely.
    """
    if len(source) != NET_SECTOR_SIZE:
        raise ValueError("Beken NET initialization requires one complete 4096-byte sector")
    table_type, length = HEADER.unpack_from(source)
    if table_type != TABLE_TYPE:
        return HEADER.pack(TABLE_TYPE, 0) + b"\xff" * (NET_SECTOR_SIZE - HEADER.size)
    end = HEADER.size + length
    if end > len(source):
        raise ValueError("Beken NET table length exceeds its sector")
    position = HEADER.size
    retained = bytearray()
    while position < end:
        if end - position < HEADER.size:
            raise ValueError("Beken NET table has a truncated record header")
        record_type, size = HEADER.unpack_from(source, position)
        record_end = position + HEADER.size + size
        if record_end > end:
            raise ValueError("Beken NET record exceeds its table")
        if record_type != NEWMOTA_RECORD_TYPE:
            retained.extend(source[position:record_end])
        position = record_end
    result = (
        HEADER.pack(TABLE_TYPE, len(retained))
        + retained
        + b"\xff" * (length - len(retained))
        + source[end:]
    )
    free_start = HEADER.size + len(retained)
    free_end = free_start + CRITICAL_RECORD_SPACE
    if free_end > len(result) or result[free_start:free_end] != b"\xff" * CRITICAL_RECORD_SPACE:
        raise ValueError("Beken NET has no erased room for a new critical credential record")
    return bytes(result)
