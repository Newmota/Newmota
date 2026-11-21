"""Beken application and OTA image formats.

The Beken SDK emits a raw linker binary.  Beken boot ROM/bootloader images use
two different containers around that binary:

* the factory application image stores CRC16 bytes after every 32-byte block;
* the normal single-image OTA file is an RBL header followed by a compressed
  and optionally encrypted payload.

These formats are deliberately configured by the selected partition
descriptor.  There is no universal Beken key, container size, or OTA format.
"""

from __future__ import annotations

import binascii
import gzip
import hashlib
import re
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from newmota.compression import zopfli_gzip
from newmota.host import load_toml
from newmota.model import ROOT, BuildError, Environment
from newmota.package import LayoutFacts

RBL_HEADER_SIZE = 96
RBL_HEADER = struct.Struct("<4sbbxxI16s24s24sIIII")
RBL_HEADER_BODY_SIZE = RBL_HEADER.size
RBL_HEADER_CRC_SIZE = 4
RBL_SERIALIZED_SIZE = RBL_HEADER_SIZE
CRC_BLOCK_SIZE = 32
CRC_RECORD_SIZE = 34
CRC16_POLY = 0x8005
CRC16_INIT = 0xFFFF
FNV1A_OFFSET = 0x811C9DC5
FNV1A_PRIME = 0x01000193

OTA_ENCRYPTION = {"none": 0, "xor": 1, "aes256": 2}
OTA_COMPRESSION = {"none": 0, "gzip": 1}


class BekenImageError(BuildError):
    """Raised when a Beken image cannot be generated or validated safely."""


@dataclass(frozen=True)
class BekenImageConfig:
    """Image-format inputs owned by one exact Beken layout descriptor."""

    chip: str
    sdk_target: str
    factory_format: str
    application_address: int
    application_container_size: int
    application_coefficients: bytes
    ota_format: str
    ota_compression: str
    ota_encryption: str
    ota_key: bytes | None
    ota_iv: bytes | None
    descriptor: Path

    @property
    def application_physical_size(self) -> int:
        return to_offset(self.application_container_size)

    @property
    def identity(self) -> str:
        document = {
            "chip": self.chip,
            "sdk_target": self.sdk_target,
            "factory_format": self.factory_format,
            "application_address": self.application_address,
            "application_container_size": self.application_container_size,
            "application_coefficients": self.application_coefficients.hex(),
            "ota_format": self.ota_format,
            "ota_compression": self.ota_compression,
            "ota_encryption": self.ota_encryption,
            "ota_key": self.ota_key.decode("ascii") if self.ota_key else None,
            "ota_iv": self.ota_iv.decode("ascii") if self.ota_iv else None,
        }
        encoded = repr(sorted(document.items())).encode("utf-8")
        return hashlib.sha256(encoded).hexdigest()

    def metadata(self) -> dict[str, object]:
        """Return manifest-safe format facts without exposing OTA secrets."""

        try:
            descriptor = str(self.descriptor.relative_to(ROOT))
        except ValueError:
            descriptor = self.descriptor.name
        return {
            "identity": self.identity,
            "chip": self.chip,
            "sdk_target": self.sdk_target,
            "descriptor": descriptor,
            "factory_format": self.factory_format,
            "application_address": self.application_address,
            "application_container_size": self.application_container_size,
            "application_physical_size": self.application_physical_size,
            "application_coefficients_sha256": hashlib.sha256(
                self.application_coefficients
            ).hexdigest(),
            "ota_format": self.ota_format,
            "ota_compression": self.ota_compression,
            "ota_encryption": self.ota_encryption,
            "ota_key_sha256": (hashlib.sha256(self.ota_key).hexdigest() if self.ota_key else None),
            "ota_iv_sha256": hashlib.sha256(self.ota_iv).hexdigest() if self.ota_iv else None,
        }


@dataclass(frozen=True)
class BekenImageSet:
    """Generated images corresponding to one raw linker output."""

    raw: Path
    application: Path
    ota: Path
    config: BekenImageConfig


