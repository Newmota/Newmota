"""pico native layouts, image validation, and firmware packaging."""

from __future__ import annotations

import json
import os
import re
import shutil
import struct
import zlib
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from newmota.factory_package import write_factory_package
from newmota.host import SdkVersion, run, sha256, source_state, tools_root
from newmota.model import (
    ROOT,
    WORK_DIR,
    Artifact,
    BuildContext,
    Environment,
    PackageResult,
)
from newmota.ota import OTA_ENVELOPE_FORMAT, create_ota_envelope
from newmota.package import (
    PACKAGE_NAME,
    PACKAGE_SCHEMA,
    LayoutFacts,
    LayoutRegion,
    PackageError,
    _artifact,
    _manifest_native_artifacts,
    create_filesystem_image,
    image_metadata,
    load_package,
    package_manifest_path,
    write_layout_facts,
)

FLASH_BASE = 0x10000000
ERASE_SIZE = 0x1000

# RP2350 4 MiB layout geometry (flash offsets, bytes). Safeboot starts at
# offset zero and embeds the resident partition table; the table-free
# application is stored at 0xD0000 and linked at XIP_BASE because the ROM
# rolls its flash partition into the runtime window when chaining.
# The resolver pins these exactly so an unreviewed geometry edit fails closed.
APPLICATION_OFFSET = 0x0D0000
APPLICATION_SIZE = 0x2D2000
SAFEBOOT_OFFSET = 0x000000
SAFEBOOT_SIZE = 0xD0000
DATA_OFFSET = 0x3A2000
DATA_SIZE = 0x5B000

# Platform-owned sticky-boot marker sector at the start of `data`; the
# LittleFS region starts one erase block later and never touches it.
MARKER_OFFSET = DATA_OFFSET
MARKER_SIZE = ERASE_SIZE
FILESYSTEM_OFFSET = DATA_OFFSET + ERASE_SIZE
FILESYSTEM_SIZE = DATA_SIZE - ERASE_SIZE

E10_RESERVE_OFFSET = 0x3FF000
E10_RESERVE_SIZE = 0x1000
BTSTACK_RESERVE_OFFSET = 0x3FD000
BTSTACK_RESERVE_SIZE = 0x2000

EXECUTABLE_FAMILIES = ("rp2350-arm-s",)
SECURE_RW_PERMISSIONS = {"secure": "rw", "nonsecure": "", "bootloader": "rw"}
RP2040_BOOT2_SIZE = 0x100
RP2040_SRAM_BEGIN = 0x20000000
RP2040_SRAM_END = 0x20042000
RP2040_LAYOUT_IDENTITY_PREFIX = b"NEWMOTA-RP2040-LAYOUT-V1:"

# Every partition the Newmota layout must contain, matched case-sensitively
# against the native JSON names.  Unknown partitions are rejected so
# upstream drift fails closed instead of silently changing the flash map.
REQUIRED_PARTITIONS = ("application", "safeboot", "data")

# Minimum dedicated LittleFS size: 16 blocks of 4 KiB.
LITTLEFS_MIN_SIZE = 0x10000

_SIZE_SUFFIX = re.compile(r"^([0-9]+)[kK]$")


class PicoLayoutError(ValueError):
    """Raised when a Pico partition layout is incomplete or unsafe."""


def _size(value: object, field: str, *, allow_zero: bool = False) -> int:
    if isinstance(value, bool):
        raise PicoLayoutError(f"Pico layout field '{field}' must be a byte size")
    if isinstance(value, int):
        if value < 0 or (value == 0 and not allow_zero):
            raise PicoLayoutError(f"Pico layout field '{field}' must be positive")
        return value
    if isinstance(value, str):
        match = _SIZE_SUFFIX.fullmatch(value.strip())
        if match:
            return int(match.group(1)) * 1024
    raise PicoLayoutError(f"Pico layout field '{field}' must be an integer byte size or '<n>K'")


