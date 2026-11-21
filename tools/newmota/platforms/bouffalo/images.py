"""bouffalo native layouts, image validation, and firmware packaging."""

from __future__ import annotations

import json
import shutil
from collections.abc import Mapping
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
    _manifest_native_artifacts,
    create_filesystem_image,
    image_metadata,
    load_package,
    package_manifest_path,
    write_layout_facts,
)

# Native boot2 partition-table locations (BFLB_PT_TABLE0/1_ADDRESS in the
# pinned SDK).  These are fixed by boot2, not by the TOML file.
PT_TABLE0_ADDRESS = 0xE000
PT_TABLE1_ADDRESS = 0xF000
PT_TABLE_SIZE = 0x1000

ERASE_SIZE = 0x1000

# Native `FW` entry identity.  Slot 0 is the normal application, slot 1 is
# the Safeboot recovery image (see the TOML header for the rationale).
FW_ENTRY_NAME = "FW"
FW_ENTRY_TYPE = 0

# Every entry the Newmota layout must contain, matched case-sensitively
# against the native TOML names.  The BL602 Compact map deliberately requires
# only the regions consumed by Newmota.  The native vendor regions are kept as
# an optional compatibility set so a separately qualified board can retain one
# without making unused storage part of the common layout contract.  Unknown
# entries are rejected so SDK drift fails closed instead of silently changing
# the flash map.
REQUIRED_ENTRIES = ("Boot2", "FW", "lfs")
OPTIONAL_VENDOR_ENTRIES = ("mfg", "media", "PSM", "KEY", "DATA", "factory")
ALLOWED_ENTRIES = set(REQUIRED_ENTRIES) | set(OPTIONAL_VENDOR_ENTRIES)

# Minimum dedicated LittleFS size: 16 blocks of 4 KiB.
LITTLEFS_MIN_SIZE = 0x10000


class BouffaloLayoutError(ValueError):
    """Raised when a Bouffalo partition layout is incomplete or unsafe."""


