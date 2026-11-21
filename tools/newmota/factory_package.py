"""Write and check the factory package, the one file every wired flasher takes."""

from __future__ import annotations

import hashlib
import json
import re
import zipfile
from dataclasses import dataclass
from itertools import pairwise
from pathlib import Path, PurePosixPath
from typing import Any

from newmota.package import FirmwarePackage, PackageError

FACTORY_FORMAT = "newmota-factory"
FACTORY_SCHEMA = 1
DESCRIPTION = "package.json"
IMAGE_DIR = "images"
ERASE_SIZE = 4096
# Steps a flasher must carry out besides plain writes; an unknown one makes it refuse the package.
# beken-rf-check: before any write, RF calibration must sit at its offset, so a module with
# another layout is not overwritten.
BEKEN_RF_CHECK = "beken-rf-check"
BEKEN_NET_RESET = "beken-net-reset"
ESP_PARTITION_TABLE = "esp-partition-table"
# bouffalo-partition-table: on a larger flash, LittleFS in both table copies grows to the end.
BOUFFALO_PARTITION_TABLE = "bouffalo-partition-table"
STEP_KINDS = (BEKEN_RF_CHECK, BEKEN_NET_RESET, ESP_PARTITION_TABLE, BOUFFALO_PARTITION_TABLE)
ESP_OPTIONS = {
    "--flash_mode": "flash_mode",
    "--flash_freq": "flash_freq",
    "--flash_size": "flash_size",
}
# Fixed entry time so the same images give the same archive.
ENTRY_TIME = (1980, 1, 1, 0, 0, 0)
HASH = re.compile(r"[0-9a-f]{64}")


@dataclass(frozen=True)
class FactoryPackage:
    """A checked factory package: its description and the images by archive name."""

    path: Path
    description: dict[str, Any]
    images: dict[str, bytes]

    def steps(self, allowed: tuple[str, ...], family: str) -> list[dict[str, Any]]:
        """Return the steps, refusing any this chip family's flasher cannot carry out."""

        steps = self.description["steps"]
        foreign = [step["kind"] for step in steps if step["kind"] not in allowed]
        if foreign:
            raise PackageError(
                f"{family} cannot carry out factory package steps: " + ", ".join(foreign)
            )
        return steps

    def write_images(self, directory: Path) -> list[tuple[str, int, Path]]:
        """Write each image into @p directory; return name, offset and file in write order."""

        directory.mkdir(parents=True, exist_ok=True)
        written: list[tuple[str, int, Path]] = []
        for item in self.description["write"]:
            path = directory / PurePosixPath(item["file"]).name
            path.write_bytes(self.images[item["file"]])
            written.append((str(item["name"]), int(item["offset"]), path))
        return written


def factory_package_path(package: FirmwarePackage) -> Path:
    environment = package.environment
    version = str(package.data["version"])
    return package.root / f"newmota-{environment}-{version}-factory.zip"


def _steps(package: FirmwarePackage, flash: dict[str, Any]) -> list[dict[str, Any]]:
    steps: list[dict[str, Any]] = []
    if package.platform == "beken":
        rf = [region for region in package.data["layout"]["regions"] if region["name"] == "rf"]
        if len(rf) != 1:
            raise PackageError("Beken package layout has no single RF calibration region")
        steps.append({"kind": BEKEN_RF_CHECK, "offset": rf[0]["offset"]})
    net = flash.get("net_initialization")
    if isinstance(net, dict):
        steps.append(
            {
                "kind": BEKEN_NET_RESET,
                "policy": net["policy"],
                "offset": net["offset"],
                "size": net["size"],
            }
        )
    if package.platform == "bouffalo":
        tables = [
            component["offset"]
            for component in flash["components"]
            if str(component.get("name", "")).startswith("partition-table")
        ]
        filesystem = [
            region for region in package.data["layout"]["regions"] if region["name"] == "littlefs"
        ]
        if not tables or len(filesystem) != 1:
            raise PackageError("Bouffalo package has no partition tables or LittleFS region")
        steps.append(
            {
                "kind": BOUFFALO_PARTITION_TABLE,
                "tables": tables,
                "filesystem_offset": filesystem[0]["offset"],
            }
        )
    table = flash.get("partition_table")
    if isinstance(table, dict):
        steps.append(
            {
                "kind": ESP_PARTITION_TABLE,
                "offset": table["offset"],
                "partitions": table["partitions"],
                "expandable_final": table["expandable_final"],
            }
        )
    return steps