def _partitions(document: dict[str, Any], path: Path) -> dict[str, dict[str, Any]]:
    try:
        relative = path.relative_to(ROOT)
    except ValueError:
        relative = path
    raw_partitions = document.get("partitions")
    if not isinstance(raw_partitions, list) or not all(
        isinstance(item, dict) for item in raw_partitions
    ):
        raise PicoLayoutError(f"Pico layout {relative} has no partitions list")
    partitions: dict[str, dict[str, Any]] = {}
    for item in raw_partitions:
        entry = dict(item)
        name = entry.get("name")
        if not isinstance(name, str) or not name:
            raise PicoLayoutError(f"Pico layout {relative} has a nameless partition")
        if name in partitions:
            raise PicoLayoutError(f"Pico layout {relative} declares '{name}' more than once")
        partitions[name] = entry
    missing = [name for name in REQUIRED_PARTITIONS if name not in partitions]
    if missing:
        raise PicoLayoutError(
            f"Pico layout {relative} is missing required partitions: " + ", ".join(missing)
        )
    unknown = sorted(set(partitions) - set(REQUIRED_PARTITIONS))
    if unknown:
        raise PicoLayoutError(
            f"Pico layout {relative} has unreviewed partitions: " + ", ".join(unknown)
        )
    return partitions


def _check_partition(
    partitions: dict[str, dict[str, Any]],
    name: str,
    offset: int,
    size: int,
    families: tuple[str, ...],
    relative: Path,
) -> None:
    entry = partitions[name]
    actual_offset = _size(entry.get("start"), f"{name}.start", allow_zero=True)
    actual_size = _size(entry.get("size"), f"{name}.size")
    if actual_offset != offset or actual_size != size:
        raise PicoLayoutError(
            f"Pico partition '{name}' must stay at {offset:#x}/{size:#x} "
            f"(got {actual_offset:#x}/{actual_size:#x} in {relative.name})"
        )
    actual_families = entry.get("families")
    if list(actual_families or []) != list(families):
        raise PicoLayoutError(f"Pico partition '{name}' must keep UF2 families {list(families)}")
    if entry.get("permissions") != dict(SECURE_RW_PERMISSIONS):
        raise PicoLayoutError(f"Pico partition '{name}' must keep secure-rw permissions")
    if entry.get("link") not in (None, ["none", 0]):
        raise PicoLayoutError(f"Pico partition '{name}' must stay independent (no A/B link)")


