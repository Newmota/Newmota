"""Shared package validation, layout facts, and LittleFS images."""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass
from importlib.metadata import version
from itertools import pairwise
from pathlib import Path
from typing import Any

from newmota.host import sha256, write_if_changed
from newmota.model import (
    FIRMWARE_DIR,
    ROOT,
    Artifact,
    BuildError,
    FactoryFile,
)
from newmota.ota import (
    OTA_ENVELOPE_FORMAT,
    geometry_from_package,
    validate_ota_envelope,
)


@dataclass(frozen=True)
class LayoutRegion:
    """One bounded region resolved from a platform-native layout input."""

    name: str
    role: str
    offset: int
    size: int
    image_budget: int | None = None

    @property
    def end(self) -> int:
        """Return the exclusive physical end offset."""

        return self.offset + self.size

    def metadata(self) -> dict[str, object]:
        """Return the stable serialized form used by build manifests."""

        result: dict[str, object] = {
            "name": self.name,
            "role": self.role,
            "offset": self.offset,
            "size": self.size,
            "end": self.end,
        }
        if self.image_budget is not None:
            result["image_budget"] = self.image_budget
        return result


@dataclass(frozen=True)
class LayoutOverlap:
    """One deliberate overlap between two resolved native regions."""

    first: str
    second: str
    reason: str

    def metadata(self) -> dict[str, str]:
        return {"first": self.first, "second": self.second, "reason": self.reason}


@dataclass(frozen=True)
class LayoutFacts:
    """Small platform-neutral result consumed by features and packaging."""

    platform: str
    flash_bytes: int
    erase_size: int
    regions: tuple[LayoutRegion, ...]
    overlaps: tuple[LayoutOverlap, ...]
    application_update_reach: tuple[str, ...]
    settings_filesystem_preserved: bool
    source_files: tuple[Path, ...]
    manifest: Path
    identity: str

    def region(self, name: str) -> LayoutRegion:
        """Return exactly one named region or raise a descriptive error."""

        matches = [region for region in self.regions if region.name == name]
        if len(matches) != 1:
            raise ValueError(f"layout requires exactly one '{name}' region")
        return matches[0]

    def metadata(self) -> dict[str, object]:
        """Return normalized facts without host-specific generated paths."""

        return {
            "schema": 1,
            "platform": self.platform,
            "identity": self.identity,
            "flash_bytes": self.flash_bytes,
            "erase_size": self.erase_size,
            "regions": [region.metadata() for region in self.regions],
            "overlaps": [overlap.metadata() for overlap in self.overlaps],
            "application_update_reach": list(self.application_update_reach),
            "settings_filesystem_preserved": self.settings_filesystem_preserved,
            "sources": [str(path) for path in self.source_files],
        }


def write_layout_facts(
    *,
    platform: str,
    flash_bytes: int,
    erase_size: int,
    regions: tuple[LayoutRegion, ...],
    overlaps: tuple[LayoutOverlap, ...],
    application_update_reach: tuple[str, ...],
    settings_filesystem_preserved: bool,
    source_files: tuple[Path, ...],
    destination: Path,
) -> LayoutFacts:
    """Validate, identify, and write one normalized layout result."""

    if flash_bytes <= 0 or erase_size <= 0 or flash_bytes % erase_size:
        raise ValueError("layout flash and erase geometry must be positive and aligned")
    names: set[str] = set()
    for region in regions:
        if region.name in names:
            raise ValueError(f"duplicate layout region '{region.name}'")
        names.add(region.name)
        if (
            region.offset < 0
            or region.size <= 0
            or region.offset % erase_size
            or region.size % erase_size
            or region.end > flash_bytes
        ):
            raise ValueError(f"layout region '{region.name}' has invalid bounds")
    if any(name not in names for name in application_update_reach):
        raise ValueError("application update reach names an unknown layout region")
    if any(overlap.first not in names or overlap.second not in names for overlap in overlaps):
        raise ValueError("layout overlap names an unknown region")
    declared_overlaps = {frozenset((overlap.first, overlap.second)) for overlap in overlaps}
    if len(declared_overlaps) != len(overlaps):
        raise ValueError("layout contains a duplicate declared overlap")
    actual_overlaps = {
        frozenset((first.name, second.name))
        for index, first in enumerate(regions)
        for second in regions[index + 1 :]
        if first.offset < second.end and second.offset < first.end
    }
    if actual_overlaps != declared_overlaps:
        raise ValueError("layout actual and declared overlaps differ")

    identity_input = {
        "schema": 1,
        "platform": platform,
        "flash_bytes": flash_bytes,
        "erase_size": erase_size,
        "regions": [region.metadata() for region in regions],
        "overlaps": [overlap.metadata() for overlap in overlaps],
        "application_update_reach": list(application_update_reach),
        "settings_filesystem_preserved": settings_filesystem_preserved,
        "sources": [str(path) for path in source_files],
    }
    encoded = json.dumps(identity_input, sort_keys=True, separators=(",", ":")).encode()
    identity = hashlib.sha256(encoded).hexdigest()
    facts = LayoutFacts(
        platform,
        flash_bytes,
        erase_size,
        regions,
        overlaps,
        application_update_reach,
        settings_filesystem_preserved,
        source_files,
        destination,
        identity,
    )
    write_if_changed(destination, json.dumps(facts.metadata(), indent=2) + "\n")
    return facts