def _options(package: FirmwarePackage, flash: dict[str, Any]) -> dict[str, Any]:
    if package.platform != "esp":
        return {}
    arguments = [str(item) for item in flash.get("write_args", [])]
    options = {
        ESP_OPTIONS[name]: value for name, value in pairwise(arguments) if name in ESP_OPTIONS
    }
    return {"esp": options}


def describe_factory_package(package: FirmwarePackage) -> tuple[dict[str, Any], dict[str, Path]]:
    """Return the package description and its images, from a checked build package."""

    data = package.data
    flash = data["flash"]
    artifacts = {str(item["path"]): item for item in data["artifacts"]}
    images: dict[str, Path] = {}
    writes: list[dict[str, Any]] = []
    for component in flash["components"]:
        relative = str(component["path"])
        name = f"{IMAGE_DIR}/{PurePosixPath(relative).name}"
        if name in images:
            raise PackageError(f"factory package has two images named {name}")
        artifact = artifacts[relative]
        images[name] = package.file(relative)
        writes.append(
            {
                "name": str(component.get("name") or PurePosixPath(relative).name),
                "role": str(component.get("role") or artifact["role"]),
                "offset": component["offset"],
                "file": name,
                "size": artifact["size"],
                "sha256": artifact["sha256"],
            }
        )
    target = data["target"]
    description = {
        "format": FACTORY_FORMAT,
        "schema": FACTORY_SCHEMA,
        "product": data["product"],
        "version": data["version"],
        "source": data["source"],
        "environment": target["environment"],
        "board": target["board"],
        "target": {
            "platform": target["platform"],
            "chip": target["chip"],
            "flash_bytes": target["flash_bytes"],
            "psram_bytes": target.get("psram_bytes"),
        },
        "write": writes,
        "steps": _steps(package, flash),
        "preserve": list(flash.get("preserves", [])),
        "options": _options(package, flash),
    }
    return description, images


def write_factory_package(package: FirmwarePackage) -> Path:
    """Write the factory package next to the build manifest and check it like a flasher."""

    description, images = describe_factory_package(package)
    destination = factory_package_path(package)
    temporary = destination.with_suffix(".zip.tmp")
    with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED) as archive:
        entries = [(DESCRIPTION, (json.dumps(description, indent=2) + "\n").encode())]
        entries.extend((name, path.read_bytes()) for name, path in sorted(images.items()))
        for name, content in entries:
            archive.writestr(zipfile.ZipInfo(name, ENTRY_TIME), content, zipfile.ZIP_DEFLATED)
    load_factory_package(temporary)
    temporary.replace(destination)
    return destination


