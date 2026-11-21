"""Cross-platform UART uploader for Realtek AmebaD.

The packet protocol is adapted from the community PlatformIO uploader at
https://github.com/tmmsunny012/ameba-arduino-d/tree/feature-platformio-support.
It deliberately exposes a small library API instead of the upstream CLI so a
validated Newmota package remains the single source of image paths and hashes.
"""

from __future__ import annotations

import struct
import sys
import time
from collections.abc import Sequence
from pathlib import Path
from typing import Any

try:
    import serial
except ImportError:  # pragma: no cover - exercised only in an incomplete host setup
    serial = None  # type: ignore[assignment]

from newmota.platforms.amebad.images import (
    BOOT_BAUD,
    DEFAULT_UPLOAD_BAUD,
    FLASH_BASE,
    FLASHLOADER_ADDRESS,
)
from newmota.progress import WAIT_SECONDS, Progress, notice

SYNC = 0x15
ACK = 0x06
FLASH_SECTOR_SIZE = 0x1000
WRITE_BLOCK_SIZE = 1024
# Each sync listen lasts this long before the download-mode instruction repeats.
SYNC_SLICE_SECONDS = 3.0
DOWNLOAD_MODE_HINT = "hold UART DOWNLOAD, press and release RESET (or power-cycle), then release"

# The sub-command bytes are part of the Realtek UART download protocol.
SPEED_COMMANDS = {
    1_500_000: 0x18,
    1_444_400: 0x17,
    1_382_400: 0x16,
    1_000_000: 0x15,
    921_600: 0x14,
    500_000: 0x13,
    460_800: 0x12,
    380_400: 0x11,
    230_400: 0x10,
    153_600: 0x0F,
    128_000: 0x0D,
    BOOT_BAUD: 0x0C,
}


class AmebadUploadError(Exception):
    """Raised when the AmebaD UART protocol cannot complete safely."""


def byte_checksum(data: bytes) -> int:
    """Return the protocol's one-byte packet checksum."""

    return (0xFF + sum(data)) & 0xFF


def word_checksum(data: bytes) -> int:
    """Return the protocol's little-endian 32-bit word-sum checksum."""

    result = 0
    for offset in range(0, len(data), 4):
        result = (result + int.from_bytes(data[offset : offset + 4].ljust(4, b"\0"), "little")) & (
            0xFFFFFFFF
        )
    return result


def write_packet(address: int, data: bytes, sequence: int) -> bytes:
    """Build one 1 KiB flashloader write packet."""

    if len(data) != WRITE_BLOCK_SIZE:
        raise ValueError("AmebaD write packets must contain exactly 1024 data bytes")
    packet = bytearray(WRITE_BLOCK_SIZE + 8)
    packet[0] = 0x02
    packet[1] = sequence & 0xFF
    packet[2] = (~sequence) & 0xFF
    struct.pack_into("<I", packet, 3, address)
    packet[7 : 7 + WRITE_BLOCK_SIZE] = data
    packet[-1] = byte_checksum(packet[:-1])
    return bytes(packet)


def _read_byte(port: Any, deadline: float) -> int | None:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        return None
    previous_timeout = getattr(port, "timeout", None)
    try:
        port.timeout = min(0.2, remaining)
        value = port.read(1)
    finally:
        if previous_timeout is not None:
            port.timeout = previous_timeout
    return value[0] if value else None