GENERATOR_PACKAGE = "littlefs-python"
GENERATOR_VERSION = "0.18.0"
GENERATOR_CORE_VERSION = (2, 11)
RUNTIME_CORE_VERSION = "2.11.3"
RUNTIME_CORE_REVISION = "6cb4e86540eca0d9ba62500a298385c9d863c8be"
DISK_VERSION = 0x00020000
READ_SIZE = 128
PROGRAM_SIZE = 128
CACHE_SIZE = 512
LOOKAHEAD_SIZE = 128
BLOCK_CYCLES = 512
BLOCK_SIZE = 4096


class LittlefsImageError(BuildError):
    """Raised when a compatible factory filesystem cannot be generated."""


def image_metadata(offset: int, size: int) -> dict[str, object]:
    return {
        "generator": f"{GENERATOR_PACKAGE} {GENERATOR_VERSION}",
        "generator_core": ".".join(str(part) for part in GENERATOR_CORE_VERSION),
        "runtime_core": RUNTIME_CORE_VERSION,
        "runtime_revision": RUNTIME_CORE_REVISION,
        "disk_version": "2.0",
        "geometry": {
            "read_size": READ_SIZE,
            "program_size": PROGRAM_SIZE,
            "cache_size": CACHE_SIZE,
            "lookahead_size": LOOKAHEAD_SIZE,
            "block_cycles": BLOCK_CYCLES,
            "block_size": BLOCK_SIZE,
            "block_count": size // BLOCK_SIZE,
        },
        "offset": offset,
        "size": size,
    }


def create_filesystem_image(destination: Path, size: int, files: tuple[FactoryFile, ...]) -> None:
    """Write the factory LittleFS image of @p size bytes holding an environment's `filesystem`."""

    from littlefs import __LFS_VERSION__, LittleFS, LittleFSError

    installed_version = version(GENERATOR_PACKAGE)
    if installed_version != GENERATOR_VERSION:
        raise LittlefsImageError(
            f"{GENERATOR_PACKAGE} version mismatch: expected {GENERATOR_VERSION}, "
            f"got {installed_version}"
        )
    if __LFS_VERSION__ != GENERATOR_CORE_VERSION:
        raise LittlefsImageError(
            "littlefs-python core mismatch: expected LittleFS 2.11, "
            f"got {'.'.join(str(part) for part in __LFS_VERSION__)}"
        )
    if size < 2 * BLOCK_SIZE or size % BLOCK_SIZE:
        raise LittlefsImageError(
            f"LittleFS image size must be a multiple of {BLOCK_SIZE} bytes: {size}"
        )

    filesystem = LittleFS(
        mount=False,
        block_size=BLOCK_SIZE,
        block_count=size // BLOCK_SIZE,
        read_size=READ_SIZE,
        prog_size=PROGRAM_SIZE,
        cache_size=CACHE_SIZE,
        lookahead_size=LOOKAHEAD_SIZE,
        block_cycles=BLOCK_CYCLES,
        disk_version=DISK_VERSION,
    )
    filesystem.format()
    filesystem.mount()
    status = filesystem.fs_stat()
    if status.disk_version != DISK_VERSION or status.block_count != size // BLOCK_SIZE:
        raise LittlefsImageError("generated LittleFS image has unexpected filesystem metadata")
    for entry in files:
        parent = entry.target.rsplit("/", 1)[0]
        try:
            if parent:
                filesystem.makedirs(parent, exist_ok=True)
            with filesystem.open(entry.target, "wb") as handle:
                handle.write((ROOT / entry.source).read_bytes())
        except LittleFSError as error:
            if error.code != LittleFSError.Error.LFS_ERR_NOSPC:
                raise LittlefsImageError(f"cannot write {entry.target}: {error.name}") from error
            raise LittlefsImageError(
                f"filesystem files do not fit the {size // 1024} KB factory filesystem; "
                f"{entry.source} found no room"
            ) from error
    filesystem.unmount()

    image = bytes(filesystem.context.buffer)
    if len(image) != size:
        raise LittlefsImageError(
            f"generated LittleFS image has {len(image)} bytes, expected {size}"
        )
    destination.write_bytes(image)


