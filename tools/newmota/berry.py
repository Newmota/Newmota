"""Generate environment-specific Berry build inputs."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

from newmota.berry_drivers import FIRMWARE_GLOBALS, check_solidify
from newmota.host import copy_if_changed, run, write_if_changed
from newmota.model import ROOT, BuildContext, BuildError

BERRY_ROOT = ROOT / "src" / "third_party" / "berry"
BERRY_SOURCE = BERRY_ROOT / "src"
BERRY_CONFIG = BERRY_ROOT / "default" / "berry_conf.h"
BERRY_NEWMOTA_CONFIG = BERRY_ROOT / "default" / "newmota_conf.h"
BERRY_COC = BERRY_ROOT / "tools" / "coc" / "coc"
BERRY_PORT = BERRY_ROOT / "berry_port"
CRYPTO_MODULE = ROOT / "src/app/services/berry/modules/crypto"
SOLIDIFY_DRIVER = ROOT / "tools/newmota/berry_solidify.be"
# Each source becomes solidified_<stem>.h, included by a C file of the owning module.
SOLIDIFIED_SOURCES = {
    "USE_BERRY_CRYPTO_SPAKE2P_MATTER": (CRYPTO_MODULE / "embedded/crypto_spake2p_matter.be",),
    "USE_MATTER": tuple(
        ROOT / "src/app/services/matter/berry/embedded" / f"matter_{name}.be"
        for name in ("base38", "certs", "tlv", "secure", "im", "model", "device")
    ),
    "USE_MATTER_BRIDGE": (ROOT / "src/app/services/matter/berry/embedded/matter_bridge.be",),
    "USE_MATTER_ENERGY": (ROOT / "src/app/services/matter/berry/embedded/matter_energy.be",),
    "USE_MATTER_TCP": (ROOT / "src/app/services/matter/berry/embedded/matter_client.be",),
    "USE_MATTER_CAMERA": (ROOT / "src/app/services/matter/berry/embedded/matter_camera.be",),
    "USE_BERRY_DRIVERS": (
        ROOT / "src/app/services/berry/modules/newmota/embedded/driver_class.be",
    ),
    "USE_DISPLAY_DRAW": (ROOT / "src/app/services/berry/modules/display/embedded/draw_class.be",),
}
LVGL_INPUTS = tuple(
    ROOT / "src/third_party" / name
    for name in (
        "berry_mapping/src",
        "berry_ctypes",
        "lv_binding_berry/src",
        "lv_binding_berry/src/solidify",
        "lv_binding_berry/generate",
    )
)
CALLBACK_INPUT = ROOT / "src/third_party/berry_mapping/src"
LVGL_MODULE = ROOT / "src/app/services/berry/modules/lvgl"
LVGL_CONFIG = ROOT / "src/third_party/lv_binding_berry/src/lv_binding_conf.h"
REQUIRED_OUTPUTS = (
    "be_const_bytes.h",
    "be_const_bytes_def.h",
    "be_const_strtab.h",
    "be_const_strtab_def.h",
    "be_fixed_m_builtin.h",
)


# Sources that extend a class of another source, which has to run first.
SOLIDIFY_BASES = {
    "matter_bridge": (ROOT / "src/app/services/matter/berry/embedded/matter_model.be",),
    "matter_energy": (ROOT / "src/app/services/matter/berry/embedded/matter_model.be",),
    "matter_camera": (ROOT / "src/app/services/matter/berry/embedded/matter_model.be",),
}


def _solidified_outputs(context: BuildContext, directory: Path) -> dict[Path, Path]:
    """Map each Berry source selected by the build to its generated header."""

    return {
        source: directory / f"solidified_{source.stem}.h"
        for feature, sources in SOLIDIFIED_SOURCES.items()
        if context.features.enabled(feature)
        for source in sources
    }


def _input_files(context: BuildContext) -> tuple[Path, ...]:
    """Return every source-controlled input covered by the Berry cache key."""

    files = [
        Path(__file__),
        context.features.manifest,
        BERRY_CONFIG,
        BERRY_NEWMOTA_CONFIG,
        BERRY_COC,
    ]
    files.extend(sorted(BERRY_SOURCE.glob("*.[ch]")))
    files.extend(sorted(BERRY_PORT.glob("*.py")))
    files.extend(sorted((BERRY_ROOT / "tools" / "coc").glob("*.py")))
    files.extend(
        sorted(
            path
            for path in CRYPTO_MODULE.iterdir()
            if path.is_file() and path.suffix in {".h", ".c", ".cc", ".cpp"}
        )
    )
    files.append(SOLIDIFY_DRIVER)
    for sources in SOLIDIFIED_SOURCES.values():
        files.extend(sources)
    files.extend(ROOT / entry for entry in context.environment.solidify)
    if context.features.enabled("USE_BLE") and not context.features.enabled("USE_LVGL"):
        files.extend(
            sorted(
                path
                for path in CALLBACK_INPUT.iterdir()
                if path.is_file() and path.suffix in {".c", ".h"}
            )
        )
    if context.features.enabled("USE_LVGL"):
        files.append(LVGL_MODULE / "embedded/lv_widgets.be")
        for directory in LVGL_INPUTS:
            files.extend(
                sorted(
                    path
                    for path in directory.iterdir()
                    if path.is_file() and path.suffix in {".c", ".h"}
                )
            )
    return tuple(files)


def _driver_outputs(context: BuildContext, directory: Path) -> dict[Path, Path]:
    """Map each `solidify` driver file, in list order, to its generated header."""

    return {
        ROOT / entry: directory / f"solidified_driver_{index}.h"
        for index, entry in enumerate(context.environment.solidify)
    }


def _solidify_drivers(context: BuildContext, directory: Path) -> Path:
    """Solidify the environment's driver classes and write the header that registers them."""

    files = check_solidify(context.environment.solidify, dict(context.features.assignments))
    outputs = _driver_outputs(context, directory)
    earlier: list[str] = []
    for item, output in zip(files, outputs.values(), strict=True):
        try:
            run(
                [
                    sys.executable,
                    "-m",
                    "berry_port",
                    "-s",
                    "-g",
                    str(SOLIDIFY_DRIVER),
                    str(item.path),
                    str(output),
                    "--globals=" + ",".join(FIRMWARE_GLOBALS),
                    *earlier,
                ],
                cwd=BERRY_ROOT,
            )
        except subprocess.CalledProcessError as error:
            raise BuildError(
                f"solidify failed for {item.relative}; a class it uses from another driver file "
                "must come from an earlier solidify entry"
            ) from error
        earlier.append(str(item.path))
    classes = [name for item in files for name in item.classes]
    lines = [
        "/* Generated by tools/newmota/berry.py from the environment's solidify list. */",
        *(f'#include "{output.name}"' for output in outputs.values()),
        "#define NEWMOTA_SOLIDIFIED_DRIVERS(X) " + " ".join(f"X({name})" for name in classes),
        "",
    ]
    header = directory / "solidified_drivers.h"
    write_if_changed(header, "\n".join(lines))
    return header


