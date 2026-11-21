"""amebad native layouts, image validation, and firmware packaging."""

from __future__ import annotations

import json
import shutil
from dataclasses import dataclass
from itertools import pairwise
from pathlib import Path
from typing import Any

from newmota.factory_package import write_factory_package
from newmota.host import SdkVersion, load_toml, sha256, source_state
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
    create_filesystem_image,
    image_metadata,
    load_package,
    package_manifest_path,
    write_layout_facts,
)

FLASH_BASE = 0x08000000
FLASHLOADER_ADDRESS = 0x082000
BOOT_BAUD = 115200
DEFAULT_UPLOAD_BAUD = 230_400


@dataclass(frozen=True)
class AmebadImage:
    """One vendor AmebaD image and its physical flash offset."""

    name: str
    offset: int

    @property
    def address(self) -> int:
        """Return the mapped address used by the UART flashloader."""

        return FLASH_BASE + self.offset


# These are the two fixed vendor images which precede the OTA regions.  The
# combined image is deliberately not listed here: factory packaging places a
# copy in each independently selectable OTA region.
BOOT_IMAGE_LAYOUT = (
    AmebadImage("km0_boot_all.bin", 0x000000),
    AmebadImage("km4_boot_all.bin", 0x004000),
)


class AmebadLayoutError(ValueError):
    """Raised when an AmebaD partition layout is incomplete or unsafe."""


@dataclass(frozen=True)
class AmebadRegion:
    """One bounded physical AmebaD flash region."""

    name: str
    offset: int
    size: int

    @property
    def end(self) -> int:
        """Return the exclusive physical end offset."""

        return self.offset + self.size


@dataclass(frozen=True)
class AmebadLayout:
    """Validated Newmota-owned OTA and filesystem layout for AmebaD."""

    path: Path
    flash_bytes: int
    ota1: AmebadRegion
    ota2: AmebadRegion
    filesystem: AmebadRegion
    filesystem_block_size: int

    def metadata(self) -> dict[str, object]:
        """Return stable manifest metadata for this layout."""

        return {
            "file": str(self.path),
            "flash_bytes": self.flash_bytes,
            "ota1": {"offset": self.ota1.offset, "size": self.ota1.size},
            "ota2": {"offset": self.ota2.offset, "size": self.ota2.size},
            "filesystem": {
                "offset": self.filesystem.offset,
                "size": self.filesystem.size,
                "block_size": self.filesystem_block_size,
            },
        }


