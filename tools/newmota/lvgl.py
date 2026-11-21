"""Source selection and flat-make SDK staging for the optional LVGL runtime."""

from __future__ import annotations

import shutil
from pathlib import Path

from newmota.host import copy_if_changed
from newmota.model import ROOT, BuildContext

THIRD_PARTY = ROOT / "src/third_party"
LVGL_ROOTS = ("lvgl", "lv_binding_berry", "berry_mapping", "berry_ctypes", "lvgl_assets")
LVGL_INCLUDE_DIRS = (
    "lvgl",
    "lvgl/src",
    "lv_binding_berry/src",
    "lv_binding_berry/generate",
    "berry_mapping/src",
    "berry_ctypes",
)


def is_lvgl_source(path: Path) -> bool:
    """Identify imported LVGL sources that retain their nested include layout."""
    return path.is_relative_to(THIRD_PARTY) and path.relative_to(THIRD_PARTY).parts[0] in LVGL_ROOTS


def lvgl_sources() -> tuple[str, ...]:
    """Return the matched core, binding, and generic callback/ctypes support."""
    sources = list((THIRD_PARTY / "lvgl/src").rglob("*.c"))
    for directory in (
        "lv_binding_berry/src",
        "lv_binding_berry/generate",
        "berry_mapping/src",
        "berry_ctypes",
        "lvgl_assets",
    ):
        sources.extend((THIRD_PARTY / directory).glob("*.c"))
    return tuple(str(path.relative_to(ROOT / "src")) for path in sorted(sources))


def stage_lvgl_runtime(context: BuildContext, source_directory: Path) -> dict[Path, Path]:
    """Preserve relative LVGL headers and sources in recursive-make overlays."""
    destination_root = source_directory / "lvgl"
    roots = (
        LVGL_ROOTS
        if context.features.enabled("USE_LVGL")
        else (("berry_mapping",) if context.features.enabled("USE_BLE") else ())
    )
    if not roots:
        shutil.rmtree(destination_root, ignore_errors=True)
        return {}
    staged = {}
    for relative in context.features.sources:
        source = ROOT / relative
        if is_lvgl_source(source):
            staged[source] = destination_root / source.relative_to(THIRD_PARTY)
    for name in roots:
        for header in (THIRD_PARTY / name).rglob("*.h"):
            staged[header] = destination_root / header.relative_to(THIRD_PARTY)
    selected = set(staged.values())
    for previous in destination_root.rglob("*.c"):
        if previous not in selected:
            previous.unlink()
            previous.with_suffix(".o").unlink(missing_ok=True)
            previous.with_suffix(".d").unlink(missing_ok=True)
    for source, destination in staged.items():
        destination.parent.mkdir(parents=True, exist_ok=True)
        copy_if_changed(source, destination)
    return staged