def _object(value: object, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise PackageError(f"factory package field '{field}' must be an object")
    return value


def _string(value: object, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise PackageError(f"factory package field '{field}' must be a non-empty string")
    return value


def _integer(value: object, field: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise PackageError(f"factory package field '{field}' must be a non-negative integer")
    return value


def _range(item: dict[str, Any], field: str, flash_bytes: int) -> tuple[int, int]:
    offset = _integer(item.get("offset"), f"{field}.offset")
    size = _integer(item.get("size"), f"{field}.size")
    if offset % ERASE_SIZE:
        raise PackageError(f"{field} does not start on a {ERASE_SIZE}-byte sector")
    if size == 0 or offset + size > flash_bytes:
        raise PackageError(f"{field} is empty or exceeds the target flash")
    return offset, offset + size


def load_factory_package(path: Path) -> FactoryPackage:
    """Check a factory package as a flasher must before it opens a device."""

    try:
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            if len(names) != len(set(names)):
                raise PackageError("factory package lists an entry twice")
            contents = {name: archive.read(name) for name in names}
    except (OSError, zipfile.BadZipFile) as error:
        raise PackageError(f"cannot read factory package {path}: {error}") from error
    if DESCRIPTION not in contents:
        raise PackageError(f"factory package has no {DESCRIPTION}")
    try:
        description = _object(json.loads(contents[DESCRIPTION]), "root")
    except (UnicodeError, json.JSONDecodeError) as error:
        raise PackageError(f"cannot read {DESCRIPTION}: {error}") from error
    if description.get("format") != FACTORY_FORMAT:
        raise PackageError("file is not a Newmota factory package")
    if description.get("schema") != FACTORY_SCHEMA:
        raise PackageError(f"unsupported factory package schema: {description.get('schema')!r}")

    target = _object(description.get("target"), "target")
    for field in ("platform", "chip"):
        _string(target.get(field), f"target.{field}")
    flash_bytes = _integer(target.get("flash_bytes"), "target.flash_bytes")
    _string(description.get("environment"), "environment")
    _string(description.get("version"), "version")

    writes = description.get("write")
    if not isinstance(writes, list) or not writes:
        raise PackageError("factory package writes nothing")
    images: dict[str, bytes] = {}
    ranges: list[tuple[int, int, str]] = []
    for index, raw in enumerate(writes):
        item = _object(raw, f"write[{index}]")
        name = _string(item.get("file"), f"write[{index}].file")
        if PurePosixPath(name).parent != PurePosixPath(IMAGE_DIR) or name in images:
            raise PackageError(f"write[{index}] names an invalid or repeated image: {name}")
        if name not in contents:
            raise PackageError(f"factory package image is missing: {name}")
        content = contents[name]
        start, end = _range(item, f"write[{index}]", flash_bytes)
        if len(content) != end - start:
            raise PackageError(f"factory package image size mismatch: {name}")
        expected = _string(item.get("sha256"), f"write[{index}].sha256")
        if not HASH.fullmatch(expected) or hashlib.sha256(content).hexdigest() != expected:
            raise PackageError(f"factory package image hash mismatch: {name}")
        images[name] = content
        ranges.append((start, end, name))
    if set(contents) != {DESCRIPTION, *images}:
        raise PackageError("factory package holds files it does not write")
    ranges.sort()
    for left, right in pairwise(ranges):
        if left[1] > right[0]:
            raise PackageError(f"factory package writes overlap: {left[2]} and {right[2]}")

    steps = description.get("steps")
    if not isinstance(steps, list):
        raise PackageError("factory package field 'steps' must be a list")
    for index, raw in enumerate(steps):
        step = _object(raw, f"steps[{index}]")
        kind = step.get("kind")
        if kind not in STEP_KINDS:
            raise PackageError(f"factory package step is unknown: {kind!r}")
        if kind == BEKEN_RF_CHECK:
            offset = _integer(step.get("offset"), f"steps[{index}].offset")
            if offset % ERASE_SIZE or offset >= flash_bytes:
                raise PackageError("Beken RF calibration check has an invalid offset")
            if any(left <= offset < right for left, right, _ in ranges):
                raise PackageError("Beken RF calibration lies inside a written image")
        elif kind == BEKEN_NET_RESET:
            _string(step.get("policy"), f"steps[{index}].policy")
            start, end = _range(step, f"steps[{index}]", flash_bytes)
            if any(start < right and left < end for left, right, _ in ranges):
                raise PackageError("Beken NET reset overlaps a written image")
        elif kind == BOUFFALO_PARTITION_TABLE:
            starts = {item.get("offset") for item in writes}
            tables = step.get("tables")
            if (
                not isinstance(tables, list)
                or not tables
                or not all(table in starts for table in tables)
                or step.get("filesystem_offset") not in starts
            ):
                raise PackageError(
                    "Bouffalo partition-table step does not match the written tables and LittleFS"
                )
        else:
            if not isinstance(step.get("partitions"), list):
                raise PackageError(f"steps[{index}].partitions must be a list")
            _object(step.get("expandable_final"), f"steps[{index}].expandable_final")
            if not any(
                item.get("role") == "partition-table" and item.get("offset") == step.get("offset")
                for item in writes
            ):
                raise PackageError(
                    "ESP partition-table step does not match a written partition table"
                )
    if not isinstance(description.get("preserve"), list):
        raise PackageError("factory package field 'preserve' must be a list")
    _object(description.get("options"), "options")
    return FactoryPackage(path, description, images)