def _integer(value: object, field: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise AmebadLayoutError(f"AmebaD layout field '{field}' must be an integer")
    return value


def _region(document: dict[str, Any], name: str, flash_bytes: int) -> AmebadRegion:
    raw = document.get(name)
    if not isinstance(raw, dict):
        raise AmebadLayoutError(f"AmebaD layout is missing [{name}]")
    offset = _integer(raw.get("offset"), f"{name}.offset")
    size = _integer(raw.get("size"), f"{name}.size")
    if offset < 0 or size <= 0 or offset % 0x1000 or size % 0x1000:
        raise AmebadLayoutError(f"AmebaD {name} must be positive and 4 KiB aligned")
    if offset + size > flash_bytes:
        raise AmebadLayoutError(f"AmebaD {name} exceeds the declared flash capacity")
    return AmebadRegion(name, offset, size)


def load_layout(path: Path, flash_bytes: int) -> AmebadLayout:
    """Read and validate a Newmota AmebaD TOML layout."""

    try:
        document = load_toml(path)
    except (OSError, UnicodeError, ValueError) as error:
        raise AmebadLayoutError(f"cannot read AmebaD layout {path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema") != 1:
        raise AmebadLayoutError("AmebaD layout schema must be 1")
    declared_flash = _integer(document.get("flash_bytes"), "flash_bytes")
    if declared_flash != flash_bytes:
        raise AmebadLayoutError(
            f"AmebaD layout declares {declared_flash} bytes, expected {flash_bytes}"
        )

    ota1 = _region(document, "ota1", flash_bytes)
    ota2 = _region(document, "ota2", flash_bytes)
    filesystem = _region(document, "filesystem", flash_bytes)
    regions = tuple(sorted((ota1, ota2, filesystem), key=lambda region: region.offset))
    for left, right in pairwise(regions):
        if left.end > right.offset:
            raise AmebadLayoutError(f"AmebaD regions {left.name} and {right.name} overlap")

    raw_filesystem = document["filesystem"]
    block_size = _integer(raw_filesystem.get("block_size", 0x1000), "filesystem.block_size")
    if block_size != 0x1000:
        raise AmebadLayoutError("AmebaD filesystem block_size must be 4096")
    if filesystem.size < 2 * block_size:
        raise AmebadLayoutError("AmebaD filesystem must contain at least two blocks")
    return AmebadLayout(path, flash_bytes, ota1, ota2, filesystem, block_size)


def resolve_layout(environment: Environment, sdk: SdkVersion, work_dir: Path) -> LayoutFacts:
    """Resolve the AmebaD OTA slots and filesystem into shared layout facts."""

    del sdk
    path = environment.partition
    if path is None:
        raise AmebadLayoutError("AmebaD requires a TOML partition default")
    try:
        relative_source = path.relative_to(ROOT)
    except ValueError:
        relative_source = path
    flash_bytes = environment.flash_kb * 1024
    layout = load_layout(path, flash_bytes)
    regions = (
        LayoutRegion("boot-system", "vendor", 0, layout.ota1.offset),
        LayoutRegion("safeboot", "safeboot", layout.ota1.offset, layout.ota1.size),
        LayoutRegion("application", "application", layout.ota2.offset, layout.ota2.size),
        LayoutRegion("littlefs", "littlefs", layout.filesystem.offset, layout.filesystem.size),
    )
    try:
        return write_layout_facts(
            platform="amebad",
            flash_bytes=flash_bytes,
            erase_size=layout.filesystem_block_size,
            regions=regions,
            overlaps=(),
            application_update_reach=("application",),
            settings_filesystem_preserved=True,
            source_files=(relative_source,),
            destination=work_dir / "config" / "layout_facts.json",
        )
    except ValueError as error:
        raise AmebadLayoutError(str(error)) from error


def _amebad_layout(context: BuildContext) -> AmebadLayout:
    item = context.environment
    if item.partition is None:
        raise PackageError("AmebaD requires a resolved partition layout")
    try:
        return load_layout(item.partition, item.flash_kb * 1024)
    except AmebadLayoutError as error:
        raise PackageError(str(error)) from error


def _amebad_native(context: BuildContext) -> dict[str, Path]:
    return {
        artifact.path.name: artifact.path
        for artifact in context.artifacts
        if artifact.role == "native" and artifact.path.is_file()
    }


def _check_amebad_image(
    path: Path, name: str, offset: int, limit: int, *, aligned_size: bool = True
) -> None:
    if not path.is_file():
        raise PackageError(f"AmebaD image is missing: {path}")
    size = path.stat().st_size
    if size == 0 or size > limit:
        raise PackageError(f"AmebaD {name} is {size} bytes but its slot allows {limit} bytes")
    if offset % 0x1000 or (aligned_size and size % 0x1000):
        raise PackageError(
            f"AmebaD {name} must have a 4 KiB-aligned offset"
            + (" and size" if aligned_size else "")
        )


def _normalize_amebad_ota_all(path: Path, image: Path, size_limit: int) -> tuple[bytes, int]:
    """Validate the SDK OTA_All container and fill its native additive checksum.

    The pinned SDK ships an x86_64-only checksum helper on Darwin. On Apple
    Silicon its shell script leaves the OTA checksum field at zero, so Newmota
    calculates the same additive checksum that the device verifies.
    """

    content = path.read_bytes()
    if len(content) < 40:
        raise PackageError("AmebaD OTA_All.bin is shorter than its header and signature")
    header = content[:32]
    image_length = int.from_bytes(header[20:24], "little")
    expected_checksum = int.from_bytes(header[16:20], "little")
    raw_image = content[32:]
    if (
        int.from_bytes(header[0:4], "little") != 0xFFFFFFFF
        or int.from_bytes(header[4:8], "little") != 1
        or header[8:12] != b"OTA1"
        or int.from_bytes(header[12:16], "little") != 24
        or int.from_bytes(header[24:28], "little") != 32
        or int.from_bytes(header[28:32], "little") != 0xFFFFFFFF
        or image_length != len(raw_image)
        or raw_image != image.read_bytes()
    ):
        raise PackageError("AmebaD OTA_All header or application payload is incompatible")
    if image_length > size_limit:
        raise PackageError(
            f"AmebaD application is {image_length} bytes but its slot allows {size_limit} bytes"
        )
    checksum = sum(raw_image) & 0xFFFFFFFF
    if expected_checksum not in (0, checksum):
        raise PackageError("AmebaD OTA_All checksum does not match its native image")
    normalized_header = bytearray(header)
    normalized_header[16:20] = checksum.to_bytes(4, "little")
    return bytes(normalized_header) + raw_image, image_length


def _amebad_artifact_from_manifest(
    manifest: dict[str, object], role: str, filename: str, root: Path
) -> Path:
    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, list):
        raise PackageError("safeboot build manifest has no artifacts")
    matches = [
        item
        for item in artifacts
        if isinstance(item, dict)
        and item.get("role") == role
        and isinstance(item.get("path"), str)
        and Path(str(item["path"])).name == filename
    ]
    if len(matches) != 1:
        raise PackageError(f"safeboot build manifest does not contain exactly one {filename}")
    record = matches[0]
    relative = str(record["path"])
    path = (ROOT / relative).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError as error:
        raise PackageError(
            f"safeboot artifact is outside its build directory: {relative}"
        ) from error
    if not path.is_file():
        raise PackageError(f"safeboot artifact is missing: {relative}")
    if record.get("size") != path.stat().st_size or record.get("sha256") != sha256(path):
        raise PackageError(f"safeboot artifact does not match its build manifest: {relative}")
    return path


