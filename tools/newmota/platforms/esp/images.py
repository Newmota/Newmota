"""esp native layouts, image validation, and firmware packaging."""

from __future__ import annotations

import binascii
import csv
import hashlib
import json
import re
import shutil
import struct
from collections.abc import Mapping
from pathlib import Path

from newmota.factory_package import write_factory_package
from newmota.host import run, sha256, source_state
from newmota.model import (
    ROOT,
    WORK_DIR,
    Artifact,
    BuildContext,
    PackageResult,
)
from newmota.package import (
    PACKAGE_NAME,
    PACKAGE_SCHEMA,
    PackageError,
    _artifact,
    create_filesystem_image,
    image_metadata,
    load_package,
    package_manifest_path,
)

ESP_IMAGE_HEADER_SIZE = 24
ESP_IMAGE_SEGMENT_HEADER_SIZE = 8
ESP_APP_DESCRIPTION_SIZE = 256
ESP_CUSTOM_DESCRIPTOR_OFFSET = (
    ESP_IMAGE_HEADER_SIZE + ESP_IMAGE_SEGMENT_HEADER_SIZE + ESP_APP_DESCRIPTION_SIZE
)
ESP_CUSTOM_DESCRIPTOR = struct.Struct("<8sHHB3xIIII32s16s65sI")
ESP_CUSTOM_DESCRIPTOR_MAGIC = b"NEWMOTA\0"
ESP_CUSTOM_DESCRIPTOR_SCHEMA = 1
ESP_CUSTOM_DESCRIPTOR_APPLICATION = 1


def _number(value: str) -> int:
    text = value.strip().lower()
    multiplier = 1
    if text.endswith("k"):
        multiplier, text = 1024, text[:-1]
    elif text.endswith("m"):
        multiplier, text = 1024 * 1024, text[:-1]
    return int(text, 0) * multiplier


def _esp_partition_metadata(path: Path, flash_bytes: int) -> dict[str, object]:
    partitions: list[dict[str, object]] = []
    with path.open(newline="", encoding="utf-8") as source:
        for row in csv.reader(line for line in source if not line.lstrip().startswith("#")):
            if not row or all(not field.strip() for field in row):
                continue
            if len(row) < 5:
                raise PackageError(f"invalid ESP-IDF partition row in {path.relative_to(ROOT)}")
            name, kind, subtype, offset_text, size_text = (field.strip() for field in row[:5])
            if not offset_text or not size_text:
                raise PackageError(
                    f"ESP-IDF package requires explicit partition offsets and sizes: {name}"
                )
            try:
                offset, size = _number(offset_text), _number(size_text)
            except ValueError as error:
                raise PackageError(f"invalid ESP-IDF partition number for {name}") from error
            if size <= 0 or offset < 0 or offset + size > flash_bytes:
                raise PackageError(f"ESP-IDF partition {name} exceeds declared flash capacity")
            if partitions and offset < int(partitions[-1]["end"]):
                raise PackageError(f"ESP-IDF partition {name} overlaps its predecessor")
            partitions.append(
                {
                    "name": name,
                    "type": kind,
                    "subtype": subtype,
                    "offset": offset,
                    "size": size,
                    "end": offset + size,
                }
            )
    if not partitions:
        raise PackageError("ESP-IDF partition CSV contains no partitions")
    final = partitions[-1]
    if final["type"] != "data" or final["subtype"] not in {"spiffs", "littlefs"}:
        raise PackageError("ESP-IDF final partition is not an expandable filesystem")
    return {
        "source_sha256": sha256(path),
        "partitions": partitions,
        "expandable_final": {
            "name": final["name"],
            "type": final["type"],
            "subtype": final["subtype"],
            "offset": final["offset"],
            "packaged_size": final["size"],
        },
    }


def _esp_partition(
    metadata: Mapping[str, object], name: str, kind: str, subtype: str
) -> dict[str, object]:
    partitions = metadata.get("partitions")
    if not isinstance(partitions, list):
        raise PackageError("ESP-IDF partition metadata is incomplete")
    matches = [
        item
        for item in partitions
        if isinstance(item, dict)
        and item.get("name") == name
        and item.get("type") == kind
        and item.get("subtype") == subtype
    ]
    if len(matches) != 1:
        raise PackageError(f"ESP-IDF layout requires exactly one {name} {kind}/{subtype} partition")
    return matches[0]


