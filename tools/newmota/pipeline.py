"""Shared build selection, fixed lifecycle runner, and artifact manifest."""

from __future__ import annotations

import json
import os
import time
from datetime import datetime, timezone
from pathlib import Path

from newmota.catalog import (
    load_environment_matrix,
    load_environments,
    load_user_environments,
    project_version,
    recovery_for_environment,
    resolve_board_environment,
)
from newmota.compdb import activate_database
from newmota.config import BuildConfigError, resolve_native_config, resolve_sdk_log_level
from newmota.features import resolve_features
from newmota.hardware import resolve_hardware_catalog
from newmota.host import check_sdk_version, run, setup_python, sha256, source_state
from newmota.i18n import locale_identity
from newmota.matter_qr import print_setup_codes
from newmota.model import (
    ROOT,
    WORK_DIR,
    ApplicationUpdate,
    Artifact,
    BuildContext,
    BuildError,
    Environment,
    PlatformAdapter,
)
from newmota.platforms import platform_for_build
from newmota.progress import format_elapsed

PHASES = (
    "resolve",
    "prepare",
    "configure",
    "pre-build",
    "build",
    "post-build",
    "package",
)


def select_environments(
    requested: list[str],
    build_all: bool,
    chip_filter: str | None,
    matrix: str | None = None,
    boards: list[str] | None = None,
) -> list[Environment]:
    available = load_environments()
    board_names = boards or []
    selectors = sum((bool(requested), build_all, matrix is not None, bool(board_names)))
    if selectors > 1:
        raise BuildError("select environments, --board, --matrix, or --all; do not combine them")
    if board_names:
        selected = [resolve_board_environment(name) for name in board_names]
        names = []
    elif matrix is not None:
        names = load_environment_matrix(matrix)
    elif build_all:
        names = list(available)
    else:
        names = requested
    if requested:
        available.update(load_user_environments(available))
    if not board_names:
        if not names:
            raise BuildError("select an environment, --board, --matrix, or --all")
        unknown = [name for name in names if name not in available]
        if unknown:
            raise BuildError(
                f"unknown environment {', '.join(unknown)}; available: {', '.join(available)}"
            )
        selected = [available[name] for name in names]
    if chip_filter:
        selected = [item for item in selected if chip_filter in item.chip]
        if not selected:
            raise BuildError(f"no environment matches chip '{chip_filter}'")
    return selected


def ensure_setup(environments: list[Environment]) -> None:
    targets = sorted(
        {
            f"{item.platform}:{item.idf_version}:{item.sdk_target}"
            if item.platform == "esp"
            else f"{item.platform}:{item.sdk_target}"
            for item in environments
        }
    )
    run(
        [
            setup_python(),
            "-m",
            "newmota",
            "setup",
            "--ensure",
            "--target",
            *targets,
        ],
        environment={**os.environ, "PYTHONPATH": str(ROOT / "tools")},
    )


def _verify_artifacts(item: Environment, artifacts: list[Artifact]) -> None:
    missing = [artifact.path for artifact in artifacts if not artifact.path.is_file()]
    if missing:
        raise BuildError(
            f"{item.name} did not produce: "
            + ", ".join(str(path.relative_to(ROOT)) for path in missing)
        )


def _write_manifest(context: BuildContext) -> Path:
    item = context.environment
    _verify_artifacts(item, context.artifacts)
    manifest = {
        "schema": 1,
        "product": "newmota",
        "version": context.version,
        "build_datetime": context.build_datetime,
        "source": source_state(),
        "target": {
            "environment": item.name,
            "board": item.board,
            "chip": item.chip,
            "platform": item.platform,
            "profile": item.profile,
            "language": item.language,
            "locale": locale_identity(item.language),
            "flash_bytes": item.flash_kb * 1024,
            "psram_bytes": item.psram_kb * 1024 if item.psram_kb is not None else None,
            "console": item.console,
            "sdk_target": item.sdk_target,
            "idf_version": item.idf_version,
            "sdk_board": item.sdk_board,
            "role": item.role,
            "safeboot": item.safeboot,
            "build_status": item.build_status,
            "capabilities": item.capabilities,
        },
        "sdk": {
            "key": context.sdk.key,
            "name": context.sdk.name,
            "version": context.sdk.version,
            "revision": context.sdk.revision,
        },
        "configuration": {
            "environment": item.configuration_provenance(),
            "identity": context.native_config.identity,
            "format": context.native_config.format,
            "sdk_log_level": context.native_config.sdk_log_level,
            "defaults": str(context.native_config.defaults.relative_to(ROOT)),
            "manifest": str(context.native_config.manifest.relative_to(ROOT)),
            "features": {
                "identity": context.features.identity,
                "file": str(context.features.manifest.relative_to(ROOT)),
                "sha256": sha256(context.features.manifest),
                "values": dict(context.features.assignments),
            },
            "hardware": {
                "identity": context.hardware.identity,
                "file": str(context.hardware.manifest.relative_to(ROOT)),
                "sha256": sha256(context.hardware.manifest),
                "source_revision": context.hardware.source_revision,
            },
            "partition": (
                {
                    "file": str(item.partition.relative_to(ROOT)),
                    "sha256": sha256(item.partition),
                }
                if item.partition
                else None
            ),
            "layout": (
                {
                    "identity": context.layout.identity,
                    "file": str(context.layout.manifest.relative_to(ROOT)),
                    "sha256": sha256(context.layout.manifest),
                }
                if context.layout is not None
                else None
            ),
        },
        "pipeline": {
            "phases": list(PHASES),
            "package_status": context.package.status,
        },
        "flashable": context.package.flashable,
        "deployable": context.package.deployable,
        "artifacts": [
            {
                "role": artifact.role,
                "path": str(artifact.path.relative_to(ROOT)),
                "size": artifact.path.stat().st_size,
                "sha256": sha256(artifact.path),
            }
            for artifact in context.artifacts
        ],
    }
    destination = context.work_dir / "artifacts.json"
    destination.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return destination