def _wait_sync(port: Any, count: int, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    matched = 0
    while time.monotonic() < deadline:
        value = _read_byte(port, deadline)
        if value is None:
            continue
        if value == SYNC:
            matched += 1
            if matched == count:
                return
        else:
            matched = 0
    raise AmebadUploadError(f"timed out waiting for {count} AmebaD sync byte(s)")


def _wait_ack(port: Any, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = _read_byte(port, deadline)
        if value is None or value == SYNC:
            continue
        if value == ACK:
            return
        raise AmebadUploadError(f"AmebaD flashloader returned unexpected byte 0x{value:02x}")
    raise AmebadUploadError("timed out waiting for AmebaD flashloader acknowledgement")


def _send_command(port: Any, command: bytes) -> None:
    if not command:
        raise ValueError("AmebaD command cannot be empty")
    if command[0] != 0x07:
        _wait_sync(port, 1)
    port.write(command)
    _wait_ack(port)


def _send_verify_command(port: Any, command: bytes) -> int:
    _wait_sync(port, 1)
    port.write(command)
    deadline = time.monotonic() + 8.0
    response = bytearray()
    while len(response) < 5 and time.monotonic() < deadline:
        value = _read_byte(port, deadline)
        if value is None:
            continue
        if not response and value == SYNC:
            continue
        response.append(value)
    if len(response) != 5 or response[0] != 0x27:
        raise AmebadUploadError("AmebaD flashloader returned an invalid verify response")
    return struct.unpack_from("<I", response, 1)[0]


def _set_speed(port: Any, speed: int) -> None:
    try:
        sub_command = SPEED_COMMANDS[speed]
    except KeyError as error:
        supported = ", ".join(str(value) for value in sorted(SPEED_COMMANDS, reverse=True))
        raise AmebadUploadError(
            f"unsupported AmebaD upload baud {speed}; use one of {supported}"
        ) from error

    if speed != BOOT_BAUD:
        _wait_sync(port, 1)
        _send_command(port, bytes((0x05, sub_command)))
        port.baudrate = speed
    _send_command(port, bytes((0x07,)))
    _wait_sync(port, 1)


def erase_packet(address: int, sectors: int) -> bytes:
    """Build the six-byte AmebaD SPI-flash erase command."""

    if sectors <= 0 or sectors > 0xFFFF:
        raise ValueError(f"invalid AmebaD erase sector count: {sectors}")
    # The vendor command is six bytes: opcode, three address bytes, and a
    # two-byte sector count. The community C++ implementation builds the
    # address and sector count as overlapping 32-bit fields and sends only
    # those six bytes; reproduce the resulting wire format exactly.
    command = bytearray(6)
    command[0] = 0x17
    struct.pack_into("<I", command, 1, address)
    command[4] = sectors & 0xFF
    command[5] = (sectors >> 8) & 0xFF
    return bytes(command)


def _erase(port: Any, address: int, size: int) -> None:
    sectors = (size + FLASH_SECTOR_SIZE - 1) // FLASH_SECTOR_SIZE
    try:
        command = erase_packet(address, sectors)
    except ValueError as error:
        raise AmebadUploadError(f"invalid AmebaD erase size: {size} bytes") from error
    _send_command(port, command)


def _write_image(
    port: Any,
    name: str,
    address: int,
    data: bytes,
    sequence: int,
    progress: Progress,
    before: int,
) -> int:
    if not data:
        raise AmebadUploadError(f"AmebaD image is empty: {name}")
    blocks = (len(data) + WRITE_BLOCK_SIZE - 1) // WRITE_BLOCK_SIZE
    padded = data.ljust(blocks * WRITE_BLOCK_SIZE, b"\xff")
    progress.update(before, f"Writing {name}")
    _wait_sync(port, 1)
    for index in range(blocks):
        block = padded[index * WRITE_BLOCK_SIZE : (index + 1) * WRITE_BLOCK_SIZE]
        port.write(write_packet(address + index * WRITE_BLOCK_SIZE, block, sequence))
        _wait_ack(port)
        sequence = (sequence + 1) & 0xFF
        progress.update(before + min((index + 1) * WRITE_BLOCK_SIZE, len(data)))
    return sequence


def _set_dtr_rts(port: Any, level: int) -> None:
    # Match the convention used by the vendor/community AmebaD uploader.
    port.rts = not bool(level & 0x01)
    port.dtr = not bool(level & 0x02)


def _enter_download_mode(port: Any) -> None:
    _set_dtr_rts(port, 0x02)
    time.sleep(0.5)
    _set_dtr_rts(port, 0x01)
    time.sleep(0.2)
    _set_dtr_rts(port, 0x03)
    time.sleep(0.5)


def _reset_to_application(port: Any) -> None:
    _set_dtr_rts(port, 0x02)
    time.sleep(0.5)
    _set_dtr_rts(port, 0x03)


def _wait_for_download_mode(port: Any, manual: bool) -> None:
    """Listen for the ROM's download sync, repeating the instruction until WAIT_SECONDS."""

    if manual:
        notice(f"AmebaD: put the board into UART download mode now: {DOWNLOAD_MODE_HINT}")
    else:
        notice("AmebaD: connecting to the download ROM ...")
    deadline = time.monotonic() + WAIT_SECONDS
    while True:
        try:
            _wait_sync(port, 2, timeout=SYNC_SLICE_SECONDS)
        except AmebadUploadError:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AmebadUploadError(
                    f"AmebaD download ROM did not answer within {WAIT_SECONDS} s. "
                    f"Enter UART download mode ({DOWNLOAD_MODE_HINT}) and retry"
                ) from None
            notice(
                "AmebaD: no answer. Enter UART download mode now: "
                f"{DOWNLOAD_MODE_HINT} ({round(remaining)} s left)"
            )
        else:
            notice("AmebaD: download ROM connected")
            return


def _flush(port: Any) -> None:
    reset_input = getattr(port, "reset_input_buffer", None)
    if reset_input is not None:
        reset_input()
    reset_output = getattr(port, "reset_output_buffer", None)
    if reset_output is not None:
        reset_output()


def _program(
    port: Any,
    speed: int,
    images: Sequence[tuple[str, int, bytes]],
    flashloader: bytes,
    manual: bool,
) -> None:
    _flush(port)
    _wait_for_download_mode(port, manual)
    _set_speed(port, speed)

    loader_progress = Progress("Writing flashloader", len(flashloader))
    _write_image(
        port,
        "flashloader",
        FLASHLOADER_ADDRESS,
        flashloader,
        1,
        loader_progress,
        0,
    )
    loader_progress.end_line()
    _send_command(port, bytes((0x04,)))

    port.baudrate = BOOT_BAUD
    _wait_sync(port, 2)
    _send_command(port, bytes((0x26, 0x01, 0x01, 0x00)))

    for name, address, data in images:
        notice(f"AmebaD: erasing {name}")
        _erase(port, address, len(data))

    _wait_sync(port, 1)
    _set_speed(port, speed)
    sequence = 1
    progress = Progress("Writing images", sum(len(data) for _, _, data in images))
    for name, address, data in images:
        sequence = _write_image(port, name, address, data, sequence, progress, progress.done)
    progress.finish()

    _send_command(port, bytes((0x04,)))
    port.baudrate = BOOT_BAUD
    _flush(port)
    _wait_sync(port, 1)

    print("Verifying AmebaD image checksums", flush=True)
    for name, address, data in images:
        command = bytearray(8)
        command[0] = 0x27
        struct.pack_into("<I", command, 1, address & ~FLASH_BASE)
        struct.pack_into("<I", command, 4, len(data))
        actual = _send_verify_command(port, bytes(command[:7]))
        expected = word_checksum(data)
        if actual != expected:
            raise AmebadUploadError(
                f"checksum mismatch for {name}: got 0x{actual:08x}, expected 0x{expected:08x}"
            )
    print("AmebaD image verification passed", flush=True)


def upload(
    port_name: str,
    flashloader_path: Path,
    images: Sequence[tuple[str, int, Path]],
    baud: int | None = None,
    *,
    auto_reset: bool = False,
) -> None:
    """Erase, write, and verify one AmebaD factory image set.

    ``images`` contains physical flash offsets. The mapped addresses required
    by the Realtek loader are derived here, so package metadata remains
    hardware-oriented and easy to inspect.
    """

    if serial is None:
        raise AmebadUploadError("AmebaD uploader requires the project pyserial dependency")
    speed = baud or DEFAULT_UPLOAD_BAUD
    if speed not in SPEED_COMMANDS:
        raise AmebadUploadError(f"unsupported AmebaD upload baud: {speed}")
    if not flashloader_path.is_file():
        raise AmebadUploadError(f"AmebaD flashloader is missing: {flashloader_path}")

    loaded_images: list[tuple[str, int, bytes]] = []
    offsets: set[int] = set()
    for name, offset, path in images:
        if offset < 0 or offset in offsets:
            raise AmebadUploadError(f"invalid or duplicate AmebaD image offset: {offset:#x}")
        if not path.is_file():
            raise AmebadUploadError(f"AmebaD image is missing: {path}")
        data = path.read_bytes()
        if not data:
            raise AmebadUploadError(f"AmebaD image is empty: {name}")
        offsets.add(offset)
        loaded_images.append((name, FLASH_BASE + offset, data))
    if not loaded_images:
        raise AmebadUploadError("AmebaD package contains no images")

    flashloader = flashloader_path.read_bytes()
    try:
        connection = serial.Serial(
            port=port_name,
            baudrate=BOOT_BAUD,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.2,
            write_timeout=5.0,
        )
    except (OSError, serial.SerialException) as error:
        raise AmebadUploadError(f"cannot open serial port {port_name}: {error}") from error

    try:
        if auto_reset:
            print("Entering AmebaD UART download mode via DTR/RTS", flush=True)
            _enter_download_mode(connection)
        _program(connection, speed, loaded_images, flashloader, not auto_reset)
        if auto_reset:
            print("Resetting AmebaD into application mode", flush=True)
            _reset_to_application(connection)
        else:
            print("Flash complete; reset the AmebaD board manually", flush=True)
    except AmebadUploadError:
        raise
    except (OSError, ValueError) as error:
        raise AmebadUploadError(f"AmebaD UART upload failed: {error}") from error
    finally:
        try:
            connection.close()
        except OSError as error:
            print(f"warning: could not close AmebaD serial port: {error}", file=sys.stderr)