def esp_image_compatibility(
    metadata: Mapping[str, object],
    environment: str,
    sdk_target: str,
    psram_bytes: int,
) -> dict[str, object]:
    """Describe the ESP layout requirements embedded in a native application image.

    The stable compatibility identity covers the partition roles and offsets used by
    Newmota's boot flow. Partition sizes and the trailing filesystem are intentionally
    excluded: the OTA writer separately checks the incoming image against the actual app
    slot, and a larger compatible filesystem must not change the identity.
    """

    partitions = metadata.get("partitions")
    if not isinstance(partitions, list):
        raise PackageError("ESP-IDF compatibility metadata has no partitions")
    names = ("nvs", "otadata", "safeboot", "app0")
    required: list[dict[str, object]] = []
    for name in names:
        matches = [
            entry for entry in partitions if isinstance(entry, dict) and entry.get("name") == name
        ]
        if len(matches) != 1:
            raise PackageError(
                f"ESP-IDF compatibility layout requires exactly one {name} partition"
            )
        entry = matches[0]
        required.append(
            {
                "name": name,
                "type": entry.get("type"),
                "subtype": entry.get("subtype"),
                "offset": entry.get("offset"),
            }
        )
    expected = {
        "nvs": ("data", "nvs"),
        "otadata": ("data", "ota"),
        "safeboot": ("app", "factory"),
        "app0": ("app", "ota_0"),
    }
    for entry in required:
        name = str(entry["name"])
        if (entry["type"], entry["subtype"]) != expected[name]:
            raise PackageError(f"ESP-IDF compatibility partition {name} has the wrong native role")
        if not isinstance(entry["offset"], int) or entry["offset"] < 0:
            raise PackageError(f"ESP-IDF compatibility partition {name} has an invalid offset")
    if len(environment.encode("ascii")) >= 32:
        raise PackageError("ESP-IDF environment name does not fit the native image descriptor")
    if len(sdk_target.encode("ascii")) >= 16:
        raise PackageError("ESP-IDF SDK target does not fit the native image descriptor")
    if psram_bytes < 0:
        raise PackageError("ESP-IDF PSRAM requirement must be non-negative")
    canonical = json.dumps(
        {"schema": ESP_CUSTOM_DESCRIPTOR_SCHEMA, "partitions": required},
        sort_keys=True,
        separators=(",", ":"),
    ).encode("ascii")
    app0 = next(entry for entry in required if entry["name"] == "app0")
    safeboot = next(entry for entry in required if entry["name"] == "safeboot")
    otadata = next(entry for entry in required if entry["name"] == "otadata")
    nvs = next(entry for entry in required if entry["name"] == "nvs")
    return {
        "environment": environment,
        "sdk_target": sdk_target,
        "layout_id": hashlib.sha256(canonical).hexdigest(),
        "app0_offset": app0["offset"],
        "safeboot_offset": safeboot["offset"],
        "otadata_offset": otadata["offset"],
        "nvs_offset": nvs["offset"],
        "psram_bytes": psram_bytes,
        "required_partitions": required,
    }


def esp_partition_compatibility(
    path: Path,
    flash_bytes: int,
    environment: str,
    sdk_target: str,
    psram_bytes: int,
) -> dict[str, object]:
    """Resolve Newmota's image compatibility facts from one selected ESP CSV."""

    return esp_image_compatibility(
        _esp_partition_metadata(path, flash_bytes), environment, sdk_target, psram_bytes
    )


