"""Resolve project-owned native SDK configuration for one build environment.

The files below ``config/build`` are immutable inputs.  This module combines the
optional platform defaults, the required chip defaults, derived board settings,
and explicit environment settings into a generated native configuration under
``build/work/<environment>/config``.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from newmota.host import write_if_changed

ROOT = Path(__file__).resolve().parents[2]
BUILD_CONFIG_DIR = ROOT / "config" / "build"

CONFIG_KEY = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
UNSET_LINE = re.compile(r"^#\s*([A-Za-z_][A-Za-z0-9_]*)\s+is\s+not\s+set\s*$")

SDK_LOG_LEVELS = ("none", "error", "warn", "info", "debug", "verbose")
SDK_LOG_LEVEL_VALUES = {level: value for value, level in enumerate(SDK_LOG_LEVELS)}
DEFAULT_SDK_LOG_LEVEL = "warn"


class BuildConfigError(Exception):
    """Raised when native configuration inputs cannot be resolved safely."""


def resolve_sdk_log_level(value: str | None = None, default: str | None = None) -> str:
    """Return the validated build-time SDK logging policy.

    The Makefile exports ``SDK_LOG_LEVEL`` for normal builds.  Keeping the
    fallback here also makes direct ``python -m newmota`` invocations follow
    the same policy.  ``default`` is a platform's level when none is requested.
    """

    raw_value = (
        os.environ.get("SDK_LOG_LEVEL", default or DEFAULT_SDK_LOG_LEVEL)
        if value is None
        else value
    )
    if not isinstance(raw_value, str):
        raise BuildConfigError(f"SDK_LOG_LEVEL must be a string; received {raw_value!r}")
    level = raw_value.strip().lower()
    if level not in SDK_LOG_LEVEL_VALUES:
        choices = ", ".join(SDK_LOG_LEVELS)
        raise BuildConfigError(f"SDK_LOG_LEVEL must be one of {choices}; received {raw_value!r}")
    return level


@dataclass(frozen=True)
class ConfigLayer:
    """One declarative input layer and its parsed assignments."""

    name: str
    kind: str
    path: Path | None
    assignments: tuple[tuple[str, str], ...]
    sha256: str | None


@dataclass(frozen=True)
class ResolvedNativeConfig:
    """Generated native configuration and the identity of its inputs."""

    platform: str
    chip: str
    format: str
    defaults: Path
    manifest: Path
    identity: str
    assignments: tuple[tuple[str, str], ...]
    layers: tuple[ConfigLayer, ...]
    overrides: tuple[tuple[str, str, str], ...]
    sdk_log_level: str
    changed: bool

    @property
    def values(self) -> dict[str, str]:
        return dict(self.assignments)

    def make_arguments(self) -> list[str]:
        """Return assignments suitable for a vendor Make command line."""

        return [f"{key}={value}" for key, value in self.assignments]


def _relative(path: Path) -> str:
    return str(path.relative_to(ROOT))


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sha256(path: Path) -> str:
    return _sha256_bytes(path.read_bytes())


def _normalize_value(value: Any, source: str, key: str) -> str:
    if isinstance(value, bool):
        return "y" if value else "n"
    if isinstance(value, (str, int, float)):
        result = str(value).strip()
        if result:
            return result
    raise BuildConfigError(f"{source}: native setting '{key}' must have a scalar value")


def _parse_file(path: Path) -> tuple[tuple[str, str], ...]:
    assignments: list[tuple[str, str]] = []
    seen: dict[str, str] = {}
    for line_number, original in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = original.strip()
        if not line or line.startswith("#"):
            unset = UNSET_LINE.fullmatch(line)
            if unset:
                key, value = unset.group(1), "n"
            else:
                continue
        else:
            key, separator, value = line.partition("=")
            if not separator:
                raise BuildConfigError(f"{_relative(path)}:{line_number}: expected KEY=VALUE")
            key = key.strip()
            value = value.strip()
            if not CONFIG_KEY.fullmatch(key):
                raise BuildConfigError(
                    f"{_relative(path)}:{line_number}: invalid native setting '{key}'"
                )
            if not value:
                raise BuildConfigError(
                    f"{_relative(path)}:{line_number}: setting '{key}' has no value"
                )

        if key in seen:
            if seen[key] != value:
                raise BuildConfigError(
                    f"{_relative(path)}:{line_number}: conflicting assignments for "
                    f"'{key}' ({seen[key]} vs {value})"
                )
            continue
        seen[key] = value
        assignments.append((key, value))
    return tuple(assignments)


def parse_native_config(path: Path) -> tuple[tuple[str, str], ...]:
    """Validate and return assignments from a checked-in native default."""

    if not path.is_file():
        raise BuildConfigError(f"required native configuration is missing: {_relative(path)}")
    return _parse_file(path)


def _file_layer(path: Path, kind: str) -> ConfigLayer:
    if not path.is_file():
        raise BuildConfigError(f"required native configuration is missing: {_relative(path)}")
    return ConfigLayer(
        name=_relative(path),
        kind=kind,
        path=path,
        assignments=_parse_file(path),
        sha256=_sha256(path),
    )


def _mapping_layer(name: str, kind: str, values: Mapping[str, Any], source: str) -> ConfigLayer:
    assignments: list[tuple[str, str]] = []
    for key, raw_value in values.items():
        if not isinstance(key, str) or not CONFIG_KEY.fullmatch(key):
            raise BuildConfigError(f"{source}: invalid native setting '{key}'")
        assignments.append((key, _normalize_value(raw_value, source, key)))
    serialized = json.dumps(assignments, separators=(",", ":"), ensure_ascii=True).encode()
    return ConfigLayer(
        name=name,
        kind=kind,
        path=None,
        assignments=tuple(assignments),
        sha256=_sha256_bytes(serialized),
    )


def _merge_layers(
    layers: tuple[ConfigLayer, ...], override_layers: tuple[ConfigLayer, ...] = ()
) -> tuple[dict[str, str], list[tuple[str, str, str]]]:
    assignments: dict[str, str] = {}
    origins: dict[str, str] = {}
    overrides: list[tuple[str, str, str]] = []
    for layer in layers:
        for key, value in layer.assignments:
            if key in assignments and assignments[key] != value:
                raise BuildConfigError(
                    f"conflicting native assignments for '{key}': "
                    f"{origins[key]}={assignments[key]} versus {layer.name}={value}"
                )
            assignments[key] = value
            origins[key] = layer.name

    # Explicit final layers are intentional policy decisions, so a changed
    # value is recorded as an override rather than treated as an accidental
    # conflict with a checked-in default or an earlier policy layer.
    for layer in override_layers:
        for key, value in layer.assignments:
            if key in assignments and assignments[key] != value:
                overrides.append((key, assignments[key], value))
                origins[key] = f"{layer.name} (overrides {origins[key]})"
            else:
                origins[key] = layer.name
            assignments[key] = value
    return assignments, overrides


def _render_defaults(
    platform: str,
    chip: str,
    version: str,
    layers: tuple[ConfigLayer, ...],
    assignments: Mapping[str, str],
) -> str:
    lines = [
        "# Generated by tools/newmota/config.py; do not edit.",
        f"# target: {platform}/{chip}",
        f"# framework version: {version}",
        "# source layers:",
    ]
    lines.extend(f"#   {layer.name}" for layer in layers)
    lines.append("")
    lines.extend(f"{key}={value}" for key, value in assignments.items())
    return "\n".join(lines) + "\n"


def resolve_native_config(
    *,
    environment: str,
    platform: str,
    config_directory: str,
    config_format: str,
    chip: str,
    board_requirements: Mapping[str, Any],
    role: str,
    profile: str,
    version: str,
    native: Mapping[str, Any] | None,
    work_dir: Path,
    feature_requirements: Mapping[str, Any] | None = None,
    build_inputs: Mapping[str, Any] | None = None,
    sdk_log_level: str | None = None,
    sdk_log_settings: Mapping[str, Any] | None = None,
) -> ResolvedNativeConfig:
    """Resolve and write native config for one environment.

    ``native`` is the optional explicit environment layer.  The SDK logging
    policy is applied after it so one global build variable cannot be weakened
    by an environment-specific native setting.  Duplicate values in all
    ordinary layers are accepted only when identical.
    """

    effective_sdk_log_level = resolve_sdk_log_level(sdk_log_level)
    platform_dir = BUILD_CONFIG_DIR / config_directory
    chip_default = platform_dir / chip / config_format
    layers: list[ConfigLayer] = []
    common_default = platform_dir / "common" / config_format
    if common_default.is_file():
        layers.append(_file_layer(common_default, "platform-common"))
    layers.append(_file_layer(chip_default, "chip"))

    board_layer = _mapping_layer(
        f"derived board requirements ({environment})",
        "board",
        board_requirements,
        f"board '{environment}'",
    )
    layers.append(board_layer)

    layers.append(
        _mapping_layer(
            f"profile and drivers ({profile})",
            "profile",
            feature_requirements or {},
            "resolved features",
        )
    )
    base_layers = tuple(layers)
    environment_values = {} if native is None else native
    if not isinstance(environment_values, Mapping):
        raise BuildConfigError(f"environment '{environment}' native settings must be a mapping")
    env_layer = _mapping_layer(
        f"environment overrides ({environment})",
        "environment",
        environment_values,
        f"environment '{environment}'",
    )
    layers.append(env_layer)

    sdk_layer = _mapping_layer(
        f"SDK log level ({effective_sdk_log_level})",
        "sdk-log-level",
        sdk_log_settings or {},
        "SDK_LOG_LEVEL",
    )
    layers.append(sdk_layer)
    generated_layer = _mapping_layer(
        f"generated identity ({environment})", "generated", {}, "generated identity"
    )
    layers.append(generated_layer)

    assignments, overrides = _merge_layers(base_layers, (env_layer, sdk_layer))

    identity_document = {
        "build_inputs": dict(build_inputs or {}),
        "sdk_log_level": effective_sdk_log_level,
        "environment": environment,
        "platform": platform,
        "chip": chip,
        "role": role,
        "profile": profile,
        "version": version,
        "format": config_format,
        "layers": [
            {
                "name": layer.name,
                "kind": layer.kind,
                "sha256": layer.sha256,
                "assignments": dict(layer.assignments),
            }
            for layer in layers
        ],
        "assignments": assignments,
        "overrides": overrides,
    }
    identity = _sha256_bytes(
        json.dumps(identity_document, sort_keys=True, separators=(",", ":")).encode()
    )

    output_dir = work_dir / "config"
    output_dir.mkdir(parents=True, exist_ok=True)
    defaults = output_dir / config_format
    manifest = output_dir / "resolved.json"
    previous_identity: str | None = None
    if manifest.is_file():
        try:
            previous_identity = json.loads(manifest.read_text(encoding="utf-8")).get("identity")
        except (OSError, json.JSONDecodeError):
            previous_identity = None

    write_if_changed(
        defaults,
        _render_defaults(platform, chip, version, tuple(layers), assignments),
        encoding="utf-8",
    )
    write_if_changed(
        manifest,
        json.dumps(
            {
                "schema": 1,
                "identity": identity,
                "build_inputs": dict(build_inputs or {}),
                "sdk_log_level": effective_sdk_log_level,
                "environment": environment,
                "platform": platform,
                "chip": chip,
                "role": role,
                "profile": profile,
                "version": version,
                "format": config_format,
                "defaults": _relative(defaults),
                "layers": [
                    {
                        "name": layer.name,
                        "kind": layer.kind,
                        "path": _relative(layer.path) if layer.path else None,
                        "sha256": layer.sha256,
                    }
                    for layer in layers
                ],
                "assignments": assignments,
                "overrides": [{"key": key, "from": old, "to": new} for key, old, new in overrides],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(f"Native config: {_relative(defaults)} ({identity[:12]})")
    for key, old, new in overrides:
        print(f"Native config override: {key} {old} -> {new}")
    return ResolvedNativeConfig(
        platform=platform,
        chip=chip,
        format=config_format,
        defaults=defaults,
        manifest=manifest,
        identity=identity,
        assignments=tuple(assignments.items()),
        layers=tuple(layers),
        overrides=tuple(overrides),
        sdk_log_level=effective_sdk_log_level,
        changed=previous_identity != identity,
    )