def resolve_layout(environment: Environment, sdk: SdkVersion, work_dir: Path) -> LayoutFacts:
    """Resolve the target-specific Pico layout into normalized facts."""

    if environment.sdk_target == "rp2040":
        return _resolve_rp2040_layout(environment, work_dir)

    del sdk
    path = environment.partition
    if path is None:
        raise PicoLayoutError("Pico requires a JSON partition default")
    try:
        relative_source = path.relative_to(ROOT)
    except ValueError:
        relative_source = path
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PicoLayoutError(f"cannot read Pico layout {path}: {error}") from error
    if not isinstance(document, dict):
        raise PicoLayoutError(f"Pico layout {relative_source} must be a JSON object")

    flash_bytes = environment.flash_kb * 1024
    if flash_bytes <= 0:
        raise PicoLayoutError("Pico environment has no declared flash capacity")

    unpartitioned = document.get("unpartitioned")
    if not isinstance(unpartitioned, dict):
        raise PicoLayoutError(f"Pico layout {relative_source} has no unpartitioned space")
    if list(unpartitioned.get("families") or []) != ["absolute"]:
        raise PicoLayoutError(
            f"Pico layout {relative_source} must keep unpartitioned UF2 family 'absolute'"
        )

    partitions = _partitions(document, path)
    for name, offset, size, families in (
        ("safeboot", SAFEBOOT_OFFSET, SAFEBOOT_SIZE, EXECUTABLE_FAMILIES),
        ("application", APPLICATION_OFFSET, APPLICATION_SIZE, EXECUTABLE_FAMILIES),
        ("data", DATA_OFFSET, DATA_SIZE, ("data",)),
    ):
        _check_partition(partitions, name, offset, size, families, relative_source)
    regions = [
        LayoutRegion(
            "application",
            "application",
            APPLICATION_OFFSET,
            APPLICATION_SIZE,
            APPLICATION_SIZE,
        ),
        LayoutRegion("safeboot", "safeboot", SAFEBOOT_OFFSET, SAFEBOOT_SIZE, SAFEBOOT_SIZE),
        LayoutRegion("boot-marker", "reserved", MARKER_OFFSET, MARKER_SIZE),
        LayoutRegion(
            "littlefs",
            "littlefs",
            FILESYSTEM_OFFSET,
            FILESYSTEM_SIZE,
            FILESYSTEM_SIZE,
        ),
        LayoutRegion(
            "btstack-reserve",
            "reserved",
            BTSTACK_RESERVE_OFFSET,
            BTSTACK_RESERVE_SIZE,
        ),
        LayoutRegion("e10-reserve", "reserved", E10_RESERVE_OFFSET, E10_RESERVE_SIZE),
    ]

    filesystem = next(region for region in regions if region.name == "littlefs")
    if filesystem.size < LITTLEFS_MIN_SIZE:
        raise PicoLayoutError(
            f"Pico LittleFS region is {filesystem.size:#x} bytes, minimum is {LITTLEFS_MIN_SIZE:#x}"
        )
    if E10_RESERVE_OFFSET + E10_RESERVE_SIZE != flash_bytes:
        raise PicoLayoutError("Pico E10 reserve must end exactly at the flash top")

    try:
        return write_layout_facts(
            platform="pico",
            flash_bytes=flash_bytes,
            erase_size=ERASE_SIZE,
            regions=tuple(regions),
            overlaps=(),
            application_update_reach=("application",),
            settings_filesystem_preserved=True,
            source_files=(relative_source,),
            destination=work_dir / "config" / "layout_facts.json",
        )
    except ValueError as error:
        raise PicoLayoutError(str(error)) from error


def _resolve_rp2040_layout(environment: Environment, work_dir: Path) -> LayoutFacts:
    """Resolve the project-owned RP2040 map; RP2040 ROM has no partition table."""

    path = environment.partition
    if path is None:
        raise PicoLayoutError("RP2040 requires a checked-in flash layout")
    try:
        relative_source = path.relative_to(ROOT)
    except ValueError:
        relative_source = path
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PicoLayoutError(f"cannot read RP2040 layout {path}: {error}") from error
    if not isinstance(document, dict) or set(document) != {
        "schema",
        "chip",
        "flash_bytes",
        "regions",
    }:
        raise PicoLayoutError(
            f"RP2040 layout {relative_source} must contain only schema, chip, flash_bytes, and regions"
        )
    flash_bytes = environment.flash_kb * 1024
    if (
        document.get("schema") != 1
        or document.get("chip") != "rp2040"
        or document.get("flash_bytes") != flash_bytes
    ):
        raise PicoLayoutError(
            f"RP2040 layout {relative_source} does not match the selected chip and flash capacity"
        )
    raw_regions = document.get("regions")
    if not isinstance(raw_regions, list):
        raise PicoLayoutError(f"RP2040 layout {relative_source} has no regions list")

    expected_roles = {
        "safeboot": "safeboot",
        "application": "application",
        "boot-marker": "reserved",
        "littlefs": "littlefs",
    }
    parsed: dict[str, tuple[int, int, str]] = {}
    for index, raw in enumerate(raw_regions):
        if not isinstance(raw, dict) or set(raw) != {"name", "role", "offset", "size"}:
            raise PicoLayoutError(f"RP2040 layout region {index} has invalid fields")
        name = raw.get("name")
        role = raw.get("role")
        offset = raw.get("offset")
        size = raw.get("size")
        if (
            not isinstance(name, str)
            or name not in expected_roles
            or name in parsed
            or role != expected_roles.get(name)
            or not isinstance(offset, int)
            or isinstance(offset, bool)
            or not isinstance(size, int)
            or isinstance(size, bool)
            or offset < 0
            or size <= 0
            or offset % ERASE_SIZE
            or size % ERASE_SIZE
            or offset + size > flash_bytes
        ):
            raise PicoLayoutError(f"RP2040 layout region {index} is invalid")
        parsed[name] = (offset, size, str(role))
    if set(parsed) != set(expected_roles):
        missing = sorted(set(expected_roles) - set(parsed))
        raise PicoLayoutError("RP2040 layout is missing required regions: " + ", ".join(missing))

    safeboot_offset, safeboot_size, _ = parsed["safeboot"]
    app_offset, app_size, _ = parsed["application"]
    marker_offset, marker_size, _ = parsed["boot-marker"]
    filesystem_offset, filesystem_size, _ = parsed["littlefs"]
    if (
        safeboot_offset != 0
        or app_offset != safeboot_offset + safeboot_size
        or marker_offset != app_offset + app_size
        or marker_size != ERASE_SIZE
        or filesystem_offset != marker_offset + marker_size
        or filesystem_offset + filesystem_size != flash_bytes
        or filesystem_size < LITTLEFS_MIN_SIZE
    ):
        raise PicoLayoutError(
            f"RP2040 layout {relative_source} must place Safeboot, application, marker, and LittleFS contiguously"
        )

    regions = tuple(
        LayoutRegion(
            name,
            role,
            offset,
            size,
            size if role in {"application", "safeboot", "littlefs"} else None,
        )
        for name, role in (
            ("safeboot", "safeboot"),
            ("application", "application"),
            ("boot-marker", "reserved"),
            ("littlefs", "littlefs"),
        )
        for offset, size, _ in (parsed[name],)
    )
    try:
        return write_layout_facts(
            platform="pico",
            flash_bytes=flash_bytes,
            erase_size=ERASE_SIZE,
            regions=regions,
            overlaps=(),
            application_update_reach=("application",),
            settings_filesystem_preserved=True,
            source_files=(relative_source,),
            destination=work_dir / "config" / "layout_facts.json",
        )
    except ValueError as error:
        raise PicoLayoutError(str(error)) from error