def _verified_amebad_safeboot_dependency(
    context: BuildContext, layout: AmebadLayout
) -> dict[str, Path]:
    item = context.environment
    if item.safeboot is None:
        raise PackageError(f"AmebaD application {item.name} has no safeboot environment")
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
    layout_facts = context.layout
    if layout_facts is None:
        raise PackageError("AmebaD build has no resolved layout identity")
    expected_partition = str(layout.path.relative_to(ROOT))
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
        or layout_configuration.get("identity") != layout_facts.identity
        or pipeline.get("package_status") != "safeboot"
        or manifest.get("flashable") is not False
        or manifest.get("source") != source_state()
        or partition.get("file") != expected_partition
    ):
        raise PackageError(
            f"safeboot build {item.safeboot} does not match the application build identity"
        )

    root = WORK_DIR / item.safeboot
    return {
        "km0_boot_all.bin": _amebad_artifact_from_manifest(
            manifest, "native", "km0_boot_all.bin", root
        ),
        "km4_boot_all.bin": _amebad_artifact_from_manifest(
            manifest, "native", "km4_boot_all.bin", root
        ),
        "km0_km4_image2.bin": _amebad_artifact_from_manifest(
            manifest, "native", "km0_km4_image2.bin", root
        ),
    }


def validate_amebad_safeboot(context: BuildContext) -> PackageResult:
    """Validate a Safeboot image against the AmebaD OTA1 slot."""

    item = context.environment
    if item.role != "safeboot" or item.profile != "Safeboot":
        raise PackageError("AmebaD safeboot validation requires the Safeboot role and profile")
    layout = _amebad_layout(context)
    native = _amebad_native(context)
    missing = [image.name for image in BOOT_IMAGE_LAYOUT if image.name not in native]
    if "km0_km4_image2.bin" not in native or missing:
        raise PackageError(
            "AmebaD Safeboot build did not produce: "
            + ", ".join((*missing, "km0_km4_image2.bin") if missing else ("km0_km4_image2.bin",))
        )
    for image in BOOT_IMAGE_LAYOUT:
        _check_amebad_image(
            native[image.name],
            image.name,
            image.offset,
            layout.ota1.offset - image.offset,
            aligned_size=False,
        )
    image = native["km0_km4_image2.bin"]
    _check_amebad_image(image, "Safeboot image2", layout.ota1.offset, layout.ota1.size)
    print(
        f"Safeboot image: {image.relative_to(ROOT)} "
        f"({image.stat().st_size}/{layout.ota1.size} bytes)"
    )
    return PackageResult(status="safeboot", flashable=False, deployable=False)