def validate_esp_custom_descriptor(image: Path, compatibility: Mapping[str, object]) -> None:
    """Check Newmota metadata at ESP-IDF's documented fixed custom-descriptor offset."""

    with image.open("rb") as source:
        source.seek(ESP_CUSTOM_DESCRIPTOR_OFFSET)
        payload = source.read(ESP_CUSTOM_DESCRIPTOR.size)
    if len(payload) != ESP_CUSTOM_DESCRIPTOR.size:
        raise PackageError("ESP-IDF application image is too short for its Newmota descriptor")
    (
        magic,
        schema,
        descriptor_size,
        role,
        app0_offset,
        safeboot_offset,
        otadata_offset,
        nvs_offset,
        environment,
        sdk_target,
        layout_id,
        psram_bytes,
    ) = ESP_CUSTOM_DESCRIPTOR.unpack(payload)
    actual = {
        "magic": magic,
        "schema": schema,
        "descriptor_size": descriptor_size,
        "role": role,
        "app0_offset": app0_offset,
        "safeboot_offset": safeboot_offset,
        "otadata_offset": otadata_offset,
        "nvs_offset": nvs_offset,
        "environment": environment.split(b"\0", 1)[0].decode("ascii", errors="replace"),
        "sdk_target": sdk_target.split(b"\0", 1)[0].decode("ascii", errors="replace"),
        "layout_id": layout_id.split(b"\0", 1)[0].decode("ascii", errors="replace"),
        "psram_bytes": psram_bytes,
    }
    expected = {
        "magic": ESP_CUSTOM_DESCRIPTOR_MAGIC,
        "schema": ESP_CUSTOM_DESCRIPTOR_SCHEMA,
        "descriptor_size": ESP_CUSTOM_DESCRIPTOR.size,
        "role": ESP_CUSTOM_DESCRIPTOR_APPLICATION,
        "app0_offset": compatibility.get("app0_offset"),
        "safeboot_offset": compatibility.get("safeboot_offset"),
        "otadata_offset": compatibility.get("otadata_offset"),
        "nvs_offset": compatibility.get("nvs_offset"),
        "environment": compatibility.get("environment"),
        "sdk_target": compatibility.get("sdk_target"),
        "layout_id": compatibility.get("layout_id"),
        "psram_bytes": compatibility.get("psram_bytes"),
    }
    if actual != expected:
        raise PackageError(
            "ESP-IDF application has missing or mismatched Newmota compatibility metadata"
        )


def validate_esp_image_info(output: str, sdk_target: str) -> None:
    """Require the pinned esptool parser to accept and verify a native ESP image."""

    suffix = sdk_target.removeprefix("esp32").upper()
    expected_chip = "ESP32" if not suffix else f"ESP32-{suffix}"
    detected = re.search(r"^Detected image type: ([A-Z0-9-]+)$", output, re.MULTILINE)
    if detected is None or detected.group(1) != expected_chip:
        actual = detected.group(1) if detected is not None else "missing"
        raise PackageError(
            f"ESP-IDF application native chip mismatch: expected {expected_chip}, got {actual}"
        )
    if re.search(r"^Checksum:\s+\S+\s+\(valid\)$", output, re.MULTILINE) is None:
        raise PackageError("ESP-IDF application native checksum is missing or invalid")
    digest = re.search(
        r"^Validation Hash:\s+.+\((valid|invalid)\)$",
        output,
        re.MULTILINE | re.IGNORECASE,
    )
    if digest is None or digest.group(1).lower() != "valid":
        raise PackageError("ESP-IDF application native SHA-256 digest is missing or invalid")
    segment_counts = re.findall(r"^(?:Segments:\s*(\d+)|(\d+) segments)$", output, re.MULTILINE)
    if not segment_counts or max(int(first or second) for first, second in segment_counts) <= 0:
        raise PackageError("ESP-IDF application native image has no segments")


def _esptool_cli_option(value: str, sdk_key: str = "esp-idf") -> str:
    """Use the CLI option spelling supported by the selected programmer."""

    if not value.startswith("--"):
        return value
    option = value[2:]
    return "--" + (
        option.replace("_", "-") if sdk_key == "esp-idf-v6.1" else option.replace("-", "_")
    )


def _esptool_reset_mode(value: str, sdk_key: str = "esp-idf") -> str:
    """Use the command/reset spelling supported by the selected programmer."""

    return value.replace("_", "-") if sdk_key == "esp-idf-v6.1" else value.replace("-", "_")


def esp_application_partition(path: Path, flash_bytes: int) -> dict[str, object]:
    """Return the fixed normal-application partition from an ESP-IDF layout."""

    metadata = _esp_partition_metadata(path, flash_bytes)
    return _esp_partition(metadata, "app0", "app", "ota_0")


def _check_partition_fit(image: Path, partition: Mapping[str, object], role: str) -> None:
    size = partition.get("size")
    if not isinstance(size, int) or size <= 0:
        raise PackageError(f"ESP-IDF {role} partition has an invalid size")
    image_size = image.stat().st_size
    if image_size > size:
        raise PackageError(
            f"ESP-IDF {role} image is too large: {image_size} bytes exceeds "
            f"the selected partition's {size} bytes"
        )