PICOBIN_MARKER_START = b"\xd3\xde\xff\xff"
PICOBIN_MAX_PARTITION_TABLE_SIZE = 0x280


def _pico_layout(context: BuildContext) -> LayoutFacts:
    if context.layout is None or context.layout.platform != "pico":
        raise PackageError("Pico packaging requires its resolved layout facts")
    return context.layout


def _pico_native(context: BuildContext) -> dict[str, Path]:
    return {
        artifact.path.name: artifact.path
        for artifact in context.artifacts
        if artifact.role == "native" and artifact.path.is_file()
    }


def _check_pico_image(path: Path, name: str, offset: int, limit: int) -> None:
    if not path.is_file():
        raise PackageError(f"Pico image is missing: {path}")
    size = path.stat().st_size
    if size == 0 or size > limit:
        raise PackageError(f"Pico {name} is {size} bytes but its slot allows {limit} bytes")
    if offset % 0x1000:
        raise PackageError(f"Pico {name} must have a 4 KiB-aligned offset")


def _reverse_bits(value: int, width: int) -> int:
    reversed_value = 0
    for _ in range(width):
        reversed_value = (reversed_value << 1) | (value & 1)
        value >>= 1
    return reversed_value


def _check_rp2040_image(
    path: Path,
    name: str,
    offset: int,
    limit: int,
    *,
    layout_identity: str | None = None,
) -> None:
    """Validate the RP2040 boot2 CRC, vector table, and optional Safeboot identity."""

    _check_pico_image(path, name, offset, limit)
    image = path.read_bytes()
    if len(image) < RP2040_BOOT2_SIZE + 8 or len(image) % 4:
        raise PackageError(f"Pico {name} is too short or unaligned for RP2040 boot2 and vectors")
    boot2 = image[:RP2040_BOOT2_SIZE]
    boot2_payload = bytes(_reverse_bits(byte, 8) for byte in boot2[:-4])
    # Pico SDK calls pad_checksum with --seed 0xffffffff, making the zlib
    # initial value zero before its final xor and bit reversal.
    boot2_crc = zlib.crc32(boot2_payload) ^ 0xFFFFFFFF
    expected_checksum = _reverse_bits(boot2_crc, 32)
    if struct.unpack_from("<I", boot2, RP2040_BOOT2_SIZE - 4)[0] != expected_checksum:
        raise PackageError(f"Pico {name} has an invalid RP2040 boot2 checksum")

    stack_pointer, reset_handler = struct.unpack_from("<II", image, RP2040_BOOT2_SIZE)
    image_begin = 0x10000000 + offset
    image_end = image_begin + len(image)
    reset_address = reset_handler & ~1
    if (
        stack_pointer & 7
        or not RP2040_SRAM_BEGIN < stack_pointer <= RP2040_SRAM_END
        or reset_handler & 1 == 0
        or reset_address < image_begin + RP2040_BOOT2_SIZE
        or reset_address >= image_end
    ):
        raise PackageError(f"Pico {name} has invalid RP2040 stack or reset vectors")

    if layout_identity is not None:
        identity = RP2040_LAYOUT_IDENTITY_PREFIX + layout_identity.encode("ascii")
        if identity not in image:
            raise PackageError(f"Pico {name} does not embed its resolved RP2040 layout identity")