@dataclass(frozen=True)
class BekenRblHeader:
    """The 96-byte RBL OTA/application header."""

    encryption: int
    compression: int
    timestamp: int
    name: str
    version: str
    serial: str
    data_crc: int
    data_hash: int
    raw_size: int
    data_size: int

    def serialize(self) -> bytes:
        body = RBL_HEADER.pack(
            b"RBL\x00",
            self.encryption,
            self.compression,
            self.timestamp,
            _padded_text(self.name, 16),
            _padded_text(self.version, 24),
            _padded_text(self.serial, 24),
            self.data_crc,
            self.data_hash,
            self.raw_size,
            self.data_size,
        )
        return body + struct.pack("<I", binascii.crc32(body) & 0xFFFFFFFF)

    @classmethod
    def parse(cls, data: bytes) -> BekenRblHeader:
        if len(data) < RBL_SERIALIZED_SIZE:
            raise BekenImageError("Beken RBL header is truncated")
        body = data[:RBL_HEADER_BODY_SIZE]
        if body[:4] != b"RBL\x00":
            raise BekenImageError("Beken OTA image does not start with an RBL header")
        found_crc = struct.unpack_from("<I", data, RBL_HEADER_BODY_SIZE)[0]
        expected_crc = binascii.crc32(body) & 0xFFFFFFFF
        if found_crc != expected_crc:
            raise BekenImageError(
                f"Beken RBL header CRC mismatch: expected {expected_crc:#x}, found {found_crc:#x}"
            )
        (
            _magic,
            encryption,
            compression,
            timestamp,
            name,
            version,
            serial,
            data_crc,
            data_hash,
            raw_size,
            data_size,
        ) = RBL_HEADER.unpack(data[:RBL_HEADER_BODY_SIZE])
        return cls(
            encryption,
            compression,
            timestamp,
            _unpack_text(name),
            _unpack_text(version),
            _unpack_text(serial),
            data_crc,
            data_hash,
            raw_size,
            data_size,
        )


def _padded_text(value: str, size: int) -> bytes:
    encoded = value.encode("ascii")
    if not encoded or len(encoded) > size:
        raise BekenImageError(f"Beken RBL text field is empty or longer than {size} bytes")
    return encoded + b"\x00" * (size - len(encoded))


def _unpack_text(value: bytes) -> str:
    try:
        return value.split(b"\x00", 1)[0].decode("ascii")
    except UnicodeDecodeError as error:
        raise BekenImageError("Beken RBL text field is not ASCII") from error


def _mapping(document: object, field: str) -> dict[str, Any]:
    if not isinstance(document, dict):
        raise BekenImageError(f"Beken image descriptor field '{field}' must be a mapping")
    return document


def _integer(document: dict[str, Any], field: str) -> int:
    value = document.get(field)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise BekenImageError(f"Beken image field '{field}' must be a non-negative integer")
    return value


def _string(document: dict[str, Any], field: str) -> str:
    value = document.get(field)
    if not isinstance(value, str) or not value:
        raise BekenImageError(f"Beken image field '{field}' must be a non-empty string")
    return value


def _hex_bytes(document: dict[str, Any], field: str, length: int) -> bytes:
    value = _string(document, field)
    if not re.fullmatch(rf"[0-9a-fA-F]{{{length * 2}}}", value):
        raise BekenImageError(
            f"Beken image field '{field}' must contain exactly {length} bytes as hexadecimal"
        )
    return bytes.fromhex(value)


def _ascii_bytes(document: dict[str, Any], field: str, length: int) -> bytes:
    value = _string(document, field)
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise BekenImageError(f"Beken image field '{field}' must be ASCII") from error
    if len(encoded) != length:
        raise BekenImageError(
            f"Beken image field '{field}' must contain exactly {length} ASCII bytes"
        )
    return encoded