PACKAGE_SCHEMA = 1
PACKAGE_NAME = re.compile(r"[a-z0-9][a-z0-9.-]*")


class PackageError(BuildError):
    """Raised when a firmware package is incomplete, unsafe, or inconsistent."""


@dataclass(frozen=True)
class FirmwarePackage:
    """A validated package manifest and its package root."""

    manifest_path: Path
    root: Path
    data: dict[str, Any]

    @property
    def environment(self) -> str:
        return str(self.data["target"]["environment"])

    @property
    def platform(self) -> str:
        return str(self.data["target"]["platform"])

    @property
    def chip(self) -> str:
        return str(self.data["target"]["chip"])

    @property
    def flash_bytes(self) -> int:
        return int(self.data["target"]["flash_bytes"])

    def file(self, relative: str) -> Path:
        return _package_file(self.root, relative)


def package_manifest_path(environment: str, version: str) -> Path:
    return FIRMWARE_DIR / environment / version / "manifest.json"


def _manifest_native_artifacts(
    manifest: dict[str, object], root: Path, owner: str
) -> tuple[Artifact, ...]:
    """Resolve and hash-check native artifacts from a paired-image build."""

    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, list):
        raise PackageError(f"{owner} build manifest has no artifacts")
    resolved: list[Artifact] = []
    for item in artifacts:
        if not isinstance(item, dict) or item.get("role") != "native":
            continue
        relative = item.get("path")
        if not isinstance(relative, str):
            raise PackageError(f"{owner} build manifest has an invalid artifact path")
        path = (ROOT / relative).resolve()
        try:
            path.relative_to(root.resolve())
        except ValueError as error:
            raise PackageError(
                f"{owner} artifact is outside its build directory: {relative}"
            ) from error
        if not path.is_file():
            raise PackageError(f"{owner} artifact is missing: {relative}")
        if item.get("size") != path.stat().st_size or item.get("sha256") != sha256(path):
            raise PackageError(f"{owner} artifact does not match its build manifest: {relative}")
        resolved.append(Artifact(path, "native"))
    return tuple(resolved)


def _package_file(root: Path, relative: str) -> Path:
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError as error:
        raise PackageError(f"package path escapes its root: {relative}") from error
    return candidate


