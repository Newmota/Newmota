"""Unsigned, layout-bound OTA artifacts for Newmota platform writers."""

from __future__ import annotations

import struct
import zlib
from pathlib import Path
from typing import Any

OTA_ENVELOPE_FORMAT = "newmota-ota-envelope-v1"
OTA_ENVELOPE_MAGIC = b"NWMOTA1\0"
OTA_ENVELOPE_SCHEMA = 1
OTA_ENVELOPE_HEADER = struct.Struct("<8sHHIIII16sIIIIIII")
OTA_ENVELOPE_SIZE = 72

if OTA_ENVELOPE_HEADER.size != OTA_ENVELOPE_SIZE:
    raise RuntimeError("Newmota OTA envelope layout is inconsistent")


def chip_id(chip: str) -> int:
    """Return the stable 32-bit identifier shared by the host and writer."""

    return zlib.crc32(chip.encode("utf-8")) & 0xFFFFFFFF


def envelope_header(
    *,
    payload_size: int,
    payload_crc32: int,
    chip: str,
    flash_bytes: int,
    layout_sha256: str,
    application: tuple[int, int],
    safeboot: tuple[int, int],
    filesystem: tuple[int, int],
) -> bytes:
    """Encode the 72-byte little-endian transport and compatibility header."""

    if len(layout_sha256) != 64:
        raise ValueError("OTA layout identity must be a SHA-256 hex digest")
    layout_digest = bytes.fromhex(layout_sha256)[:16]
    values: tuple[Any, ...] = (
        OTA_ENVELOPE_MAGIC,
        OTA_ENVELOPE_SCHEMA,
        OTA_ENVELOPE_SIZE,
        payload_size,
        payload_crc32,
        chip_id(chip),
        flash_bytes,
        layout_digest,
        *application,
        *safeboot,
        *filesystem,
        0,
    )
    initial = OTA_ENVELOPE_HEADER.pack(*values)
    checksum = zlib.crc32(initial[:-4]) & 0xFFFFFFFF
    return OTA_ENVELOPE_HEADER.pack(*values[:-1], checksum)


def create_ota_envelope(
    source: Path,
    destination: Path,
    *,
    chip: str,
    flash_bytes: int,
    layout_sha256: str,
    application: tuple[int, int],
    safeboot: tuple[int, int],
    filesystem: tuple[int, int],
) -> dict[str, object]:
    """Wrap one native update payload and return its public manifest metadata.

    The header is deliberately unsigned. Its CRC and target fields reject
    accidental corruption and incompatible layouts; they do not authenticate
    a publisher.
    """

    size = source.stat().st_size
    crc = 0
    with source.open("rb") as image:
        while block := image.read(64 * 1024):
            crc = zlib.crc32(block, crc)
    crc &= 0xFFFFFFFF
    header = envelope_header(
        payload_size=size,
        payload_crc32=crc,
        chip=chip,
        flash_bytes=flash_bytes,
        layout_sha256=layout_sha256,
        application=application,
        safeboot=safeboot,
        filesystem=filesystem,
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as image, destination.open("wb") as output:
        output.write(header)
        while block := image.read(64 * 1024):
            output.write(block)
    return {
        "format": OTA_ENVELOPE_FORMAT,
        "header_size": OTA_ENVELOPE_SIZE,
        "payload_size": size,
        "payload_crc32": crc,
        "chip_id": chip_id(chip),
        "flash_bytes": flash_bytes,
        "layout_id": layout_sha256[:32],
        "application": {"offset": application[0], "size": application[1]},
        "safeboot": {"offset": safeboot[0], "size": safeboot[1]},
        "filesystem": {"offset": filesystem[0], "size": filesystem[1]},
        "unsigned": True,
    }


def validate_ota_envelope(
    path: Path,
    *,
    chip: str,
    flash_bytes: int,
    layout_sha256: str,
    application: tuple[int, int],
    safeboot: tuple[int, int],
    filesystem: tuple[int, int],
) -> dict[str, object]:
    """Read an artifact and verify all envelope fields and its payload CRC."""

    if path.stat().st_size < OTA_ENVELOPE_SIZE:
        raise ValueError("OTA envelope is shorter than its header")
    with path.open("rb") as image:
        header = image.read(OTA_ENVELOPE_SIZE)
        unpacked = OTA_ENVELOPE_HEADER.unpack(header)
        (
            magic,
            schema,
            header_size,
            payload_size,
            payload_crc,
            declared_chip_id,
            declared_flash,
            declared_layout,
            app_offset,
            app_size,
            safeboot_offset,
            safeboot_size,
            fs_offset,
            fs_size,
            header_crc,
        ) = unpacked
        if magic != OTA_ENVELOPE_MAGIC or schema != OTA_ENVELOPE_SCHEMA:
            raise ValueError("OTA envelope magic or schema is unsupported")
        if header_size != OTA_ENVELOPE_SIZE or zlib.crc32(header[:-4]) & 0xFFFFFFFF != header_crc:
            raise ValueError("OTA envelope header checksum is invalid")
        if path.stat().st_size != OTA_ENVELOPE_SIZE + payload_size:
            raise ValueError("OTA envelope payload length does not match the artifact")
        expected = (
            chip_id(chip),
            flash_bytes,
            bytes.fromhex(layout_sha256)[:16],
            *application,
            *safeboot,
            *filesystem,
        )
        actual = (
            declared_chip_id,
            declared_flash,
            declared_layout,
            app_offset,
            app_size,
            safeboot_offset,
            safeboot_size,
            fs_offset,
            fs_size,
        )
        if actual != expected:
            raise ValueError("OTA envelope target or flash layout does not match the package")
        crc = 0
        while block := image.read(64 * 1024):
            crc = zlib.crc32(block, crc)
    if crc & 0xFFFFFFFF != payload_crc:
        raise ValueError("OTA envelope payload checksum is invalid")
    return {
        "format": OTA_ENVELOPE_FORMAT,
        "header_size": header_size,
        "payload_size": payload_size,
        "payload_crc32": payload_crc,
        "chip_id": declared_chip_id,
        "flash_bytes": declared_flash,
        "layout_id": declared_layout.hex(),
    }


def geometry_from_package(
    data: dict[str, Any],
) -> tuple[tuple[int, int], tuple[int, int], tuple[int, int]]:
    """Resolve application, recovery, and filesystem geometry from a manifest."""

    layout = data.get("layout")
    if not isinstance(layout, dict):
        raise TypeError("OTA package has no resolved layout")
    if "ota2" in layout:
        application = layout["ota2"]
        safeboot = layout["ota1"]
    else:
        application = layout["application"]
        safeboot = layout["safeboot"]
    filesystem = layout.get("filesystem", layout.get("littlefs"))
    if not all(isinstance(region, dict) for region in (application, safeboot, filesystem)):
        raise ValueError("OTA package has incomplete update geometry")
    return (
        (int(application["offset"]), int(application["size"])),
        (int(safeboot["offset"]), int(safeboot["size"])),
        (int(filesystem["offset"]), int(filesystem["size"])),
    )
