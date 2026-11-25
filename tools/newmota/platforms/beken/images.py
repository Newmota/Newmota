"""beken native layouts, image validation, and firmware packaging."""

from __future__ import annotations

import json
import re
import shutil
from pathlib import Path

from newmota.factory_package import write_factory_package
from newmota.host import SdkVersion, load_toml, sha256, source_state
from newmota.model import ROOT, Artifact, BuildContext, BuildError, Environment, PackageResult
from newmota.package import (
    PACKAGE_NAME,
    PACKAGE_SCHEMA,
    LayoutFacts,
    LayoutOverlap,
    LayoutRegion,
    PackageError,
    _artifact,
    load_package,
    package_manifest_path,
    write_layout_facts,
)
from newmota.platforms.beken.codec import (
    BekenImageError,
    load_image_config,
    ota_compatibility_tag,
    package_images,
    validate_image_layout,
)
from newmota.platforms.beken.net_params import NET_RESET_POLICY
from newmota.platforms.beken.profile import BekenTargetProfile, resolve_target_profile

PARTITION_NAMES = {
    "BK_PARTITION_BOOTLOADER": ("bootloader", "bootloader"),
    "BK_PARTITION_APPLICATION": ("application", "application"),
    "BK_PARTITION_OTA": ("ota", "ota-staging"),
    "BK_PARTITION_RF_FIRMWARE": ("rf", "vendor-metadata"),
    "BK_PARTITION_NET_PARAM": ("net", "vendor-metadata"),
    "BK_PARTITION_MATTER_FLASH": ("matter", "vendor-metadata"),
    "BK_PARTITION_MATTER_FACTORY_FLASH": ("matter-factory", "vendor-metadata"),
    "BK_PARTITION_USR_CONFIG": ("user-config", "vendor-metadata"),
    "BK_PARTITION_BLE_SVR_CONFIG": ("ble-server", "vendor-metadata"),
    "BK_PARTITION_BLE_BONDING_FLASH": ("ble-bonding", "vendor-metadata"),
}
ENTRY = re.compile(
    r"^[ \t]*\[\s*(BK_PARTITION_[A-Z_]+)\s*\]\s*=\s*\{(?P<body>.*?)\}",
    re.DOTALL | re.MULTILINE,
)
FIELD = re.compile(r"\.(partition_start_addr|partition_length)\s*=\s*(0[xX][0-9a-fA-F]+|[0-9]+)")
TABLE = re.compile(
    r"const\s+bk_logic_partition_t\s+\w+_partitions_(?P<size>[1248])M\s*"
    r"\[[^\]]+\]\s*=\s*\{",
    re.MULTILINE,
)


class BekenLayoutError(BuildError):
    """Raised when SDK discovery and a checked-in Beken layout descriptor disagree."""


def _integer(document: dict[str, object], key: str) -> int:
    value = document.get(key)
    if not isinstance(value, int) or isinstance(value, bool):
        raise BekenLayoutError(f"Beken layout field '{key}' must be an integer")
    return value


def _native_partition_source(profile: BekenTargetProfile, relative: str) -> Path:
    return profile.native_partition_source(relative)


def _flash_table_size(flash_bytes: int) -> str:
    sizes = {0x100000: "1", 0x200000: "2", 0x400000: "4", 0x800000: "8"}
    try:
        return sizes[flash_bytes]
    except KeyError as error:
        raise BekenLayoutError(
            f"Beken native partition tables do not define {flash_bytes} bytes"
        ) from error


def _table_body(content: str, flash_bytes: int) -> str:
    expected = _flash_table_size(flash_bytes)
    matches = [match for match in TABLE.finditer(content) if match.group("size") == expected]
    if len(matches) != 1:
        raise BekenLayoutError(
            f"Beken SDK partition source must contain exactly one {expected}M table; "
            f"found {len(matches)}"
        )
    opening = content.find("{", matches[0].start(), matches[0].end())
    depth = 0
    for index in range(opening, len(content)):
        character = content[index]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return content[opening + 1 : index]
    raise BekenLayoutError("Beken SDK partition table has an unterminated initializer")