def load_image_config(environment: Environment) -> BekenImageConfig:
    """Load the exact image policy next to the selected Beken layout."""

    descriptor = environment.partition
    if descriptor is None:
        raise BekenImageError("Beken image packaging requires a layout descriptor")
    try:
        document = load_toml(descriptor)
    except (OSError, UnicodeError, ValueError) as error:
        raise BekenImageError(
            f"cannot read Beken image descriptor {descriptor}: {error}"
        ) from error
    image = _mapping(document.get("image"), "image")
    factory_format = _string(image, "factory_format")
    if factory_format != "app-rbl":
        raise BekenImageError(
            f"Beken factory image format {factory_format!r} is not implemented; expected 'app-rbl'"
        )
    application_container_size = _integer(image, "application_container_size")
    if application_container_size == 0 or application_container_size % CRC_BLOCK_SIZE:
        raise BekenImageError(
            "Beken application container size must be a positive 32-byte multiple"
        )
    application_coefficients = _hex_bytes(image, "application_coefficients", 16)
    ota_format = _string(image, "ota_format")
    if ota_format != "rbl":
        raise BekenImageError(f"Beken OTA format {ota_format!r} is not implemented; expected 'rbl'")
    ota_compression = _string(image, "ota_compression")
    if ota_compression not in OTA_COMPRESSION:
        raise BekenImageError(f"unsupported Beken OTA compression: {ota_compression}")
    ota_encryption = _string(image, "ota_encryption")
    if ota_encryption not in OTA_ENCRYPTION:
        raise BekenImageError(f"unsupported Beken OTA encryption: {ota_encryption}")
    ota_key = _ascii_bytes(image, "ota_key", 32) if ota_encryption == "aes256" else None
    ota_iv = _ascii_bytes(image, "ota_iv", 16) if ota_encryption == "aes256" else None
    if ota_encryption != "aes256" and ("ota_key" in image or "ota_iv" in image):
        raise BekenImageError("Beken OTA key/IV are only valid with aes256 encryption")
    return BekenImageConfig(
        chip=environment.chip,
        sdk_target=environment.sdk_target,
        factory_format=factory_format,
        application_address=_integer(image, "application_address"),
        application_container_size=application_container_size,
        application_coefficients=application_coefficients,
        ota_format=ota_format,
        ota_compression=ota_compression,
        ota_encryption=ota_encryption,
        ota_key=ota_key,
        ota_iv=ota_iv,
        descriptor=descriptor,
    )