def _integer(value: object, field: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise BouffaloLayoutError(f"Bouffalo layout field '{field}' must be an integer")
    return value


def _entries(document: dict[str, Any], path: Path) -> dict[str, dict[str, Any]]:
    try:
        relative = path.relative_to(ROOT)
    except ValueError:
        relative = path
    raw_entries = document.get("pt_entry")
    if not isinstance(raw_entries, list) or not all(isinstance(item, dict) for item in raw_entries):
        raise BouffaloLayoutError(f"Bouffalo layout {relative} has no pt_entry list")
    entries: dict[str, dict[str, Any]] = {}
    for item in raw_entries:
        entry = dict(item)
        name = entry.get("name")
        if not isinstance(name, str) or not name:
            raise BouffaloLayoutError(f"Bouffalo layout {relative} has a nameless pt_entry")
        if name in entries:
            raise BouffaloLayoutError(
                f"Bouffalo layout {relative} declares '{name}' more than once"
            )
        entries[name] = entry
    missing = [name for name in REQUIRED_ENTRIES if name not in entries]
    if missing:
        raise BouffaloLayoutError(
            f"Bouffalo layout {relative} is missing required entries: " + ", ".join(missing)
        )
    unknown = sorted(set(entries) - ALLOWED_ENTRIES)
    if unknown:
        raise BouffaloLayoutError(
            f"Bouffalo layout {relative} has unreviewed entries: " + ", ".join(unknown)
        )
    return entries


def expected_partition_entries(environment: Environment) -> dict[str, dict[str, int]]:
    """Return the immutable on-device fields selected by the checked-in TOML.

    Runtime fields such as active slot, image length, and age are deliberately
    excluded.  Wired application updates use this identity to reject a valid
    but foreign Bouffalo partition table before writing.
    """

    path = environment.partition
    if path is None:
        raise BouffaloLayoutError("Bouffalo requires a TOML partition default")
    try:
        document = load_toml(path)
    except (OSError, UnicodeError, ValueError) as error:
        raise BouffaloLayoutError(f"cannot read Bouffalo layout {path}: {error}") from error
    if not isinstance(document, dict):
        raise BouffaloLayoutError(f"Bouffalo layout {path} must be a TOML table")
    entries = _entries(document, path)
    fields = ("type", "device", "address0", "address1", "size0", "size1")
    return {
        name: {field: _integer(entry.get(field), f"{name}.{field}") for field in fields}
        for name, entry in entries.items()
    }


def _single_slot(
    entries: dict[str, dict[str, Any]], name: str, flash_bytes: int, path: Path
) -> LayoutRegion:
    entry = entries[name]
    address1 = _integer(entry.get("address1"), f"{name}.address1")
    size1 = _integer(entry.get("size1"), f"{name}.size1")
    if address1 != 0 or size1 != 0:
        raise BouffaloLayoutError(
            f"Bouffalo entry '{name}' must be a single-slot region in {path.name}"
        )
    offset = _integer(entry.get("address0"), f"{name}.address0")
    size = _integer(entry.get("size0"), f"{name}.size0")
    if offset <= 0 or size <= 0:
        raise BouffaloLayoutError(f"Bouffalo entry '{name}' has invalid bounds")
    role = "littlefs" if name == "lfs" else "vendor"
    region_name = "littlefs" if name == "lfs" else name.lower()
    return LayoutRegion(region_name, role, offset, size)


def resolve_layout(environment: Environment, sdk: SdkVersion, work_dir: Path) -> LayoutFacts:
    """Parse the native TOML table into validated, normalized layout facts."""

    del sdk
    path = environment.partition
    if path is None:
        raise BouffaloLayoutError("Bouffalo requires a TOML partition default")
    try:
        relative_source = path.relative_to(ROOT)
    except ValueError:
        relative_source = path
    try:
        document = load_toml(path)
    except (OSError, UnicodeError, ValueError) as error:
        raise BouffaloLayoutError(f"cannot read Bouffalo layout {path}: {error}") from error
    if not isinstance(document, dict):
        raise BouffaloLayoutError(f"Bouffalo layout {relative_source} must be a TOML table")

    flash_bytes = environment.flash_kb * 1024
    if flash_bytes <= 0:
        raise BouffaloLayoutError("Bouffalo environment has no declared flash capacity")

    raw_table = document.get("pt_table")
    if not isinstance(raw_table, dict):
        raise BouffaloLayoutError(f"Bouffalo layout {relative_source} has no [pt_table]")
    if _integer(raw_table.get("address0"), "pt_table.address0") != PT_TABLE0_ADDRESS:
        raise BouffaloLayoutError("Bouffalo pt_table.address0 must be 0xE000 for boot2")
    if _integer(raw_table.get("address1"), "pt_table.address1") != PT_TABLE1_ADDRESS:
        raise BouffaloLayoutError("Bouffalo pt_table.address1 must be 0xF000 for boot2")

    entries = _entries(document, path)

    boot2 = entries["Boot2"]
    if _integer(boot2.get("type"), "Boot2.type") != 16:
        raise BouffaloLayoutError("Bouffalo Boot2 entry must keep native type 16")
    if _integer(boot2.get("device"), "Boot2.device") != 0:
        raise BouffaloLayoutError("Bouffalo Boot2 entry must target flash device 0")
    if (
        _integer(boot2.get("address0"), "Boot2.address0") != 0
        or _integer(boot2.get("size0"), "Boot2.size0") != PT_TABLE0_ADDRESS
        or _integer(boot2.get("address1"), "Boot2.address1") != 0
        or _integer(boot2.get("size1"), "Boot2.size1") != 0
    ):
        raise BouffaloLayoutError("Bouffalo Boot2 entry must reserve flash from 0 to 0xE000")

    firmware = entries[FW_ENTRY_NAME]
    if _integer(firmware.get("type"), "FW.type") != FW_ENTRY_TYPE:
        raise BouffaloLayoutError("Bouffalo FW entry must keep native type 0")
    if _integer(firmware.get("device"), "FW.device") != 0:
        raise BouffaloLayoutError("Bouffalo FW entry must target flash device 0")
    app_offset = _integer(firmware.get("address0"), "FW.address0")
    app_size = _integer(firmware.get("size0"), "FW.size0")
    safeboot_offset = _integer(firmware.get("address1"), "FW.address1")
    safeboot_size = _integer(firmware.get("size1"), "FW.size1")
    if app_offset != 0x10000:
        raise BouffaloLayoutError(
            "Bouffalo FW slot 0 must start at 0x10000 (boot2 default image base)"
        )
    for label, offset, size in (
        ("FW slot 0", app_offset, app_size),
        ("FW slot 1", safeboot_offset, safeboot_size),
    ):
        if offset <= 0 or size <= 0:
            raise BouffaloLayoutError(f"Bouffalo {label} has invalid bounds")
    if safeboot_offset < app_offset + app_size:
        raise BouffaloLayoutError("Bouffalo FW slots overlap or are unordered")

    regions = [
        LayoutRegion("pt-table0", "pt-table", PT_TABLE0_ADDRESS, PT_TABLE_SIZE),
        LayoutRegion("pt-table1", "pt-table", PT_TABLE1_ADDRESS, PT_TABLE_SIZE),
        LayoutRegion("application", "application", app_offset, app_size, app_size),
        LayoutRegion("safeboot", "safeboot", safeboot_offset, safeboot_size, safeboot_size),
    ]
    # `lfs` is Newmota-required.  The remaining names are optional native
    # vendor regions; when present they are validated and retained in the
    # normalized facts, but they are not required by the Compact profile.
    regions.append(_single_slot(entries, "lfs", flash_bytes, path))
    for name in OPTIONAL_VENDOR_ENTRIES:
        if name in entries:
            regions.append(_single_slot(entries, name, flash_bytes, path))

    filesystem = next(region for region in regions if region.name == "littlefs")
    if filesystem.size < LITTLEFS_MIN_SIZE:
        raise BouffaloLayoutError(
            f"Bouffalo LittleFS region is {filesystem.size:#x} bytes, "
            f"minimum is {LITTLEFS_MIN_SIZE:#x}"
        )

    try:
        return write_layout_facts(
            platform="bouffalo",
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
        raise BouffaloLayoutError(str(error)) from error


def _bouffalo_layout(context: BuildContext) -> LayoutFacts:
    if context.layout is None or context.layout.platform != "bouffalo":
        raise PackageError("Bouffalo packaging requires its resolved layout facts")
    return context.layout


def _bouffalo_native(context: BuildContext) -> dict[str, Path]:
    return {
        artifact.path.name: artifact.path
        for artifact in context.artifacts
        if artifact.role == "native" and artifact.path.is_file()
    }


def _check_bouffalo_image(path: Path, name: str, offset: int, limit: int) -> None:
    if not path.is_file():
        raise PackageError(f"Bouffalo image is missing: {path}")
    size = path.stat().st_size
    if size == 0 or size > limit:
        raise PackageError(f"Bouffalo {name} is {size} bytes but its slot allows {limit} bytes")
    if offset % 0x1000:
        raise PackageError(f"Bouffalo {name} must have a 4 KiB-aligned offset")


def _bouffalo_build_image(native: Mapping[str, Path], owner: str) -> Path:
    matches = sorted(
        path
        for name, path in native.items()
        if name.startswith("newmota_") and name.endswith(".bin")
    )
    if len(matches) != 1:
        raise PackageError(
            f"Bouffalo {owner} build must produce exactly one newmota image; found {len(matches)}"
        )
    return matches[0]


def _bouffalo_build_file(native: Mapping[str, Path], pattern: str, owner: str) -> Path:
    matches = sorted(
        path for name, path in native.items() if name.startswith(pattern) and name.endswith(".bin")
    )
    if len(matches) != 1:
        raise PackageError(
            f"Bouffalo {owner} build must produce exactly one {pattern} file; found {len(matches)}"
        )
    return matches[0]


def _verified_bouffalo_safeboot_dependency(
    context: BuildContext, layout: LayoutFacts
) -> dict[str, Path]:
    item = context.environment
    if item.safeboot is None:
        raise PackageError(f"Bouffalo application {item.name} has no safeboot environment")
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
    return {
        "image": _bouffalo_build_image(native, "Safeboot"),
        "partition": _bouffalo_build_file(native, "partition", "Safeboot"),
    }


def validate_bouffalo_safeboot(context: BuildContext) -> PackageResult:
    """Validate a Safeboot image against the resolved Bouffalo safeboot slot."""

    item = context.environment
    if item.role != "safeboot" or item.profile != "Safeboot":
        raise PackageError("Bouffalo safeboot validation requires the Safeboot role and profile")
    layout = _bouffalo_layout(context)
    native = _bouffalo_native(context)
    safeboot = layout.region("safeboot")
    image = _bouffalo_build_image(native, "Safeboot")
    _check_bouffalo_image(image, "Safeboot image", safeboot.offset, safeboot.size)
    print(
        f"Safeboot image: {image.relative_to(ROOT)} ({image.stat().st_size}/{safeboot.size} bytes)"
    )
    return PackageResult(status="safeboot", flashable=False, deployable=False)


def package_bouffalo(context: BuildContext) -> PackageResult:
    """Create a paired Bouffalo UART factory package."""

    item = context.environment
    if item.role != "application":
        raise PackageError("Bouffalo factory packaging requires an application environment")
    if not PACKAGE_NAME.fullmatch(item.name) or not PACKAGE_NAME.fullmatch(context.version):
        raise PackageError("environment and version must be safe firmware filename components")

    layout = _bouffalo_layout(context)
    native = _bouffalo_native(context)
    application = layout.region("application")
    safeboot = layout.region("safeboot")
    filesystem = layout.region("littlefs")
    pt_table0 = layout.region("pt-table0")
    pt_table1 = layout.region("pt-table1")

    image = _bouffalo_build_image(native, "application")
    _check_bouffalo_image(image, "application image", application.offset, application.size)
    boot2 = _bouffalo_build_file(native, "boot2_", "application")
    partition = _bouffalo_build_file(native, "partition", "application")
    safeboot_files = _verified_bouffalo_safeboot_dependency(context, layout)
    safeboot_image = safeboot_files["image"]
    _check_bouffalo_image(safeboot_image, "Safeboot image", safeboot.offset, safeboot.size)
    if sha256(partition) != sha256(safeboot_files["partition"]):
        raise PackageError("Bouffalo application and Safeboot partition tables differ")

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

    add_component("boot2.bin", boot2, 0x0000)
    add_component("partition-table0.bin", partition, pt_table0.offset)
    add_component("partition-table1.bin", partition, pt_table1.offset)
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
            native_format="bouffalo-boot2-image",
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
            "blflashcommand": context.sdk.programmer_version,
            "protocol": "bouffalo-uart",
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
            "factory": "bouffalo-uart",
            "safeboot": "bouffalo-paired",
            "ota": "bouffalo-wired-application",
            "deployment": "unqualified",
        },
        "flashable": True,
        "deployable": False,
        "signing": {"status": "unsigned-development"},
        "ota": {
            "status": "bouffalo-wired-application",
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
                    "native_format": "bouffalo-boot2-image",
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
            "protocol": "bouffalo-uart",
            "protocol_version": "blflashcommand",
            "chipname": item.sdk_target,
            "baud": 500000,
            "before": "eflash-loader-handshake",
            "after": "reset",
            "auto_reset": True,
            "components": components,
        },
        "monitor": {"baud": 2000000},
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