def _phase(name: str) -> None:
    print(f"\n--- {name} ---", flush=True)


def _build_datetime() -> str:
    """Return one build timestamp in the StatusFWR-compatible format."""

    source_date_epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if source_date_epoch is None:
        value = datetime.now().astimezone()
    else:
        try:
            value = datetime.fromtimestamp(int(source_date_epoch), tz=timezone.utc)
        except (OSError, OverflowError, ValueError) as error:
            raise BuildError("SOURCE_DATE_EPOCH must be a valid Unix timestamp") from error
    return value.strftime("%Y.%m.%d %H:%M:%S")


def effective_build_jobs(requested: int | None) -> int:
    """Use all logical CPUs available to this process unless the user overrides it."""

    if requested is not None:
        if requested < 1:
            raise BuildError("JOBS must be a positive integer")
        return requested
    cpu_count = getattr(os, "process_cpu_count", os.cpu_count)()
    return max(1, cpu_count or os.cpu_count() or 1)


def _resolve_adapter(
    item: Environment, version: str, jobs: int, build_datetime: str
) -> PlatformAdapter:
    if item.build_status != "native":
        raise BuildError(
            f"{item.name} is cataloged as planned; {item.platform}/{item.sdk_target} "
            "has no native SDK adapter yet"
        )
    definition = platform_for_build(item.platform)
    sdk = check_sdk_version(item.sdk_key)
    work_dir = WORK_DIR / item.name
    layout = definition.adapter.resolve_layout(item, sdk, work_dir)
    features = resolve_features(item, work_dir, layout)
    hardware = resolve_hardware_catalog(item, features, work_dir)
    try:
        sdk_log_level = definition.adapter.effective_sdk_log_level(
            item, resolve_sdk_log_level(default=definition.adapter.default_sdk_log_level)
        )
        native_config = resolve_native_config(
            environment=item.name,
            platform=item.platform,
            config_directory=definition.adapter.native_config.directory,
            config_format=definition.adapter.native_config.filename,
            chip=item.chip,
            board_requirements=definition.adapter.board_native_settings(
                item, f"board '{item.board}'", item.partition
            ),
            role=item.role,
            profile=item.profile,
            version=version,
            native=item.native,
            work_dir=work_dir,
            feature_requirements=features.native_settings,
            sdk_log_level=sdk_log_level,
            sdk_log_settings=definition.adapter.sdk_log_settings(sdk_log_level),
            build_inputs={
                "environment_configuration": item.configuration_provenance(),
                "sdk_revision": sdk.revision,
                "sdk_pin": sha256(ROOT / "config/sdk" / f"{item.sdk_key}.toml"),
                "sdk_log_level": sdk_log_level,
                "features": features.identity,
                "hardware": hardware.identity,
                "layout": layout.identity if layout is not None else None,
                "language": locale_identity(item.language),
                "tooling": {
                    str(path.relative_to(ROOT)): sha256(path)
                    for path in definition.build_input_paths()
                },
                "partition": sha256(item.partition) if item.partition else None,
            },
        )
    except BuildConfigError as error:
        raise BuildError(str(error)) from error

    context = BuildContext(
        item,
        version,
        sdk,
        native_config,
        features,
        hardware,
        layout,
        work_dir,
        jobs,
        build_datetime,
    )
    return definition.adapter(context)