def _condition(expression: str, sdk_target: str) -> bool:
    """Evaluate the small target-condition subset used in Beken tables."""

    normalized = re.sub(r"\s+", "", expression)
    if "CFG_SUPPORT_MATTER" in normalized:
        return False
    if "AT_SERVICE_CFG" in normalized:
        return False
    if "CFG_SOC_NAME==SOC_BK7221U" in normalized:
        # The BDK builds BK7252 as SOC_BK7221U.
        return sdk_target == "bk7252"
    return True


def _preprocess_table(body: str, sdk_target: str) -> str:
    """Resolve only the conditional forms present in the checked-in BDK table."""

    # Each frame stores its parent activity, whether one branch has already been
    # selected, and the activity of the current branch.
    frames: list[tuple[bool, bool, bool]] = []
    active = True
    output: list[str] = []
    directive = re.compile(r"^\s*#\s*(if|elif|else|endif)\b(?:\s+(.*?))?\s*$")
    for line in body.splitlines(keepends=True):
        match = directive.match(line)
        if match is None:
            if active:
                output.append(line)
            continue
        kind, expression = match.groups()
        if kind == "if":
            parent = active
            selected = _condition(expression or "", sdk_target)
            branch = parent and selected
            frames.append((parent, parent and selected, branch))
            active = branch
        elif kind == "elif":
            if not frames:
                raise BekenLayoutError("Beken partition table has an unmatched #elif")
            parent, selected, _ = frames.pop()
            branch = parent and not selected and _condition(expression or "", sdk_target)
            frames.append((parent, parent or branch if selected else branch, branch))
            active = branch
        elif kind == "else":
            if not frames:
                raise BekenLayoutError("Beken partition table has an unmatched #else")
            parent, selected, _ = frames.pop()
            branch = parent and not selected
            frames.append((parent, True, branch))
            active = branch
        else:
            if not frames:
                raise BekenLayoutError("Beken partition table has an unmatched #endif")
            parent, _, _ = frames.pop()
            active = parent
    if frames:
        raise BekenLayoutError("Beken partition table has an unterminated conditional")
    return "".join(output)


def _read_native_regions(
    source: Path, flash_bytes: int, sdk_target: str
) -> dict[str, LayoutRegion]:
    try:
        content = source.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise BekenLayoutError(
            f"cannot read Beken SDK partition source {source}: {error}"
        ) from error

    regions: dict[str, LayoutRegion] = {}
    body = _preprocess_table(_table_body(content, flash_bytes), sdk_target)
    for match in ENTRY.finditer(body):
        native_name = match.group(1)
        if native_name not in PARTITION_NAMES:
            continue
        fields = FIELD.findall(match.group("body"))
        if len(fields) != 2 or {name for name, _ in fields} != {
            "partition_start_addr",
            "partition_length",
        }:
            raise BekenLayoutError(f"Beken SDK partition {native_name} has incomplete bounds")
        values = {name: int(value, 0) for name, value in fields}
        name, role = PARTITION_NAMES[native_name]
        if name in regions:
            raise BekenLayoutError(f"Beken SDK partition {native_name} is duplicated")
        regions[name] = LayoutRegion(
            name,
            role,
            values["partition_start_addr"],
            values["partition_length"],
            values["partition_length"] if name in ("application", "ota") else None,
        )
    required = {"bootloader", "application", "ota", "rf", "net", "ble-bonding"}
    missing = sorted(required - set(regions))
    if missing:
        raise BekenLayoutError("Beken SDK partition source is missing: " + ", ".join(missing))
    return regions


NATIVE_ORDER = ("bootloader", "application", "ota", "rf", "net", "ble-bonding")
NATIVE_ROLES = {name: role for name, role in PARTITION_NAMES.values()}
# Factory flashing and updates write only these; every other region is preserved.
WRITTEN_REGIONS = ("application", "ota", "net", "filesystem")