def to_offset(address: int) -> int:
    """Convert a Beken logical address to its CRC16-wrapped flash offset."""

    if address < 0:
        raise BekenImageError("Beken image address cannot be negative")
    return address + (address // CRC_BLOCK_SIZE) * 2


def maximum_ota_raw_size(config: BekenImageConfig) -> int:
    """Return the largest raw image that fits the configured factory app container."""

    physical_limit = config.application_physical_size

    def fits(raw_size: int) -> bool:
        factory_payload_logical = _align_up(raw_size + 32, CRC_BLOCK_SIZE)
        factory_header_raw_size = _align_up(raw_size + 48, CRC_BLOCK_SIZE) + 16
        factory_image_physical = to_offset(factory_payload_logical) + 102
        return (
            factory_header_raw_size <= config.application_container_size
            and factory_image_physical <= physical_limit
        )

    low = 0
    high = config.application_container_size
    while low < high:
        middle = (low + high + 1) // 2
        if fits(middle):
            low = middle
        else:
            high = middle - 1
    return low


def ota_compatibility_tag(config: BekenImageConfig, layout: LayoutFacts) -> str:
    """Bind native RBL files to this chip, resolved layout, and image policy."""

    identity = "\0".join(  # noqa: FLY002 - readable NUL-separated identity
        (
            "newmota-beken-rbl-v1",
            config.chip,
            config.sdk_target,
            layout.identity,
            config.identity,
        )
    )
    return "NM1" + hashlib.sha256(identity.encode("utf-8")).hexdigest()[:21]


def _align_up(value: int, alignment: int) -> int:
    return ((value + alignment - 1) // alignment) * alignment


def _crc16_cms(data: bytes) -> int:
    crc = CRC16_INIT
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ CRC16_POLY) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def _crc_wrap(data: bytes) -> bytes:
    output = bytearray()
    for offset in range(0, len(data), CRC_BLOCK_SIZE):
        block = data[offset : offset + CRC_BLOCK_SIZE]
        block += b"\xff" * (CRC_BLOCK_SIZE - len(block))
        output.extend(block)
        output.extend(struct.pack(">H", _crc16_cms(block)))
    return bytes(output)


def _crc_unwrap(data: bytes, unwrapped_size: int) -> bytes:
    if unwrapped_size < 0 or unwrapped_size % CRC_BLOCK_SIZE:
        raise BekenImageError("Beken CRC-wrapped size must be a non-negative 32-byte multiple")
    expected_size = to_offset(unwrapped_size)
    if len(data) != expected_size:
        raise BekenImageError(
            f"Beken CRC-wrapped data has size {len(data)}, expected {expected_size}"
        )
    output = bytearray()
    for offset in range(0, len(data), CRC_RECORD_SIZE):
        block = data[offset : offset + CRC_BLOCK_SIZE]
        found = struct.unpack(">H", data[offset + CRC_BLOCK_SIZE : offset + CRC_RECORD_SIZE])[0]
        expected = _crc16_cms(block)
        if found != expected:
            raise BekenImageError(
                f"Beken CRC16 mismatch at wrapped offset {offset:#x}: "
                f"expected {expected:#x}, found {found:#x}"
            )
        output.extend(block)
    return bytes(output)


def _fnv1a(data: bytes) -> int:
    value = FNV1A_OFFSET
    for byte in data:
        value ^= byte
        value = (value * FNV1A_PRIME) & 0xFFFFFFFF
    return value


def _bits(value: int, high: int, low: int) -> int:
    return (value >> low) & ((1 << (high - low + 1)) - 1)


def _pn15(address: int) -> int:
    first = (address & 0x7F) << 9 | ((address >> 7) & 0x1FF)
    second = (address >> 5) & 0xF
    return first ^ (0x6371 & (second * 0x1111))


def _pn16(address: int) -> int:
    first = (address & 0x3FF) << 7 | ((address >> 10) & 0x7F)
    second = ((address >> 13) & 1) | (((address >> 9) & 1) << 1)
    second |= (((address >> 5) & 1) << 2) | (((address >> 1) & 1) << 3)
    third = (address >> 4) & 1
    return first ^ (0x13659 & (third * 0x10000 | second * 0x1111))


def _pn32(address: int) -> int:
    first = (address & 0x7FFF) << 17 | ((address >> 15) & 0x1FFFF)
    second = (address >> 2) & 0xF
    return first ^ (0xE519A4F1 & (second * 0x11111111))


class _BekenCrypto:
    """Beken application word transform, translated from LibreTiny's MIT code."""

    # Original algorithm reference:
    # https://github.com/ghsecuritylab/tysdk_for_bk7231/blob/master/toolchain/encrypt_crc/abc.c

    def __init__(self, coefficients: bytes) -> None:
        self.coef0, self.coef1, self.coef2, self.coef3 = struct.unpack(">4I", coefficients)
        self.bypass = ((self.coef3 >> 24) & 0xFF) in (0x00, 0xFF)
        if self.bypass or self.coef3 & 0xF == 0xF:
            self.pn15_selection = self.pn16_selection = self.pn32_selection = None
            self.random = 0
            return
        self.pn15_selection = None if self.coef3 & 1 else (self.coef3 >> 5) & 3
        self.pn16_selection = None if self.coef3 & 2 else (self.coef3 >> 8) & 3
        self.pn32_selection = None if self.coef3 & 4 else (self.coef3 >> 11) & 3
        self.coef1_mix = ((self.coef1 >> 8) & 0xFF) << 9
        self.coef1_mix |= ((self.coef3 >> 4) & 1) << 8
        self.coef1_mix |= self.coef1 & 0xFF
        self.random = 0 if self.coef3 & 8 else self.coef2

    def encrypt_word(self, address: int, data: int) -> int:
        if self.bypass:
            return data
        address &= 0xFFFFFFFF
        pn15_value = 0
        if self.pn15_selection is not None:
            choices = (
                (31, 24, 15, 8, 23, 16, 7, 0),
                (31, 24, 15, 8, 7, 0, 23, 16),
                (23, 16, 31, 24, 15, 8, 7, 0),
                (23, 16, 31, 24, 7, 0, 15, 8),
            )[self.pn15_selection]
            first = _bits(address, choices[0], choices[1]) << 8
            first |= _bits(address, choices[2], choices[3])
            second = _bits(address, choices[4], choices[5]) << 8
            second |= _bits(address, choices[6], choices[7])
            pn15_value = _pn15((first ^ second ^ (self.coef1 >> 16)) & 0xFFFF)

        pn16_value = 0
        if self.pn16_selection is not None:
            low = self.pn16_selection
            pn16_value = _pn16(
                (_bits(address, 16 + self.pn16_selection, low) ^ self.coef1_mix) & 0x1FFFF
            )

        pn32_value = 0
        if self.pn32_selection is not None:
            shifts = ((0, 0), (8, 24), (16, 16), (24, 8))[self.pn32_selection]
            pn32_address = (address >> shifts[0]) | ((address << shifts[1]) & 0xFFFFFFFF)
            pn32_value = _pn32((pn32_address ^ self.coef0) & 0xFFFFFFFF)

        mask = pn32_value ^ ((pn15_value << 16) & 0xFFFFFFFF) ^ pn16_value ^ self.random
        return (data ^ mask) & 0xFFFFFFFF


def _crypt_words(data: bytes, address: int, coefficients: bytes) -> bytes:
    crypto = _BekenCrypto(coefficients)
    output = bytearray()
    padded = data + b"\xff" * ((_align_up(len(data), 32)) - len(data))
    for offset in range(0, len(padded), 4):
        word = struct.unpack("<I", padded[offset : offset + 4])[0]
        output.extend(struct.pack("<I", crypto.encrypt_word(address + offset, word)))
    return bytes(output)


def _rbl_update_data(header: BekenRblHeader, data: bytes, raw_size: int) -> BekenRblHeader:
    return BekenRblHeader(
        encryption=header.encryption,
        compression=header.compression,
        timestamp=header.timestamp,
        name=header.name,
        version=header.version,
        serial=header.serial,
        data_crc=binascii.crc32(data) & 0xFFFFFFFF,
        data_hash=_fnv1a(data[:raw_size]),
        raw_size=raw_size,
        data_size=len(data),
    )


def build_application_image(
    raw: Path,
    destination: Path,
    config: BekenImageConfig,
    version: str,
) -> None:
    """Create one CRC-wrapped app image for the selected application slot."""

    data = raw.read_bytes()
    if not data:
        raise BekenImageError("Beken raw application image is empty")
    transformed = _crypt_words(data, config.application_address, config.application_coefficients)
    raw_size = _align_up(len(data) + 16, CRC_BLOCK_SIZE) + 16
    header_data = transformed + b"\xff" * 16 + b"\x10" * 16
    header = _rbl_update_data(
        BekenRblHeader(0, 0, 0, "app", version, "0" * 23, 0, FNV1A_OFFSET, 0, 0),
        header_data,
        raw_size,
    )
    payload = _crc_wrap(transformed + b"\xff" * 32)
    header_bytes = _crc_wrap(header.serialize())
    container_size = config.application_physical_size
    padding = container_size - len(payload) - len(header_bytes)
    if padding < 0:
        raise BekenImageError(
            f"Beken application package needs {len(payload) + len(header_bytes)} bytes, "
            f"but the configured container has {container_size} bytes"
        )
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(payload + b"\xff" * padding + header_bytes)
    validate_application_image(destination, config, None)


def _aes_encrypt(data: bytes, key: bytes, iv: bytes) -> bytes:
    try:
        from Crypto.Cipher import AES
    except ImportError as error:  # pragma: no cover - depends on host installation
        raise BekenImageError(
            "Beken AES OTA packaging requires pycryptodome; run make uv-setup"
        ) from error
    padding = 16 - len(data) % 16
    return AES.new(key, AES.MODE_CBC, iv).encrypt(data + bytes([padding]) * padding)


def _aes_decrypt(data: bytes, key: bytes, iv: bytes) -> bytes:
    try:
        from Crypto.Cipher import AES
    except ImportError as error:  # pragma: no cover - depends on host installation
        raise BekenImageError(
            "Beken AES OTA validation requires pycryptodome; run make uv-setup"
        ) from error
    if not data or len(data) % 16:
        raise BekenImageError("Beken AES OTA payload is not 16-byte aligned")
    decrypted = AES.new(key, AES.MODE_CBC, iv).decrypt(data)
    padding = decrypted[-1]
    if padding == 0 or padding > 16 or decrypted[-padding:] != bytes([padding]) * padding:
        raise BekenImageError("Beken AES OTA payload has invalid padding")
    return decrypted[:-padding]


def build_ota_image(
    raw: Path,
    destination: Path,
    config: BekenImageConfig,
    version: str,
    layout: LayoutFacts,
) -> None:
    """Create the standard RBL OTA package used by the OpenBeken path."""

    data = raw.read_bytes()
    if not data:
        raise BekenImageError("Beken raw application image is empty")
    if len(data) > maximum_ota_raw_size(config):
        raise BekenImageError(
            f"Beken OTA raw image is {len(data)} bytes; configured application container "
            f"allows at most {maximum_ota_raw_size(config)} bytes after native overhead"
        )
    compression = OTA_COMPRESSION[config.ota_compression]
    encryption = OTA_ENCRYPTION[config.ota_encryption]
    payload = data
    if compression == OTA_COMPRESSION["gzip"]:
        # The bootloader inflates it with zlib; Zopfli's deflate is about 10 KB smaller.
        payload = zopfli_gzip(payload)
    if encryption == OTA_ENCRYPTION["aes256"]:
        if config.ota_key is None or config.ota_iv is None:
            raise BekenImageError("Beken AES OTA image has no key and IV")
        payload = _aes_encrypt(payload, config.ota_key, config.ota_iv)
    header = BekenRblHeader(
        encryption=encryption,
        compression=compression,
        timestamp=0,
        name="app",
        version=version,
        serial=ota_compatibility_tag(config, layout),
        data_crc=binascii.crc32(payload) & 0xFFFFFFFF,
        data_hash=_fnv1a(data),
        raw_size=len(data),
        data_size=len(payload),
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(header.serialize() + payload)
    validate_ota_image(destination, config, None, layout)


def validate_application_image(
    image: Path, config: BekenImageConfig, region_size: int | None
) -> dict[str, object]:
    """Validate app CRC records, the trailing RBL header, and its container size."""

    data = image.read_bytes()
    expected_size = config.application_physical_size
    if len(data) != expected_size:
        raise BekenImageError(
            f"Beken application image is {len(data)} bytes; configured container is {expected_size}"
        )
    if region_size is not None and len(data) > region_size:
        raise BekenImageError(
            f"Beken application image is {len(data)} bytes; application region allows {region_size}"
        )
    if len(data) < RBL_SERIALIZED_SIZE:
        raise BekenImageError("Beken application image is smaller than its RBL header")
    header = BekenRblHeader.parse(_crc_unwrap(data[-102:], 96))
    if header.name != "app" or header.encryption != 0 or header.compression != 0:
        raise BekenImageError("Beken factory application has an unexpected RBL header")
    if header.data_size == 0 or header.data_size % CRC_BLOCK_SIZE:
        raise BekenImageError("Beken factory application header has an invalid data size")
    payload_size = to_offset(header.data_size)
    payload = data[:payload_size]
    unwrapped = _crc_unwrap(payload, header.data_size)
    if header.data_size < 32:
        raise BekenImageError("Beken factory application payload is too small")
    logical = unwrapped[:-32] + b"\xff" * 16 + b"\x10" * 16
    if binascii.crc32(logical) & 0xFFFFFFFF != header.data_crc:
        raise BekenImageError("Beken factory application data CRC does not match its header")
    if _fnv1a(logical[: header.raw_size]) != header.data_hash:
        raise BekenImageError("Beken factory application data hash does not match its header")
    if data[payload_size:-102] != b"\xff" * (len(data) - payload_size - 102):
        raise BekenImageError("Beken factory application has non-erased container padding")
    return {
        "format": "app-rbl",
        "size": len(data),
        "container_size": config.application_container_size,
        "rbl_header_offset": len(data) - (RBL_SERIALIZED_SIZE + RBL_HEADER_CRC_SIZE),
        "rbl_raw_size": header.raw_size,
        "rbl_data_size": header.data_size,
    }


def _decode_ota_payload(payload: bytes, header: BekenRblHeader, config: BekenImageConfig) -> bytes:
    data = payload
    if header.encryption == OTA_ENCRYPTION["aes256"]:
        if config.ota_key is None or config.ota_iv is None:
            raise BekenImageError("Beken AES OTA image has no configured key and IV")
        data = _aes_decrypt(data, config.ota_key, config.ota_iv)
    elif header.encryption != OTA_ENCRYPTION["none"]:
        raise BekenImageError("unsupported Beken OTA encryption in RBL image")
    if header.compression == OTA_COMPRESSION["gzip"]:
        try:
            data = gzip.decompress(data)
        except (OSError, EOFError) as error:
            raise BekenImageError(f"Beken OTA gzip payload is invalid: {error}") from error
    elif header.compression != OTA_COMPRESSION["none"]:
        raise BekenImageError("unsupported Beken OTA compression in RBL image")
    return data


def validate_ota_image(
    image: Path,
    config: BekenImageConfig,
    region_size: int | None,
    layout: LayoutFacts,
) -> dict[str, object]:
    """Validate RBL header, payload CRC, and decoded raw-image identity."""

    data = image.read_bytes()
    if region_size is not None and len(data) > region_size:
        raise BekenImageError(
            f"Beken OTA image is {len(data)} bytes; OTA region allows {region_size}"
        )
    header = BekenRblHeader.parse(data[:RBL_SERIALIZED_SIZE])
    if header.name != "app":
        raise BekenImageError(f"Beken OTA RBL name is {header.name!r}, expected 'app'")
    expected_tag = ota_compatibility_tag(config, layout)
    if header.serial != expected_tag:
        raise BekenImageError(
            "Beken OTA RBL compatibility tag does not match this chip, layout, and image policy"
        )
    if header.data_size <= 0 or len(data) != RBL_SERIALIZED_SIZE + header.data_size:
        raise BekenImageError("Beken OTA RBL payload size does not match its header")
    if header.raw_size <= 0 or header.raw_size > maximum_ota_raw_size(config):
        raise BekenImageError("Beken OTA raw image exceeds the configured application capacity")
    if header.encryption != OTA_ENCRYPTION[config.ota_encryption]:
        raise BekenImageError("Beken OTA encryption does not match the layout descriptor")
    if header.compression != OTA_COMPRESSION[config.ota_compression]:
        raise BekenImageError("Beken OTA compression does not match the layout descriptor")
    payload = data[RBL_SERIALIZED_SIZE:]
    if binascii.crc32(payload) & 0xFFFFFFFF != header.data_crc:
        raise BekenImageError("Beken OTA payload CRC does not match its RBL header")
    decoded = _decode_ota_payload(payload, header, config)
    if len(decoded) != header.raw_size or _fnv1a(decoded) != header.data_hash:
        raise BekenImageError("Beken OTA decoded payload does not match its RBL identity")
    return {
        "format": "rbl",
        "size": len(data),
        "payload_size": header.data_size,
        "raw_size": header.raw_size,
        "compression": config.ota_compression,
        "encryption": config.ota_encryption,
    }


def validate_image_layout(config: BekenImageConfig, layout: LayoutFacts) -> None:
    """Check that image addresses and fixed containers fit the resolved layout."""

    application = layout.region("application")
    ota = layout.region("ota")
    if to_offset(config.application_address) != application.offset:
        raise BekenImageError(
            f"Beken image address {config.application_address:#x} maps to "
            f"{to_offset(config.application_address):#x}, not application offset {application.offset:#x}"
        )
    if config.application_physical_size > application.size:
        raise BekenImageError(
            f"Beken application container {config.application_physical_size:#x} exceeds "
            f"application region {application.size:#x}"
        )
    if ota.size <= 0:
        raise BekenImageError("Beken OTA region is empty")


def package_images(
    raw: Path,
    application: Path,
    ota: Path,
    config: BekenImageConfig,
    layout: LayoutFacts,
    version: str,
) -> BekenImageSet:
    """Generate and validate both Beken host/device image forms."""

    validate_image_layout(config, layout)
    build_application_image(raw, application, config, version)
    build_ota_image(raw, ota, config, version, layout)
    validate_application_image(application, config, layout.region("application").size)
    validate_ota_image(ota, config, layout.region("ota").size, layout)
    return BekenImageSet(raw, application, ota, config)