def build_environment(
    item: Environment,
    version: str,
    jobs: int,
    build_datetime: str | None = None,
) -> None:
    started = time.monotonic()
    print(f"\n=== Building {item.name} ({item.platform}/{item.chip}) ===", flush=True)
    _phase("resolve")
    adapter = _resolve_adapter(item, version, jobs, build_datetime or _build_datetime())
    context = adapter.context

    _phase("prepare")
    adapter.prepare()
    _phase("configure")
    adapter.configure()
    _phase("pre-build")
    adapter.pre_build()
    _phase("build")
    adapter.build()
    _phase("post-build")
    context.artifacts.extend(adapter.post_build())
    _phase("package")
    context.package = adapter.package()
    context.artifacts.extend(context.package.artifacts)
    manifest = _write_manifest(context)
    print(
        f"Artifacts: {manifest.relative_to(ROOT)} (elapsed {format_elapsed(time.monotonic() - started)})",
        flush=True,
    )
    print_setup_codes(context)


def build_application_update(item: Environment, jobs: int | None) -> ApplicationUpdate:
    """Build only the normal application image needed by a wired update."""

    jobs = effective_build_jobs(jobs)
    if item.role != "application":
        raise BuildError("application-only update requires an application environment")
    if item.build_status != "native":
        raise BuildError(
            f"planned target {item.board} ({item.platform}/{item.sdk_target}) has no native adapter"
        )
    ensure_setup([item])
    started = time.monotonic()
    print(
        f"\n=== Building application update {item.name} ({item.platform}/{item.chip}) ===",
        flush=True,
    )
    _phase("resolve")
    adapter = _resolve_adapter(item, project_version(), jobs, _build_datetime())
    _phase("prepare")
    adapter.prepare()
    _phase("configure")
    adapter.configure()
    _phase("pre-build")
    adapter.pre_build()
    _phase("build application")
    adapter.build_application()
    _phase("validate application")
    update = adapter.application_update()
    if not update.image.is_file():
        raise BuildError(f"application-only build did not produce {update.image.relative_to(ROOT)}")
    print(
        f"Application: {update.image.relative_to(ROOT)} "
        f"(elapsed {format_elapsed(time.monotonic() - started)})",
        flush=True,
    )
    return update


def configure_clangd(item: Environment, jobs: int | None) -> None:
    """Generate and activate compile commands for exactly one environment."""

    jobs = effective_build_jobs(jobs)
    ensure_setup([item])
    print(f"\n=== Configuring clangd for {item.name} ({item.platform}/{item.chip}) ===", flush=True)
    _phase("resolve")
    adapter = _resolve_adapter(item, project_version(), jobs, _build_datetime())
    _phase("prepare")
    adapter.prepare()
    _phase("configure")
    adapter.configure()
    _phase("pre-build")
    adapter.pre_build()
    _phase("compilation database")
    database = adapter.compilation_database()
    activate_database(item.name, database)


def build_selected(environments: list[Environment], jobs: int | None) -> None:
    jobs = effective_build_jobs(jobs)
    available = load_environments()
    ordered: list[Environment] = []
    added: set[str] = set()
    visiting: set[str] = set()

    def add_with_dependencies(item: Environment) -> None:
        if item.name in added:
            return
        if item.name in visiting:
            raise BuildError(f"cyclic environment dependency involving {item.name}")
        visiting.add(item.name)
        if item.safeboot is not None:
            recovery = recovery_for_environment(item, available)
            add_with_dependencies(recovery)
            if recovery.name != item.safeboot:
                from dataclasses import replace

                item = replace(item, safeboot=recovery.name)
        visiting.remove(item.name)
        added.add(item.name)
        ordered.append(item)

    for environment in environments:
        add_with_dependencies(environment)

    planned = [item for item in ordered if item.build_status != "native"]
    if planned:
        targets = ", ".join(f"{item.board} ({item.platform}/{item.sdk_target})" for item in planned)
        raise BuildError(f"planned targets have no native SDK adapter: {targets}")
    ensure_setup(ordered)
    version = project_version()
    build_datetime = _build_datetime()
    for item in ordered:
        build_environment(item, version, jobs, build_datetime)


def print_environments() -> None:
    environments = load_environments()
    environments.update(load_user_environments(environments))
    print(
        f"{'ENVIRONMENT':34} {'ROLE':11} {'CHIP':10} {'FLASH':7} {'PROFILE':10} PLATFORM SOURCE/BASE"
    )
    for item in environments.values():
        print(
            f"{item.name:34} {item.role:11} {item.chip:10} {item.flash_kb // 1024:>4} MB "
            f"{item.profile:10} {item.platform} {item.path.relative_to(ROOT) if item.path else 'board'}"
            f"{' inherits ' + item.base_environment if item.base_environment else ''}"
        )