def _pico_build_image(native: Mapping[str, Path], owner: str) -> Path:
    matches = sorted(path for name, path in native.items() if name == "newmota.bin")
    if len(matches) != 1:
        raise PackageError(
            f"Pico {owner} build must produce exactly one newmota.bin image; found {len(matches)}"
        )
    return matches[0]


def _pico_partition_embedded(image: Path) -> bool:
    """Return whether one PICOBIN block carries the native partition names.

    ``picotool partition create`` embeds the table as a PICOBIN block item;
    the three partition names therefore cluster within one block of a
    PICOBIN start marker.  A bare name search is insufficient because the
    firmware itself contains similar strings (settings templates).
    """

    content = image.read_bytes()
    names = (b"application", b"safeboot", b"data")
    start = 0
    while True:
        hit = content.find(PICOBIN_MARKER_START, start)
        if hit < 0:
            return False
        window = content[max(0, hit - 64) : hit + 512]
        if all(name in window for name in names):
            return True
        start = hit + len(PICOBIN_MARKER_START)


def _check_pico_partition_embedded(image: Path, owner: str) -> None:
    """Prove the first-stage Safeboot image owns the resident table."""

    if not _pico_partition_embedded(image):
        raise PackageError(f"Pico {owner} image has no embedded native partition table")


def _check_pico_partition_absent(image: Path, owner: str) -> None:
    """Prove a ROM chain target does not contain a second partition table."""

    if _pico_partition_embedded(image):
        raise PackageError(f"Pico {owner} image unexpectedly embeds a partition table")


def _pico_picotool(context: BuildContext) -> Path:
    sdk = context.sdk
    if sdk.programmer != "picotool" or sdk.programmer_status == "unselected":
        raise PackageError("Pico packaging requires a selected picotool pin")
    root = tools_root() / "pico-sdk" / f"picotool-{sdk.programmer_version}" / "picotool"
    executable = root.with_name("picotool.exe") if os.name == "nt" else root / "picotool"
    if not executable.is_file():
        raise PackageError(f"Pico picotool executable is missing: {executable}")
    return executable


def _pico_partition_table_artifact(
    context: BuildContext, layout: LayoutFacts, factory_dir: Path
) -> Path:
    """Render the reference partition-table binary from the layout JSON."""

    source = ROOT / layout.source_files[0]
    destination = factory_dir / "partition-table.bin"
    try:
        run(
            [
                str(_pico_picotool(context)),
                "partition",
                "create",
                str(source),
                str(destination),
                "-t",
                "bin",
            ]
        )
    except Exception as error:
        raise PackageError(f"Pico partition table rendering failed: {error}") from error
    content = destination.read_bytes()
    if (
        not content
        or len(content) > PICOBIN_MAX_PARTITION_TABLE_SIZE
        or not content.startswith(PICOBIN_MARKER_START)
    ):
        raise PackageError("Pico partition table binary has an invalid envelope")
    return destination