def _declared_regions(
    table: object, field: str, roles: dict[str, str] | None
) -> dict[str, LayoutRegion]:
    """Read `name = [offset, size]` entries; roles=None marks preserved vendor data."""

    if not isinstance(table, dict) or not table:
        raise BekenLayoutError(f"Beken layout {field} must be a non-empty table")
    regions: dict[str, LayoutRegion] = {}
    for name, bounds in table.items():
        if (
            not isinstance(bounds, list)
            or len(bounds) != 2
            or not all(isinstance(value, int) and not isinstance(value, bool) for value in bounds)
        ):
            raise BekenLayoutError(f"Beken layout {field}.{name} must be [offset, size]")
        if roles is not None and name not in roles:
            raise BekenLayoutError(f"Beken layout {field} has unknown region '{name}'")
        role = "vendor-data" if roles is None else roles[name]
        regions[name] = LayoutRegion(
            name, role, bounds[0], bounds[1], bounds[1] if name in ("application", "ota") else None
        )
    return regions


def load_layout_document(path: Path) -> dict[str, object]:
    """Load one schema 2 Beken layout descriptor."""

    try:
        document = load_toml(path)
    except (OSError, UnicodeError, ValueError) as error:
        raise BekenLayoutError(f"cannot read Beken layout {path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema") != 2:
        raise BekenLayoutError("Beken layout schema must be 2")
    return document


def explicit_native_regions(document: dict[str, object]) -> dict[str, LayoutRegion] | None:
    """Return the descriptor's own native regions, or None when the BDK table owns them."""

    native = document.get("native")
    if not isinstance(native, dict) or "regions" not in native:
        return None
    regions = _declared_regions(native["regions"], "native.regions", NATIVE_ROLES)
    missing = sorted(set(NATIVE_ORDER) - set(regions))
    if missing or set(regions) - set(NATIVE_ORDER):
        raise BekenLayoutError(
            "Beken layout native.regions must name exactly: " + ", ".join(NATIVE_ORDER)
        )
    return regions


def resolve_layout(environment: Environment, sdk: SdkVersion, work_dir: Path) -> LayoutFacts:
    """Merge native regions, preserved vendor data and the filesystem placement."""

    profile = resolve_target_profile(environment, sdk)

    path = environment.partition
    if path is None:
        raise BekenLayoutError("Beken requires a flash-class layout descriptor")
    document = load_layout_document(path)

    flash_bytes = _integer(document, "flash_bytes")
    expected_flash = environment.flash_kb * 1024
    if flash_bytes != expected_flash:
        raise BekenLayoutError(
            f"Beken layout declares {flash_bytes} bytes, expected {expected_flash}"
        )
    erase_size = _integer(document, "erase_size")
    if erase_size != 0x1000:
        raise BekenLayoutError("Beken layouts currently require 4096-byte sectors")

    native = document.get("native")
    if not isinstance(native, dict):
        raise BekenLayoutError("Beken layout requires a [native] table")
    source_files = [path.relative_to(ROOT)]
    native_regions = explicit_native_regions(document)
    if native_regions is None:
        if not isinstance(native.get("partition_source"), str):
            raise BekenLayoutError(
                "Beken layout requires native.partition_source or native.regions"
            )
        native_source = _native_partition_source(profile, str(native["partition_source"]))
        # The firmware compiles one table (CFG_FLASH_SELECTION_TYPE); larger flash may reuse it.
        table = native.get("table")
        table_bytes = flash_bytes
        if table is not None:
            sizes = {"1M": 0x100000, "2M": 0x200000, "4M": 0x400000, "8M": 0x800000}
            if table not in sizes or sizes[str(table)] > flash_bytes:
                raise BekenLayoutError("Beken layout native.table must name a table within flash")
            table_bytes = sizes[str(table)]
        native_regions = _read_native_regions(native_source, table_bytes, environment.sdk_target)
        # Recorded at its checkout path in both SDK modes, so the layout identity is mode-independent.
        sdk_relative = native_source.relative_to(sdk.source.resolve())
        source_files.append(sdk.checkout.relative_to(ROOT) / sdk_relative)
    elif "partition_source" in native:
        raise BekenLayoutError("Beken layout names both native.partition_source and native.regions")
    vendor = document.get("vendor")
    vendor_regions = {} if vendor is None else _declared_regions(vendor, "vendor", None)
    if set(vendor_regions) & set(NATIVE_ORDER + ("filesystem",)):
        raise BekenLayoutError("Beken layout vendor regions must not reuse native names")

    filesystem = document.get("filesystem")
    if not isinstance(filesystem, dict) or "overlay" in filesystem:
        raise BekenLayoutError(
            "Beken filesystem needs its own offset and size; OTA overlays are no longer allowed"
        )
    filesystem_region = LayoutRegion(
        "filesystem", "littlefs", _integer(filesystem, "offset"), _integer(filesystem, "size")
    )
    if filesystem_region.size < 4 * erase_size:
        raise BekenLayoutError("Beken filesystem must span at least four sectors")

    ordered_native = tuple(native_regions[name] for name in NATIVE_ORDER)
    application = native_regions["application"]
    ota = native_regions["ota"]
    overlaps: list[LayoutOverlap] = []
    if application.offset < ota.end and ota.offset < application.end:
        overlaps.append(
            LayoutOverlap(
                "application",
                "ota",
                "native SDK application and OTA declarations overlap",
            )
        )
    regions = (*ordered_native, *vendor_regions.values(), filesystem_region)
    for index, first in enumerate(regions):
        for second in regions[index + 1 :]:
            native_pair = {first.name, second.name} == {"application", "ota"}
            if not native_pair and first.offset < second.end and second.offset < first.end:
                raise BekenLayoutError(
                    f"Beken layout regions '{first.name}' and '{second.name}' overlap"
                )
    try:
        return write_layout_facts(
            platform="beken",
            flash_bytes=flash_bytes,
            erase_size=erase_size,
            regions=regions,
            overlaps=tuple(overlaps),
            application_update_reach=("ota",),
            settings_filesystem_preserved=True,
            source_files=tuple(source_files),
            destination=work_dir / "config" / "layout_facts.json",
        )
    except ValueError as error:
        raise BekenLayoutError(str(error)) from error