def _write_initial_otadata(path: Path, partition_size: int) -> None:
    """Write ESP-IDF OTA selection data which boots ota_0 instead of factory."""

    if partition_size != 0x2000:
        raise PackageError(
            f"ESP-IDF initial OTA selection expects an 0x2000-byte otadata partition, "
            f"got {partition_size:#x}"
        )
    ota_sequence = struct.pack("<I", 1)
    crc = binascii.crc32(ota_sequence, 0xFFFFFFFF) & 0xFFFFFFFF
    contents = bytearray(b"\xff" * partition_size)
    contents[0:4] = ota_sequence
    contents[28:32] = struct.pack("<I", crc)
    path.write_bytes(contents)


def validate_esp_idf_safeboot(context: BuildContext) -> PackageResult:
    """Validate a safeboot build against the selected layout without publishing a factory package."""

    item = context.environment
    if item.role != "safeboot" or item.profile != "Safeboot":
        raise PackageError("ESP-IDF safeboot validation requires the Safeboot role and profile")
    if item.partition is None:
        raise PackageError("ESP-IDF safeboot build has no resolved partition layout")
    image = context.work_dir / "newmota.bin"
    metadata = _esp_partition_metadata(item.partition, item.flash_kb * 1024)
    partition = _esp_partition(metadata, "safeboot", "app", "factory")
    _check_partition_fit(image, partition, "safeboot")
    print(
        f"Safeboot image: {image.relative_to(ROOT)} "
        f"({image.stat().st_size}/{partition['size']} bytes)"
    )
    return PackageResult(
        status="safeboot",
        flashable=False,
        deployable=False,
    )


def _verified_safeboot_dependency(context: BuildContext) -> Path:
    item = context.environment
    if item.safeboot is None:
        raise PackageError(f"ESP-IDF application {item.name} has no safeboot environment")
    manifest_path = WORK_DIR / item.safeboot / "artifacts.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PackageError(
            f"cannot read safeboot build manifest {manifest_path.relative_to(ROOT)}: {error}"
        ) from error
    target = manifest.get("target")
    sdk = manifest.get("sdk")
    pipeline = manifest.get("pipeline")
    if not isinstance(target, dict) or not isinstance(sdk, dict) or not isinstance(pipeline, dict):
        raise PackageError("safeboot build manifest is incomplete")
    if (
        manifest.get("version") != context.version
        or target.get("environment") != item.safeboot
        or target.get("role") != "safeboot"
        or target.get("profile") != "Safeboot"
        or target.get("sdk_target") != item.sdk_target
        or sdk.get("revision") != context.sdk.revision
        or pipeline.get("package_status") != "safeboot"
        or manifest.get("flashable") is not False
        or manifest.get("source") != source_state()
    ):
        raise PackageError(
            f"safeboot build {item.safeboot} does not match the application build identity"
        )
    image = WORK_DIR / item.safeboot / "newmota.bin"
    if not image.is_file():
        raise PackageError(f"safeboot image is missing: {image.relative_to(ROOT)}")
    artifacts = manifest.get("artifacts")
    expected = (
        next(
            (
                artifact
                for artifact in artifacts
                if isinstance(artifact, dict)
                and artifact.get("role") == "native"
                and artifact.get("path") == str(image.relative_to(ROOT))
            ),
            None,
        )
        if isinstance(artifacts, list)
        else None
    )
    if (
        not isinstance(expected, dict)
        or expected.get("size") != image.stat().st_size
        or expected.get("sha256") != sha256(image)
    ):
        raise PackageError("safeboot image does not match its build manifest")
    return image