def _verified_pico_safeboot_dependency(
    context: BuildContext, layout: LayoutFacts
) -> dict[str, Path]:
    item = context.environment
    if item.safeboot is None:
        raise PackageError(f"Pico application {item.name} has no safeboot environment")
    manifest_path = WORK_DIR / item.safeboot / "artifacts.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PackageError(
            f"cannot read safeboot build manifest {manifest_path.relative_to(ROOT)}: {error}"
        ) from error
    if not isinstance(manifest, dict):
        raise PackageError("safeboot build manifest must contain an object")
    target = manifest.get("target")
    sdk = manifest.get("sdk")
    pipeline = manifest.get("pipeline")
    configuration = manifest.get("configuration")
    partition = configuration.get("partition") if isinstance(configuration, dict) else None
    layout_configuration = configuration.get("layout") if isinstance(configuration, dict) else None
    expected_partition = str(layout.source_files[0])
    if (
        not isinstance(target, dict)
        or not isinstance(sdk, dict)
        or not isinstance(pipeline, dict)
        or not isinstance(partition, dict)
        or manifest.get("version") != context.version
        or target.get("environment") != item.safeboot
        or target.get("board") != item.board
        or target.get("chip") != item.chip
        or target.get("sdk_target") != item.sdk_target
        or target.get("role") != "safeboot"
        or target.get("profile") != "Safeboot"
        or sdk.get("revision") != context.sdk.revision
        or not isinstance(layout_configuration, dict)
        or layout_configuration.get("identity") != layout.identity
        or pipeline.get("package_status") != "safeboot"
        or manifest.get("flashable") is not False
        or manifest.get("source") != source_state()
        or partition.get("file") != expected_partition
    ):
        raise PackageError(
            f"safeboot build {item.safeboot} does not match the application build identity"
        )

    root = WORK_DIR / item.safeboot
    native = {
        artifact.path.name: artifact.path
        for artifact in _manifest_native_artifacts(manifest, root, "safeboot")
    }
    return {"image": _pico_build_image(native, "Safeboot")}


def validate_pico_safeboot(context: BuildContext) -> PackageResult:
    """Validate a Safeboot image against the resolved Pico safeboot slot."""

    item = context.environment
    if item.role != "safeboot" or item.profile != "Safeboot":
        raise PackageError("Pico safeboot validation requires the Safeboot role and profile")
    layout = _pico_layout(context)
    native = _pico_native(context)
    safeboot = layout.region("safeboot")
    image = _pico_build_image(native, "Safeboot")
    _check_pico_image(image, "Safeboot image", safeboot.offset, safeboot.size)
    if item.sdk_target == "rp2040":
        _check_rp2040_image(
            image,
            "Safeboot image",
            safeboot.offset,
            safeboot.size,
            layout_identity=layout.identity,
        )
    else:
        # Safeboot is the RP2350 ROM's first image and owns the resident table.
        _check_pico_partition_embedded(image, "Safeboot")
    print(
        f"Safeboot image: {image.relative_to(ROOT)} ({image.stat().st_size}/{safeboot.size} bytes)"
    )
    return PackageResult(status="safeboot", flashable=False, deployable=False)


