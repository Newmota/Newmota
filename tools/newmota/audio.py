"""Select the carried Tasmota codec cores and isolate their build configuration."""

from __future__ import annotations

from pathlib import Path

from newmota.host import copy_if_changed, copy_tree
from newmota.model import ROOT, BuildContext

AUDIO_ROOT = ROOT / "src/third_party/tasmota_audio"


def codec_sources(values: dict[str, bool]) -> tuple[str, ...]:
    """Return only selected portable cores; hardware transport has no codec dependency."""

    sources: list[Path] = []
    if values.get("USE_AUDIO_MP3", False):
        sources.extend(
            p for p in sorted((AUDIO_ROOT / "libmad").glob("*.c")) if p.name != "decoder.c"
        )
        sources.extend(
            p for p in sorted((AUDIO_ROOT / "shine").glob("*.cpp")) if p.name != "layer3.cpp"
        )
        sources.append(ROOT / "src/app/drivers/audio/audio_shine_port.cpp")
    if values.get("USE_AUDIO_AAC", False):
        sources.extend(sorted((AUDIO_ROOT / "libhelix-aac").glob("*.c")))
    if values.get("USE_AUDIO_OPUS", False):
        for directory in ("libopus", "libopus/celt", "libopus/silk", "libopus/silk/fixed"):
            sources.extend(sorted((AUDIO_ROOT / directory).glob("*.c")))
        sources.append(ROOT / "src/app/drivers/audio/audio_opus.cpp")
    if values.get("USE_AUDIO_SAM", False):
        sources.extend(sorted((AUDIO_ROOT / "sam").glob("*.c")))
        sources.append(ROOT / "src/app/drivers/audio/audio_sam.cpp")
    return tuple(str(p.relative_to(ROOT / "src")) for p in sources)


def codec_settings(source: Path) -> tuple[tuple[str, ...], tuple[Path, ...]]:
    """Confine compatibility defines and shim headers to audio translation units."""

    relative = source.relative_to(ROOT).as_posix()
    includes = (
        AUDIO_ROOT,
        AUDIO_ROOT / "compat",
        AUDIO_ROOT / "libopus",
        AUDIO_ROOT / "libopus/celt",
        AUDIO_ROOT / "libopus/silk",
        AUDIO_ROOT / "libopus/silk/fixed",
        AUDIO_ROOT / "shine",
    )
    if source.name == "audio_shine_port.cpp":
        includes = (AUDIO_ROOT / "compat/shine", *includes)
    if "/tasmota_audio/sam/" in relative or source.name == "audio_sam.cpp":
        includes = (*includes, AUDIO_ROOT / "sam")
    defines: tuple[str, ...] = ()
    if "/libhelix-aac/" in relative:
        defines = ("ARDUINO=1",)
    elif "/libmad/" in relative or "/libopus/" in relative:
        defines = ("HAVE_CONFIG_H=1",)
    return defines, includes


def cmake_codec_settings(sources: tuple[str, ...]) -> list[str]:
    """Emit per-source CMake settings without exposing Arduino defines to the SDK."""

    lines: list[str] = []
    for relative in sources:
        if "/tasmota_audio/" not in relative and "/drivers/audio/audio_" not in relative:
            continue
        source = ROOT / relative
        defines, includes = codec_settings(source)
        quoted = f'"${{NEWMOTA_ROOT}}/{relative}"'
        lines.append(f"set_property(SOURCE {quoted} APPEND PROPERTY INCLUDE_DIRECTORIES")
        lines.extend(f'  "${{NEWMOTA_ROOT}}/{p.relative_to(ROOT)}"' for p in includes)
        lines.append(")")
        if defines:
            lines.append(
                f"set_property(SOURCE {quoted} APPEND PROPERTY COMPILE_DEFINITIONS {' '.join(defines)})"
            )
        if "/tasmota_audio/" in relative or source.name == "audio_shine_port.cpp":
            lines.append(
                f"set_property(SOURCE {quoted} APPEND PROPERTY COMPILE_OPTIONS -fno-strict-aliasing -Wno-error=all -Wno-error=extra -Wno-error=misleading-indentation)"
            )
        if "/libopus/" in relative:
            # GCC 11+ diagnoses SILK's pointer into the first row of a two-row
            # coefficient array as a smaller object. Keep the carried core and
            # its warning visible without promoting this diagnostic to an error.
            lines.append(
                'if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 11)'
            )
            lines.append(
                f"set_property(SOURCE {quoted} APPEND PROPERTY COMPILE_OPTIONS -Wno-error=stringop-overread)"
            )
            lines.append("endif()")
    return lines


def stage_audio_runtime(context: BuildContext, source_dir: Path) -> dict[Path, Path]:
    """Preserve codec relative includes and unique object names in Ameba's flat build."""

    if not any(
        context.features.enabled(name)
        for name in ("USE_AUDIO_MP3", "USE_AUDIO_AAC", "USE_AUDIO_OPUS", "USE_AUDIO_SAM")
    ):
        return {}
    destination_root = source_dir / "tasmota_audio"
    copy_tree(AUDIO_ROOT, destination_root)
    staged: dict[Path, Path] = {}
    for relative in context.features.sources:
        source = ROOT / relative
        if not source.is_relative_to(AUDIO_ROOT):
            continue
        local = source.relative_to(AUDIO_ROOT)
        name = "audio_" + "_".join(local.with_suffix("").parts) + source.suffix
        destination = destination_root / local.parent / name
        copy_if_changed(source, destination)
        staged[source] = destination
    return staged


def ameba_codec_settings(sources: dict[Path, Path], source_dir: Path) -> list[str]:
    """Apply codec defines/includes to selected objects in the vendor make module."""

    lines: list[str] = []
    for original, destination in sources.items():
        if original.suffix not in (".c", ".cpp"):
            continue
        if not original.is_relative_to(AUDIO_ROOT) and "/drivers/audio/audio_" not in str(original):
            continue
        defines, includes = codec_settings(original)
        staged_includes = [
            "-I$(DIR)/tasmota_audio/" + str(p.relative_to(AUDIO_ROOT)) for p in includes
        ]
        flags = " ".join([*("-D" + d for d in defines), *staged_includes])
        if original.is_relative_to(AUDIO_ROOT) or original.name == "audio_shine_port.cpp":
            flags += " -fno-strict-aliasing -Wno-error=all -Wno-error=extra -Wno-error=misleading-indentation"
        for variable in ("GLOBAL_CFLAGS", "CPPFLAGS"):
            lines.append(f"{destination.stem}.o: {variable} += {flags}")
    return lines