def package_esp_idf(
    context: BuildContext,
    python: Path,
    flasher: dict[str, object],
    environment: Mapping[str, str],
) -> PackageResult:
    """Create the self-contained ESP-IDF factory package from native outputs."""

    item = context.environment
    if item.role != "application":
        raise PackageError("ESP-IDF factory packaging requires an application environment")
    if not PACKAGE_NAME.fullmatch(item.name) or not PACKAGE_NAME.fullmatch(context.version):
        raise PackageError("environment and version must be safe firmware filename components")
    flash_files = flasher.get("flash_files")
    write_args = flasher.get("write_flash_args")
    extra_args = flasher.get("extra_esptool_args")
    if not isinstance(flash_files, dict) or not flash_files:
        raise PackageError("ESP-IDF flasher_args.json has no flash_files")
    if not isinstance(write_args, list) or not all(isinstance(value, str) for value in write_args):
        raise PackageError("ESP-IDF flasher_args.json has invalid write_flash_args")
    write_args = [_esptool_cli_option(value, context.sdk.key) for value in write_args]
    if not isinstance(extra_args, dict):
        raise PackageError("ESP-IDF flasher_args.json has invalid extra_esptool_args")
    chip = extra_args.get("chip")
    if chip != item.sdk_target:
        raise PackageError(f"ESP-IDF flasher target {chip!r} does not match {item.sdk_target}")
    if extra_args.get("stub") is not True:
        raise PackageError("ESP-IDF factory packaging has not qualified no-stub builds")
    if item.partition is None:
        raise PackageError("ESP-IDF package has no resolved partition layout")

    post_flash_reset = _esptool_reset_mode(
        str(extra_args.get("after", "hard_reset")), context.sdk.key
    )
    if item.sdk_target == "esp32s3" and item.console == "usb-serial-jtag":
        post_flash_reset = _esptool_reset_mode("watchdog-reset", context.sdk.key)

    stem = f"newmota-{item.name}-{context.version}"
    staging = context.work_dir / "package-staging"
    if staging.exists():
        shutil.rmtree(staging)
    factory_dir = staging / "factory"
    ota_dir = staging / "ota"
    safeboot_dir = staging / "safeboot"
    debug_dir = staging / "debug"
    factory_dir.mkdir(parents=True)
    ota_dir.mkdir()
    safeboot_dir.mkdir()
    debug_dir.mkdir()

    layout = factory_dir / f"{stem}-partition.csv"
    shutil.copy2(item.partition, layout)
    partition_metadata = _esp_partition_metadata(layout, item.flash_kb * 1024)
    safeboot_partition = _esp_partition(partition_metadata, "safeboot", "app", "factory")
    application_partition = _esp_partition(partition_metadata, "app0", "app", "ota_0")
    otadata_partition = _esp_partition(partition_metadata, "otadata", "data", "ota")
    compatibility = esp_image_compatibility(
        partition_metadata,
        item.name,
        item.sdk_target,
        (item.psram_kb or 0) * 1024,
    )
    artifact_records: list[dict[str, object]] = [_artifact(staging, layout, "partition-layout")]

    detail_roles: dict[str, str] = {}
    for role in ("bootloader", "app", "partition-table", "otadata"):
        detail = flasher.get(role)
        if isinstance(detail, dict) and isinstance(detail.get("file"), str):
            if detail.get("encrypted") not in (None, "false", False):
                raise PackageError("encrypted ESP-IDF factory components are not yet supported")
            detail_roles[str(detail["file"])] = role

    role_names = {
        "bootloader": "bootloader",
        "app": "application",
        "partition-table": "partition-table",
    }
    components: list[dict[str, object]] = []
    merge_pairs: list[str] = []
    found_roles: set[str] = set()
    application_image: Path | None = None
    for offset_text, relative_source in flash_files.items():
        if not isinstance(offset_text, str) or not isinstance(relative_source, str):
            raise PackageError("ESP-IDF flash_files must map string offsets to string paths")
        try:
            offset = int(offset_text, 0)
        except ValueError as error:
            raise PackageError(f"invalid ESP-IDF flash offset: {offset_text}") from error
        source = (context.work_dir / relative_source).resolve()
        try:
            source.relative_to(context.work_dir.resolve())
        except ValueError as error:
            raise PackageError(
                f"ESP-IDF component escapes build work tree: {relative_source}"
            ) from error
        if not source.is_file():
            raise PackageError(f"ESP-IDF component is missing: {source.relative_to(ROOT)}")
        role = detail_roles.get(relative_source)
        if role is None:
            raise PackageError(f"ESP-IDF component has no explicit role: {relative_source}")
        found_roles.add(role)
        if role == "otadata":
            continue
        if role == "app":
            offset_value = application_partition.get("offset")
            if not isinstance(offset_value, int):
                raise PackageError("ESP-IDF app0 partition has an invalid offset")
            offset = offset_value
            _check_partition_fit(source, application_partition, "application")
        destination = factory_dir / f"{stem}-{role_names[role]}.bin"
        shutil.copy2(source, destination)
        if role == "app":
            application_image = destination
        relative = str(destination.relative_to(staging))
        components.append({"role": role, "path": relative, "offset": offset})
        artifact_records.append(_artifact(staging, destination, "factory-component", offset=offset))
        merge_pairs.extend((hex(offset), str(destination)))

    required_roles = {"bootloader", "app", "partition-table", "otadata"}
    if found_roles != required_roles:
        missing = ", ".join(sorted(required_roles - found_roles))
        extra = ", ".join(sorted(found_roles - required_roles))
        details = "; ".join(
            value
            for value in (
                f"missing: {missing}" if missing else "",
                f"extra: {extra}" if extra else "",
            )
            if value
        )
        raise PackageError(
            f"ESP-IDF native flash roles do not match factory requirements ({details})"
        )

    if application_image is None:
        raise PackageError("ESP-IDF factory package has no application image")
    image_info = run(
        [
            str(python),
            "-m",
            "esptool",
            "--chip",
            "auto",
            _esptool_reset_mode("image-info", context.sdk.key),
            str(application_image),
        ],
        environment=dict(environment),
        capture=True,
    )
    validate_esp_image_info(image_info.stdout, item.sdk_target)
    validate_esp_custom_descriptor(application_image, compatibility)
    ota_image = ota_dir / f"{stem}-ota.bin"
    shutil.copy2(application_image, ota_image)
    if sha256(application_image) != sha256(ota_image):
        raise PackageError("ESP-IDF OTA image differs from the native application image")
    ota_relative = str(ota_image.relative_to(staging))
    application_offset = application_partition.get("offset")
    application_limit = application_partition.get("size")
    if not isinstance(application_offset, int) or not isinstance(application_limit, int):
        raise PackageError("ESP-IDF app0 partition metadata is invalid")
    artifact_records.append(
        _artifact(
            staging,
            ota_image,
            "application-update",
            offset=application_offset,
            size_limit=application_limit,
        )
    )

    safeboot_source = _verified_safeboot_dependency(context)
    _check_partition_fit(safeboot_source, safeboot_partition, "safeboot")
    safeboot_offset = safeboot_partition.get("offset")
    if not isinstance(safeboot_offset, int):
        raise PackageError("ESP-IDF safeboot partition has an invalid offset")
    safeboot = safeboot_dir / f"{stem}-safeboot.bin"
    shutil.copy2(safeboot_source, safeboot)
    safeboot_relative = str(safeboot.relative_to(staging))
    components.append({"role": "safeboot", "path": safeboot_relative, "offset": safeboot_offset})
    artifact_records.append(
        _artifact(staging, safeboot, "factory-component", offset=safeboot_offset)
    )
    merge_pairs.extend((hex(safeboot_offset), str(safeboot)))

    otadata_offset = otadata_partition.get("offset")
    otadata_size = otadata_partition.get("size")
    if not isinstance(otadata_offset, int) or not isinstance(otadata_size, int):
        raise PackageError("ESP-IDF otadata partition metadata is invalid")
    otadata = factory_dir / f"{stem}-ota-data-initial.bin"
    _write_initial_otadata(otadata, otadata_size)
    otadata_relative = str(otadata.relative_to(staging))
    components.append({"role": "otadata", "path": otadata_relative, "offset": otadata_offset})
    artifact_records.append(_artifact(staging, otadata, "factory-component", offset=otadata_offset))
    merge_pairs.extend((hex(otadata_offset), str(otadata)))

    final_partition = partition_metadata["expandable_final"]
    if not isinstance(final_partition, dict):
        raise PackageError("ESP-IDF package has no filesystem partition metadata")
    filesystem_offset = final_partition.get("offset")
    filesystem_size = final_partition.get("packaged_size")
    if not isinstance(filesystem_offset, int) or not isinstance(filesystem_size, int):
        raise PackageError("ESP-IDF filesystem partition metadata is invalid")
    filesystem = factory_dir / f"{stem}-littlefs.bin"
    create_filesystem_image(filesystem, filesystem_size, item.filesystem)
    filesystem_details = image_metadata(filesystem_offset, filesystem_size)
    filesystem_relative = str(filesystem.relative_to(staging))
    components.append(
        {"role": "filesystem", "path": filesystem_relative, "offset": filesystem_offset}
    )
    artifact_records.append(
        _artifact(
            staging,
            filesystem,
            "factory-component",
            offset=filesystem_offset,
            filesystem=filesystem_details,
        )
    )
    merge_pairs.extend((hex(filesystem_offset), str(filesystem)))

    factory = factory_dir / f"{stem}-factory.bin"
    run(
        [
            str(python),
            "-m",
            "esptool",
            "--chip",
            item.sdk_target,
            _esptool_reset_mode("merge-bin", context.sdk.key),
            "--output",
            str(factory),
            *write_args,
            *merge_pairs,
        ],
        environment=dict(environment),
    )
    artifact_records.insert(0, _artifact(staging, factory, "factory", offset=0))

    debug_sources = (
        (context.work_dir / "newmota.elf", "elf"),
        (context.work_dir / "newmota.map", "map"),
    )
    for source, kind in debug_sources:
        destination = debug_dir / f"{stem}.{kind}"
        shutil.copy2(source, destination)
        artifact_records.append(_artifact(staging, destination, "debug"))

    project_description_path = context.work_dir / "project_description.json"
    try:
        project_description = json.loads(project_description_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PackageError(f"cannot read ESP-IDF project description: {error}") from error
    monitor_baud = str(project_description.get("monitor_baud", "115200"))
    toolchain_prefix = str(project_description.get("monitor_toolprefix", ""))
    tool_result = run(
        [str(python), "-m", "esptool", "version"],
        environment=dict(environment),
        capture=True,
    )
    tool_version = next(
        (line.strip() for line in reversed(tool_result.stdout.splitlines()) if line.strip()),
        "unknown",
    )

    partition_component = next(
        (component for component in components if component["role"] == "partition-table"), None
    )
    if partition_component is None:
        raise PackageError("ESP-IDF factory package has no partition-table component")
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
            "idf_version": item.idf_version,
            "platform": item.platform,
            "role": item.role,
            "profile": item.profile,
            "safeboot": item.safeboot,
            "language": item.language,
            "flash_bytes": item.flash_kb * 1024,
            "psram_bytes": item.psram_kb * 1024 if item.psram_kb is not None else None,
        },
        "sdk": {
            "key": context.sdk.key,
            "name": context.sdk.name,
            "version": context.sdk.version,
            "revision": context.sdk.revision,
        },
        "host_tools": {
            "esptool": tool_version,
            "littlefs-python": filesystem_details["generator"],
        },
        "configuration": {
            "environment": item.configuration_provenance(),
            "identity": context.native_config.identity,
            "partition": partition_metadata,
            "compatibility": compatibility,
            "filesystem": filesystem_details,
        },
        "qualification": {
            "factory": "esp-idf-native-development",
            "safeboot": "layout-only-development",
            "ota": "unqualified",
            "deployment": "unqualified",
        },
        "flashable": True,
        "deployable": False,
        "signing": {"status": "unsigned-development"},
        "ota": {
            "status": "raw-esp-idf-app",
            "artifacts": [
                {
                    "role": "application-update",
                    "format": "esp-idf-app",
                    "path": ota_relative,
                    "offset": application_offset,
                    "size": ota_image.stat().st_size,
                    "size_limit": application_limit,
                    "layout_id": compatibility["layout_id"],
                }
            ],
        },
        "safeboot": {
            "environment": item.safeboot,
            "path": safeboot_relative,
            "offset": safeboot_offset,
            "size_limit": safeboot_partition["size"],
        },
        "flash": {
            "before": _esptool_reset_mode(
                str(extra_args.get("before", "default_reset")), context.sdk.key
            ),
            "after": post_flash_reset,
            "stub": True,
            "write_args": write_args,
            "components": components,
            "partition_table": {
                "offset": partition_component["offset"],
                "component": partition_component["path"],
                "layout": str(layout.relative_to(staging)),
                **partition_metadata,
            },
        },
        "monitor": {
            "baud": int(monitor_baud),
            "toolchain_prefix": toolchain_prefix,
            "elf": str((debug_dir / f"{stem}.elf").relative_to(staging)),
        },
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