def _identity(context: BuildContext) -> str:
    """Hash Berry runtime, generator, configuration, and resolved features."""

    digest = hashlib.sha256()
    for path in _input_files(context):
        digest.update(str(path.relative_to(ROOT)).encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def _write_definitions(context: BuildContext, directory: Path) -> Path:
    """Translate the resolved feature map into Berry preprocessor definitions."""

    lines = [
        "# Generated by tools/newmota/berry.py; do not edit.",
        "import preproc",
        "",
    ]
    lines.extend(
        f"preproc.define('{name}', {'true' if enabled else 'false'})"
        for name, enabled in context.features.assignments
    )
    lines.append("")
    destination = directory / "newmota_defines_for_berry.be"
    write_if_changed(destination, "\n".join(lines))
    return destination


def generate_berry(context: BuildContext) -> None:
    """Generate Berry constants for normal firmware or skip Safeboot entirely."""

    if not context.features.enabled("USE_BERRY"):
        print("Berry pre-build: skipped (feature disabled)")
        return

    for path in (
        BERRY_SOURCE,
        BERRY_CONFIG,
        BERRY_NEWMOTA_CONFIG,
        BERRY_COC,
        BERRY_PORT,
    ):
        if not path.exists():
            raise BuildError(f"Berry input is missing: {path.relative_to(ROOT)}")

    directory = context.work_dir / "generated" / "berry"
    directory.mkdir(parents=True, exist_ok=True)
    definitions = _write_definitions(context, directory)
    identity = _identity(context)
    cache_path = directory / "generation.json"
    solidified = _solidified_outputs(context, directory)
    drivers = _driver_outputs(context, directory)
    expected = (
        tuple(directory / name for name in REQUIRED_OUTPUTS)
        + tuple(solidified.values())
        + tuple(drivers.values())
    )
    if drivers:
        expected += (directory / "solidified_drivers.h",)
    if context.features.enabled("USE_LVGL"):
        expected += (directory / "lvgl_widgets.h",)
    if cache_path.is_file() and all(path.is_file() for path in expected):
        try:
            cached = json.loads(cache_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            cached = {}
        if cached.get("identity") == identity:
            print("Berry pre-build: generated constants are current")
            return

    for previous in directory.glob("*.h"):
        previous.unlink()
    # Before the constant generator, which collects the strings these headers name.
    for source, output in solidified.items():
        run(
            [
                sys.executable,
                "-m",
                "berry_port",
                "-s",
                "-g",
                str(SOLIDIFY_DRIVER),
                str(source),
                str(output),
                *(str(path) for path in SOLIDIFY_BASES.get(source.stem, ())),
            ],
            cwd=BERRY_ROOT,
        )
    if drivers:
        _solidify_drivers(context, directory)
    run(
        [
            sys.executable,
            str(BERRY_COC),
            "-o",
            str(directory),
            str(directory),
            str(BERRY_SOURCE),
            str(CRYPTO_MODULE),
            *(str(path) for path in LVGL_INPUTS if context.features.enabled("USE_LVGL")),
            *(
                [str(CALLBACK_INPUT)]
                if context.features.enabled("USE_BLE") and not context.features.enabled("USE_LVGL")
                else []
            ),
            "-c",
            str(BERRY_CONFIG),
            str(BERRY_NEWMOTA_CONFIG),
            str(context.features.header),
            *([str(LVGL_CONFIG)] if context.features.enabled("USE_LVGL") else []),
        ]
    )
    if context.features.enabled("USE_LVGL"):
        source = (LVGL_MODULE / "embedded/lv_widgets.be").read_text()
        write_if_changed(
            directory / "lvgl_widgets.h",
            'static const char newmota_lvgl_widgets[] = R"newmota(' + source + ')newmota";\n',
        )
    missing = [path for path in expected if not path.is_file()]
    if missing:
        raise BuildError(
            "Berry generator did not produce: "
            + ", ".join(str(path.relative_to(ROOT)) for path in missing)
        )
    cache = {
        "schema": 1,
        "identity": identity,
        "definitions": str(definitions.relative_to(ROOT)),
        "outputs": sorted(path.name for path in directory.glob("*.h") if path.is_file()),
    }
    cache_path.write_text(json.dumps(cache, indent=2) + "\n", encoding="utf-8")
    print(f"Berry pre-build: generated {len(cache['outputs'])} constant headers")


def stage_berry_generated_headers(context: BuildContext, destination: Path) -> None:
    """Copy generated Berry C headers into a flat-make SDK build overlay."""

    if not context.features.enabled("USE_BERRY"):
        return
    source = context.work_dir / "generated" / "berry"
    headers = sorted(source.glob("*.h"))
    if not headers:
        raise BuildError("Berry generated headers are missing from the pre-build output")
    destination.mkdir(parents=True, exist_ok=True)
    for header in headers:
        copy_if_changed(header, destination / header.name)


def stage_berry_runtime(
    context: BuildContext, source_directory: Path, include_directory: Path
) -> dict[Path, Path]:
    """Stage Berry sources and headers for a recursive-make SDK."""

    berry_source_directory = source_directory / "berry"
    berry_include_root = include_directory / "berry"
    module_source_directory = source_directory / "modules"
    if not context.features.enabled("USE_BERRY"):
        shutil.rmtree(berry_source_directory, ignore_errors=True)
        shutil.rmtree(module_source_directory, ignore_errors=True)
        shutil.rmtree(berry_include_root, ignore_errors=True)
        shutil.rmtree(include_directory / "services" / "berry", ignore_errors=True)
        for name in (
            "autoexec.cpp",
            "berry_service.cpp",
            "berry_port.cpp",
            "berry_module_table.c",
            "newmota_module.cpp",
        ):
            path = source_directory / name
            path.unlink(missing_ok=True)
            path.with_suffix(".o").unlink(missing_ok=True)
        return {}

    staged = {}
    for relative in context.features.sources:
        source = ROOT / relative
        if "berry" not in source.parts:
            continue
        if source.parent == BERRY_SOURCE:
            destination = berry_source_directory / source.name
        elif source.is_relative_to(ROOT / "src/app/services") and (
            source.relative_to(ROOT / "src/app/services").parts[1:2] == ("berry",)
        ):
            owner_relative = source.relative_to(ROOT / "src/app/services")
            destination = module_source_directory / owner_relative.parts[0]
            for part in owner_relative.parts[2:]:
                destination /= part
        elif "modules" in source.relative_to(ROOT / "src/app/services/berry").parts:
            destination = source_directory / source.relative_to(ROOT / "src/app/services/berry")
        else:
            destination = source_directory / source.name
        staged[source] = destination
    for header in sorted((ROOT / "src/app/services/berry").glob("*.h")):
        staged[header] = include_directory / "services/berry" / header.name
    # Remove retired runtime sources without discarding unchanged objects or headers.
    for previous in berry_source_directory.glob("*.c"):
        if previous not in staged.values():
            previous.unlink()
            previous.with_suffix(".o").unlink(missing_ok=True)
    for previous in module_source_directory.rglob("*"):
        if (
            previous.is_file()
            and previous.suffix in {".c", ".cpp"}
            and previous not in staged.values()
        ):
            previous.unlink()
            previous.with_suffix(".o").unlink(missing_ok=True)
            previous.with_suffix(".d").unlink(missing_ok=True)
    for source, destination in staged.items():
        destination.parent.mkdir(parents=True, exist_ok=True)
        copy_if_changed(source, destination)

    berry_source_directory.mkdir(parents=True, exist_ok=True)
    berry_include_directory = berry_include_root / "src"
    berry_include_directory.mkdir(parents=True, exist_ok=True)
    for source in sorted(BERRY_SOURCE.glob("*.h")):
        copy_if_changed(source, berry_include_directory / source.name)
    berry_config_directory = berry_include_root / "default"
    berry_config_directory.mkdir(parents=True, exist_ok=True)
    for source in (BERRY_CONFIG, BERRY_NEWMOTA_CONFIG):
        copy_if_changed(source, berry_config_directory / source.name)
    return staged