def _beken_context_artifact(context: BuildContext, role: str, suffix: str | None = ".bin") -> Path:
    matches = [
        artifact.path
        for artifact in context.artifacts
        if artifact.role == role
        and artifact.path.is_file()
        and (suffix is None or artifact.path.suffix == suffix)
    ]
    if len(matches) != 1:
        raise PackageError(
            f"Beken build must produce exactly one {role} artifact; found {len(matches)}"
        )
    return matches[0]


def package_beken(context: BuildContext, layout: LayoutFacts) -> PackageResult:
    """Create an OpenBeken-style Beken package."""

    item = context.environment
    target_profile = resolve_target_profile(item, context.sdk)
    if item.role != "application":
        raise PackageError("Beken factory packaging requires an application environment")
    if context.sdk.programmer_status == "unselected":
        raise PackageError("Beken programmer is not selected in its SDK pin")
    if not PACKAGE_NAME.fullmatch(item.name) or not PACKAGE_NAME.fullmatch(context.version):
        raise PackageError("environment and version must be safe firmware filename components")

    application = layout.region("application")
    ota = layout.region("ota")
    filesystem = layout.region("filesystem")
    raw_application = _beken_context_artifact(context, "native")
    filesystem_image = _beken_context_artifact(context, "filesystem-init")
    application_limit = application.image_budget or application.size
    ota_limit = ota.image_budget or ota.size
    filesystem_size = filesystem_image.stat().st_size
    if filesystem_size != filesystem.size:
        raise PackageError(
            f"Beken filesystem image is {filesystem_size} bytes; expected {filesystem.size}"
        )

    staging = context.work_dir / "package-staging"
    if staging.exists():
        shutil.rmtree(staging)
    factory = staging / "factory"
    ota_directory = staging / "ota"
    diagnostic = staging / "diagnostic"
    factory.mkdir(parents=True)
    ota_directory.mkdir(parents=True)
    diagnostic.mkdir(parents=True)

    application_destination = factory / "application.rbl"
    ota_destination = ota_directory / "application.rbl"
    filesystem_destination = factory / "littlefs.bin"
    try:
        image_config = load_image_config(item)
        validate_image_layout(image_config, layout)
        package_images(
            raw_application,
            application_destination,
            ota_destination,
            image_config,
            layout,
            context.version,
        )
    except BekenImageError as error:
        raise PackageError(str(error)) from error
    shutil.copy2(filesystem_image, filesystem_destination)
    diagnostic_destination = diagnostic / "application.raw.bin"
    shutil.copy2(raw_application, diagnostic_destination)
    application_size = application_destination.stat().st_size
    ota_size = ota_destination.stat().st_size
    if application_size <= 0 or application_size > application_limit:
        raise PackageError(
            f"Beken factory application image is {application_size} bytes but its native "
            f"region allows {application_limit} bytes"
        )
    if ota_size <= 0 or ota_size > ota_limit:
        raise PackageError(
            f"Beken OTA image is {ota_size} bytes but stock OTA staging allows {ota_limit} bytes"
        )
    components = [
        {
            "name": "application.rbl",
            "partition": "application",
            "role": "factory",
            "path": str(application_destination.relative_to(staging)),
            "offset": application.offset,
        },
        {
            "name": "littlefs.bin",
            "partition": "filesystem",
            "role": "factory-component",
            "path": str(filesystem_destination.relative_to(staging)),
            "offset": filesystem.offset,
        },
    ]
    artifact_records = [
        _artifact(staging, application_destination, "factory", offset=application.offset),
        _artifact(
            staging,
            filesystem_destination,
            "factory-component",
            offset=filesystem.offset,
        ),
        _artifact(
            staging,
            ota_destination,
            "application-update",
            offset=ota.offset,
            size_limit=ota_limit,
        ),
        _artifact(staging, diagnostic_destination, "native-diagnostic"),
    ]
    layout_metadata = layout.metadata()
    layout_metadata.update(
        {
            "file": str(layout.manifest.relative_to(ROOT)),
            "sha256": sha256(layout.manifest),
        }
    )
    manifest: dict[str, object] = {
        "schema": PACKAGE_SCHEMA,
        "product": "newmota",
        "version": context.version,
        "source": source_state(),
        "target": {
            "environment": item.name,
            "board": item.board,
            "chip": item.chip,
            "platform": item.platform,
            "profile": item.profile,
            "language": item.language,
            "flash_bytes": item.flash_kb * 1024,
            "psram_bytes": item.psram_kb * 1024 if item.psram_kb is not None else None,
            "console": item.console,
            "sdk_target": item.sdk_target,
            "role": item.role,
            "safeboot": item.safeboot,
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
        "host_tools": {"bk7231tools": context.sdk.programmer_version},
        "configuration": {
            "environment": item.configuration_provenance(),
            "identity": context.native_config.identity,
            "features": context.features.identity,
            "hardware": context.hardware.identity,
            "layout": layout_metadata,
            "beken_target": target_profile.metadata(),
            "beken_image": image_config.metadata(),
        },
        "layout": layout_metadata,
        "qualification": {
            "factory": "beken-bk7231tools-rbl",
            "safeboot": "not-applicable",
            "ota": "beken-openbeken-rbl",
            "deployment": "unqualified",
        },
        "flashable": True,
        "deployable": False,
        "signing": {"status": "unsigned-development"},
        "ota": {
            "status": "beken-openbeken-rbl",
            "artifacts": [
                {
                    "role": "application-update",
                    "format": "rbl",
                    "compatibility_tag": ota_compatibility_tag(image_config, layout),
                    "path": str(ota_destination.relative_to(staging)),
                    "offset": ota.offset,
                    "size": ota_size,
                    "size_limit": ota_limit,
                }
            ],
            "erases": [],
            "preserves": [
                region.name
                for region in layout.regions
                if region.name not in ("application", "ota")
            ],
        },
        "flash": {
            "protocol": "bk7231tools-python",
            "protocol_version": context.sdk.programmer_version,
            "before": "hardware-reset-or-manual",
            "after": "chip-reboot",
            "preserves": [
                region.name for region in layout.regions if region.name not in WRITTEN_REGIONS
            ],
            "net_initialization": {
                "policy": NET_RESET_POLICY,
                "partition": "net",
                "offset": layout.region("net").offset,
                "size": layout.region("net").size,
            },
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