def package_pico(context: BuildContext) -> PackageResult:
    """Create a paired Pico picotool factory package."""

    item = context.environment
    if item.role != "application":
        raise PackageError("Pico factory packaging requires an application environment")
    if not PACKAGE_NAME.fullmatch(item.name) or not PACKAGE_NAME.fullmatch(context.version):
        raise PackageError("environment and version must be safe firmware filename components")

    layout = _pico_layout(context)
    native = _pico_native(context)
    application = layout.region("application")
    safeboot = layout.region("safeboot")
    filesystem = layout.region("littlefs")
    marker = layout.region("boot-marker")

    image = _pico_build_image(native, "application")
    _check_pico_image(image, "application image", application.offset, application.size)
    if item.sdk_target == "rp2040":
        _check_rp2040_image(image, "application image", application.offset, application.size)
    else:
        _check_pico_partition_absent(image, "application")
    safeboot_image = _verified_pico_safeboot_dependency(context, layout)["image"]
    _check_pico_image(safeboot_image, "Safeboot image", safeboot.offset, safeboot.size)
    if item.sdk_target == "rp2040":
        _check_rp2040_image(
            safeboot_image,
            "Safeboot image",
            safeboot.offset,
            safeboot.size,
            layout_identity=layout.identity,
        )
    else:
        _check_pico_partition_embedded(safeboot_image, "Safeboot")

    staging = context.work_dir / "package-staging"
    if staging.exists():
        shutil.rmtree(staging)
    factory_dir = staging / "factory"
    ota_dir = staging / "ota"
    debug_dir = staging / "debug"
    factory_dir.mkdir(parents=True)
    ota_dir.mkdir()
    debug_dir.mkdir()

    components: list[dict[str, object]] = []
    artifact_records: list[dict[str, object]] = []

    def add_component(
        name: str,
        source: Path,
        offset: int,
        *,
        update_role: str | None = None,
        update_size_limit: int | None = None,
    ) -> None:
        destination = factory_dir / name
        shutil.copy2(source, destination)
        relative = str(destination.relative_to(staging))
        component = {
            "name": name,
            "role": "factory-component",
            "path": relative,
            "offset": offset,
        }
        if update_role is not None:
            component["update_role"] = update_role
            component["update_size_limit"] = update_size_limit
        components.append(component)
        artifact_records.append(
            _artifact(
                staging,
                destination,
                "factory-component",
                offset=offset,
                name=name,
                **(
                    {
                        "update_role": update_role,
                        "update_size_limit": update_size_limit,
                    }
                    if update_role is not None
                    else {}
                ),
            )
        )

    add_component("safeboot.bin", safeboot_image, safeboot.offset)
    add_component(
        "application.bin",
        image,
        application.offset,
    )
    ota_image = ota_dir / "application.nmo"
    envelope = create_ota_envelope(
        image,
        ota_image,
        chip=item.chip,
        flash_bytes=layout.flash_bytes,
        layout_sha256=layout.identity,
        application=(application.offset, application.size),
        safeboot=(safeboot.offset, safeboot.size),
        filesystem=(filesystem.offset, filesystem.size),
    )
    artifact_records.append(
        _artifact(
            staging,
            ota_image,
            "application-update",
            format=OTA_ENVELOPE_FORMAT,
            native_format=(
                "rp2040-boot2-vector-image" if item.sdk_target == "rp2040" else "rp2350-rom-image"
            ),
            native_size=image.stat().st_size,
            native_payload_overhead=0,
            offset=application.offset,
            size_limit=application.size,
            envelope=envelope,
        )
    )
    filesystem_image = factory_dir / "littlefs.bin"
    create_filesystem_image(filesystem_image, filesystem.size, context.environment.filesystem)
    filesystem_details = image_metadata(filesystem.offset, filesystem.size)
    components.append(
        {
            "name": "littlefs.bin",
            "role": "factory-component",
            "path": str(filesystem_image.relative_to(staging)),
            "offset": filesystem.offset,
        }
    )
    artifact_records.append(
        _artifact(
            staging,
            filesystem_image,
            "factory-component",
            offset=filesystem.offset,
            name="littlefs.bin",
            filesystem=filesystem_details,
        )
    )
    if item.sdk_target != "rp2040":
        partition_table = _pico_partition_table_artifact(context, layout, factory_dir)
        artifact_records.append(
            _artifact(
                staging,
                partition_table,
                "factory-reference",
                name="partition-table.bin",
            )
        )

    for artifact in context.artifacts:
        if artifact.role != "debug" or not artifact.path.is_file():
            continue
        destination = debug_dir / artifact.path.name
        shutil.copy2(artifact.path, destination)
        artifact_records.append(_artifact(staging, destination, "debug"))

    layout_metadata = {
        "file": str(layout.source_files[0]),
        "sha256": sha256(ROOT / layout.source_files[0]),
        "flash_bytes": layout.flash_bytes,
        "erase_size": layout.erase_size,
        "regions": [region.metadata() for region in layout.regions],
        "identity": layout.identity,
        "application": {"offset": application.offset, "size": application.size},
        "safeboot": {"offset": safeboot.offset, "size": safeboot.size},
        "filesystem": {"offset": filesystem.offset, "size": filesystem.size},
        "marker": {"offset": marker.offset, "size": marker.size},
    }
    manifest: dict[str, object] = {
        "schema": PACKAGE_SCHEMA,
        "product": "newmota",
        "version": context.version,
        "source": source_state(),
        "target": {
            "environment": item.name,
            "board": item.board,
            "chip": item.chip,
            "sdk_target": item.sdk_target,
            "sdk_board": item.sdk_board,
            "platform": item.platform,
            "role": item.role,
            "profile": item.profile,
            "language": item.language,
            "flash_bytes": item.flash_kb * 1024,
            "psram_bytes": item.psram_kb * 1024 if item.psram_kb is not None else None,
        },
        "sdk": {
            "name": context.sdk.name,
            "version": context.sdk.version,
            "revision": context.sdk.revision,
            "programmer": context.sdk.programmer,
            "programmer_version": context.sdk.programmer_version,
            "programmer_status": context.sdk.programmer_status,
        },
        "host_tools": {
            "picotool": context.sdk.programmer_version,
            "protocol": "picotool-usb",
            "littlefs-python": filesystem_details["generator"],
        },
        "configuration": {
            "environment": item.configuration_provenance(),
            "native": context.native_config.identity,
            "features": context.features.identity,
            "hardware": context.hardware.identity,
            "partition": {
                "file": str(layout.source_files[0]),
                "sha256": sha256(ROOT / layout.source_files[0]),
            },
            "layout": layout.identity,
        },
        "layout": layout_metadata,
        "qualification": {
            "factory": "pico-usb",
            "safeboot": "pico-paired",
            "ota": "pico-wired-application",
            "deployment": "unqualified",
        },
        "flashable": True,
        "deployable": False,
        "signing": {"status": "unsigned-development"},
        "ota": {
            "status": "pico-wired-application",
            "artifacts": [
                {
                    "role": "application-update",
                    "path": "ota/application.nmo",
                    "offset": application.offset,
                    "size_limit": application.size,
                    "size": ota_image.stat().st_size,
                    "payload_size": envelope["payload_size"],
                    "native_size": image.stat().st_size,
                    "native_payload_overhead": 0,
                    "native_format": (
                        "rp2040-boot2-vector-image"
                        if item.sdk_target == "rp2040"
                        else "rp2350-rom-image"
                    ),
                }
            ],
        },
        "safeboot": {
            "environment": item.safeboot,
            "path": "factory/safeboot.bin",
            "offset": safeboot.offset,
            "size_limit": safeboot.size,
        },
        "flash": {
            "protocol": "picotool-usb",
            "protocol_version": "picotool",
            "transport": "usb-bootsel",
            "chipname": item.sdk_target,
            "components": components,
        },
        "monitor": {"transport": "usb-cdc"},
        "artifacts": artifact_records,
    }
    staging_manifest = staging / "manifest.json"
    staging_manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    destination = package_manifest_path(item.name, context.version).parent
    if destination.exists():
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging.rename(destination)
    print(f"Firmware package: {(destination / 'manifest.json').relative_to(ROOT)}")

    validated = load_package(destination / "manifest.json")
    print(f"Factory package: {write_factory_package(validated).relative_to(ROOT)}")
    artifacts = tuple(
        Artifact(validated.file(str(record["path"])), str(record["role"]))
        for record in validated.data["artifacts"]
    )
    return PackageResult(status="flashable", flashable=True, deployable=False, artifacts=artifacts)