def _required_object(value: object, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise PackageError(f"package manifest field '{field}' must be an object")
    return value


def _required_string(value: object, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise PackageError(f"package manifest field '{field}' must be a non-empty string")
    return value


def _required_integer(value: object, field: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise PackageError(f"package manifest field '{field}' must be a non-negative integer")
    return value


def load_package(manifest_path: Path) -> FirmwarePackage:
    """Load a package and verify every declared file before device access."""

    manifest_path = manifest_path.expanduser().resolve()
    if not manifest_path.is_file():
        raise PackageError(f"package manifest does not exist: {manifest_path}")
    if manifest_path.name != "manifest.json":
        raise PackageError("PACKAGE must point to a package manifest.json, not a raw binary")
    try:
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PackageError(f"cannot read package manifest {manifest_path}: {error}") from error
    data = _required_object(document, "root")
    if data.get("schema") != PACKAGE_SCHEMA:
        raise PackageError(f"unsupported package manifest schema: {data.get('schema')!r}")
    if data.get("product") != "newmota":
        raise PackageError("package manifest is not a Newmota package")
    if data.get("flashable") is not True:
        raise PackageError("package is not qualified for wired flashing")
    qualification = _required_object(data.get("qualification"), "qualification")
    factory_qualification = _required_string(qualification.get("factory"), "qualification.factory")
    if factory_qualification == "unqualified":
        raise PackageError("package factory image is unqualified")

    target = _required_object(data.get("target"), "target")
    for field in ("environment", "platform", "chip"):
        _required_string(target.get(field), f"target.{field}")
    if target.get("role") != "application":
        raise PackageError("wired-flash package target is not an application environment")
    flash_bytes = _required_integer(target.get("flash_bytes"), "target.flash_bytes")
    if flash_bytes < 2 * 1024 * 1024:
        raise PackageError("package targets less than Newmota's 2 MiB minimum flash")

    flash = _required_object(data.get("flash"), "flash")
    components = flash.get("components")
    if not isinstance(components, list) or not components:
        raise PackageError("package has no factory flash components")

    artifacts = data.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        raise PackageError("package manifest has no artifacts")
    declared_paths: dict[str, str] = {}
    declared_sizes: dict[str, int] = {}
    declared_artifacts: dict[str, dict[str, Any]] = {}
    root = manifest_path.parent
    for index, item in enumerate(artifacts):
        artifact = _required_object(item, f"artifacts[{index}]")
        relative = _required_string(artifact.get("path"), f"artifacts[{index}].path")
        if relative in declared_paths:
            raise PackageError(f"package declares artifact more than once: {relative}")
        role = _required_string(artifact.get("role"), f"artifacts[{index}].role")
        declared_paths[relative] = role
        declared_artifacts[relative] = artifact
        path = _package_file(root, relative)
        if not path.is_file():
            raise PackageError(f"package artifact is missing: {relative}")
        expected_size = _required_integer(artifact.get("size"), f"artifacts[{index}].size")
        if path.stat().st_size != expected_size:
            raise PackageError(f"package artifact size mismatch: {relative}")
        declared_sizes[relative] = expected_size
        expected_hash = _required_string(artifact.get("sha256"), f"artifacts[{index}].sha256")
        if not re.fullmatch(r"[0-9a-f]{64}", expected_hash) or sha256(path) != expected_hash:
            raise PackageError(f"package artifact hash mismatch: {relative}")

    ota = data.get("ota")
    if ota is not None:
        ota_data = _required_object(ota, "ota")
        ota_artifacts = ota_data.get("artifacts")
        if not isinstance(ota_artifacts, list):
            raise PackageError("package OTA artifacts must be a list")
        for index, item in enumerate(ota_artifacts):
            update = _required_object(item, f"ota.artifacts[{index}]")
            role = _required_string(update.get("role"), f"ota.artifacts[{index}].role")
            if role != "application-update":
                raise PackageError(f"ota.artifacts[{index}] has unsupported update role: {role}")
            relative = _required_string(update.get("path"), f"ota.artifacts[{index}].path")
            if relative not in declared_paths:
                raise PackageError(f"OTA artifact is not declared in package artifacts: {relative}")
            artifact_role = declared_paths[relative]
            if artifact_role not in {"application-update", "factory-component"}:
                raise PackageError(
                    f"OTA path does not reference an application artifact: {relative}"
                )
            size = declared_sizes[relative]
            if (
                "size" in update
                and _required_integer(update.get("size"), f"ota.artifacts[{index}].size") != size
            ):
                raise PackageError(f"OTA artifact size does not match package artifact: {relative}")
            size_limit = _required_integer(
                update.get("size_limit"), f"ota.artifacts[{index}].size_limit"
            )
            if size_limit <= 0:
                raise PackageError(f"OTA artifact has an empty native image limit: {relative}")
            artifact = declared_artifacts[relative]
            envelope_format = artifact.get("format") == OTA_ENVELOPE_FORMAT
            if envelope_format:
                payload_size = _required_integer(
                    update.get("payload_size"), f"ota.artifacts[{index}].payload_size"
                )
                native_size = _required_integer(
                    update.get("native_size"), f"ota.artifacts[{index}].native_size"
                )
                overhead = _required_integer(
                    update.get("native_payload_overhead", 0),
                    f"ota.artifacts[{index}].native_payload_overhead",
                )
                if (
                    payload_size != size - 72
                    or payload_size > size_limit + overhead
                    or native_size == 0
                    or native_size > size_limit
                ):
                    raise PackageError(f"OTA envelope exceeds its native image limit: {relative}")
                try:
                    layout_config = _required_object(data.get("layout"), "layout")
                    layout_digest = _required_string(
                        layout_config.get("identity"), "layout.identity"
                    )
                    if not re.fullmatch(r"[0-9a-f]{64}", layout_digest):
                        raise ValueError("resolved layout identity is malformed")
                    application, safeboot, filesystem = geometry_from_package(data)
                    envelope = validate_ota_envelope(
                        _package_file(root, relative),
                        chip=str(target["chip"]),
                        flash_bytes=flash_bytes,
                        layout_sha256=layout_digest,
                        application=application,
                        safeboot=safeboot,
                        filesystem=filesystem,
                    )
                except (ValueError, KeyError, TypeError) as error:
                    raise PackageError(
                        f"OTA envelope does not match the package: {relative}: {error}"
                    ) from error
                if envelope["payload_size"] != payload_size:
                    raise PackageError(f"OTA envelope payload size mismatch: {relative}")
            elif size > size_limit:
                raise PackageError(f"OTA artifact exceeds its native image limit: {relative}")
            offset = _required_integer(update.get("offset"), f"ota.artifacts[{index}].offset")
            if offset + size_limit > flash_bytes:
                raise PackageError(f"OTA artifact exceeds target flash capacity: {relative}")

            if artifact_role == "factory-component":
                matching_components = [
                    _required_object(component, f"flash.components[{component_index}]")
                    for component_index, component in enumerate(components)
                    if isinstance(component, dict) and component.get("path") == relative
                ]
                if (
                    len(matching_components) != 1
                    or declared_artifacts[relative].get("update_role") != "application"
                    or matching_components[0].get("update_role") != "application"
                    or matching_components[0].get("offset") != offset
                    or declared_artifacts[relative].get("update_size_limit") != size_limit
                    or matching_components[0].get("update_size_limit") != size_limit
                ):
                    raise PackageError(
                        f"OTA factory component is not explicitly marked as the bounded application: {relative}"
                    )
            elif artifact_role != "application-update":
                raise PackageError(
                    f"OTA path does not reference an application artifact: {relative}"
                )
            elif (
                declared_artifacts[relative].get("offset") != offset
                or declared_artifacts[relative].get("size_limit") != size_limit
            ):
                raise PackageError(
                    f"OTA artifact does not match its declared native update bounds: {relative}"
                )

    offsets: set[int] = set()
    ranges: list[tuple[int, int, str]] = []
    for index, item in enumerate(components):
        component = _required_object(item, f"flash.components[{index}]")
        relative = _required_string(component.get("path"), f"flash.components[{index}].path")
        if relative not in declared_paths:
            raise PackageError(f"flash component is not a declared artifact: {relative}")
        if declared_paths[relative] not in {"factory", "factory-component"}:
            raise PackageError(f"flash component does not have a factory artifact role: {relative}")
        offset = _required_integer(component.get("offset"), f"flash.components[{index}].offset")
        if offset in offsets:
            raise PackageError(f"factory flash offset is declared twice: {offset:#x}")
        offsets.add(offset)
        size = declared_sizes[relative]
        if offset + size > flash_bytes:
            raise PackageError(f"factory flash component exceeds flash capacity: {relative}")
        ranges.append((offset, offset + size, relative))
    ranges.sort()
    for left, right in pairwise(ranges):
        if left[1] > right[0]:
            raise PackageError(f"factory flash components overlap: {left[2]} and {right[2]}")

    return FirmwarePackage(manifest_path, root, data)


def _artifact(root: Path, path: Path, role: str, **extra: object) -> dict[str, object]:
    relative = str(path.relative_to(root))
    return {
        "role": role,
        "path": relative,
        "format": path.suffix.removeprefix(".") or "binary",
        "size": path.stat().st_size,
        "sha256": sha256(path),
        **extra,
    }