def package_amebad(context: BuildContext, flashloader: Path) -> PackageResult:
    """Create a paired AmebaD UART factory package."""

    item = context.environment
    if item.role != "application":
        raise PackageError("AmebaD factory packaging requires an application environment")
    if context.sdk.programmer_status == "unselected":
        raise PackageError("AmebaD Python programmer is not selected in its SDK pin")
    if not PACKAGE_NAME.fullmatch(item.name) or not PACKAGE_NAME.fullmatch(context.version):
        raise PackageError("environment and version must be safe firmware filename components")
    if not flashloader.is_file():
        raise PackageError(f"AmebaD flashloader is missing: {flashloader}")

    layout = _amebad_layout(context)
    layout_facts = context.layout
    if layout_facts is None:
        raise PackageError("AmebaD build has no resolved layout identity")
    native = _amebad_native(context)
    missing = [image.name for image in BOOT_IMAGE_LAYOUT if image.name not in native]
    if "km0_km4_image2.bin" not in native or missing:
        raise PackageError(
            "AmebaD application build did not produce: "
            + ", ".join((*missing, "km0_km4_image2.bin") if missing else ("km0_km4_image2.bin",))
        )
    safeboot = _verified_amebad_safeboot_dependency(context, layout)
    if "OTA_All.bin" not in native:
        raise PackageError("AmebaD application build did not produce OTA_All.bin")
    ota_all_payload, ota_image_size = _normalize_amebad_ota_all(
        native["OTA_All.bin"], native["km0_km4_image2.bin"], layout.ota2.size
    )

    for image in BOOT_IMAGE_LAYOUT:
        _check_amebad_image(
            native[image.name],
            image.name,
            image.offset,
            layout.ota1.offset - image.offset,
            aligned_size=False,
        )
        if sha256(native[image.name]) != sha256(safeboot[image.name]):
            raise PackageError(f"AmebaD application and Safeboot {image.name} differ")
    _check_amebad_image(
        safeboot["km0_km4_image2.bin"],
        "Safeboot image2",
        layout.ota1.offset,
        layout.ota1.size,
    )
    _check_amebad_image(
        native["km0_km4_image2.bin"],
        "application image2",
        layout.ota2.offset,
        layout.ota2.size,
    )

    staging = context.work_dir / "package-staging"
    if staging.exists():
        shutil.rmtree(staging)
    factory_dir = staging / "factory"
    ota_dir = staging / "ota"
    programmer_dir = staging / "programmer"
    debug_dir = staging / "debug"
    factory_dir.mkdir(parents=True)
    ota_dir.mkdir()
    programmer_dir.mkdir()
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

    for image in BOOT_IMAGE_LAYOUT:
        add_component(image.name, native[image.name], image.offset)
    add_component(
        "safeboot-image2.bin",
        safeboot["km0_km4_image2.bin"],
        layout.ota1.offset,
    )
    add_component(
        "application-image2.bin",
        native["km0_km4_image2.bin"],
        layout.ota2.offset,
    )
    ota_image = ota_dir / "application.nmo"
    normalized_ota_all = ota_dir / ".application-ota-all.tmp"
    normalized_ota_all.write_bytes(ota_all_payload)
    try:
        envelope = create_ota_envelope(
            normalized_ota_all,
            ota_image,
            chip=item.chip,
            flash_bytes=layout.flash_bytes,
            layout_sha256=layout_facts.identity,
            application=(layout.ota2.offset, layout.ota2.size),
            safeboot=(layout.ota1.offset, layout.ota1.size),
            filesystem=(layout.filesystem.offset, layout.filesystem.size),
        )
    finally:
        normalized_ota_all.unlink(missing_ok=True)
    artifact_records.append(
        _artifact(
            staging,
            ota_image,
            "application-update",
            format=OTA_ENVELOPE_FORMAT,
            native_format="amebad-ota-all",
            native_size=ota_image_size,
            native_payload_overhead=32,
            offset=layout.ota2.offset,
            size_limit=layout.ota2.size,
            envelope=envelope,
        )
    )
    filesystem = factory_dir / "littlefs.bin"
    create_filesystem_image(filesystem, layout.filesystem.size, context.environment.filesystem)
    filesystem_details = image_metadata(layout.filesystem.offset, layout.filesystem.size)
    filesystem_component = {
        "name": "littlefs.bin",
        "role": "factory-component",
        "path": str(filesystem.relative_to(staging)),
        "offset": layout.filesystem.offset,
    }
    components.append(filesystem_component)
    artifact_records.append(
        _artifact(
            staging,
            filesystem,
            "factory-component",
            offset=layout.filesystem.offset,
            name="littlefs.bin",
            filesystem=filesystem_details,
        )
    )

    loader_destination = programmer_dir / "imgtool_flashloader_amebad.bin"
    shutil.copy2(flashloader, loader_destination)
    loader_relative = str(loader_destination.relative_to(staging))
    artifact_records.append(_artifact(staging, loader_destination, "programmer"))

    for artifact in context.artifacts:
        if artifact.role != "debug" or not artifact.path.is_file():
            continue
        destination = debug_dir / artifact.path.name
        shutil.copy2(artifact.path, destination)
        artifact_records.append(_artifact(staging, destination, "debug"))

    layout_metadata = {
        "file": str(layout.path.relative_to(ROOT)),
        "sha256": sha256(layout.path),
        "identity": layout_facts.identity,
        "flash_bytes": layout.flash_bytes,
        "ota1": {"offset": layout.ota1.offset, "size": layout.ota1.size},
        "ota2": {"offset": layout.ota2.offset, "size": layout.ota2.size},
        "filesystem": {
            "offset": layout.filesystem.offset,
            "size": layout.filesystem.size,
            "block_size": layout.filesystem_block_size,
        },
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
            "programmer_sha256": context.sdk.programmer_sha256,
        },
        "host_tools": {
            "amebad_uploader": context.sdk.programmer_version,
            "protocol": "realtek-amebad-uart",
        },
        "configuration": {
            "environment": item.configuration_provenance(),
            "native": context.native_config.identity,
            "features": context.features.identity,
            "hardware": context.hardware.identity,
            "partition": layout_metadata,
            "layout": layout_facts.identity,
        },
        "layout": layout_metadata,
        "qualification": {
            "factory": "amebad-community-python",
            "safeboot": "amebad-paired",
            "ota": "amebad-wired-ota2",
            "deployment": "unqualified",
        },
        "flashable": True,
        "deployable": False,
        "signing": {"status": "unsigned-development"},
        "ota": {
            "status": "amebad-wired-ota2",
            "artifacts": [
                {
                    "role": "application-update",
                    "path": "ota/application.nmo",
                    "offset": layout.ota2.offset,
                    "size_limit": layout.ota2.size,
                    "size": ota_image.stat().st_size,
                    "payload_size": envelope["payload_size"],
                    "native_size": ota_image_size,
                    "native_payload_overhead": 32,
                    "native_format": "amebad-ota-all",
                }
            ],
        },
        "flash": {
            "protocol": "amebad-uart",
            "protocol_version": "community-python",
            "loader": loader_relative,
            "baud": DEFAULT_UPLOAD_BAUD,
            "before": "manual_uart_download",
            "after": "manual_reset",
            "auto_reset": False,
            "components": components,
        },
        "monitor": {"baud": 115200},
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
