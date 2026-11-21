"""Check the Berry driver files a personal environment builds in with `solidify`."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from newmota.model import ROOT, BuildError

DRIVER_DIR = ROOT / "berry/drivers"
SOLIDIFY_MARK = re.compile(r"^#@\s*solidify:(\S+)\s*$", re.MULTILINE)
REQUIRES_MARK = re.compile(r"^#@\s*requires:(\S+)\s*$", re.MULTILINE)
CLASS_LINE = re.compile(r"class\s+([A-Za-z_]\w*)(?:\s*:\s*([A-Za-z_]\w*))?\s*$")
FEATURE_NAME = re.compile(r"USE_[A-Z0-9_]+")
# Globals the firmware sets with be_setglobal. The solidify pass declares them, because the
# host compiler refuses names it has not seen; built-in classes may not take them.
FIRMWARE_GLOBALS = (
    "Driver",
    "Wire",
    "display",
    "draw",
    "energy",
    "font",
    "img",
    "led_strip",
    "light",
    "load",
    "log",
    "matter",
    "newmota",
    "pixmat",
    "serial",
    "tasmota",
    "wire1",
    "wire2",
)


@dataclass(frozen=True)
class DriverFile:
    """One `solidify` entry: its marked classes, their bases and the features it needs."""

    relative: str
    classes: tuple[str, ...]
    bases: dict[str, str | None]
    requires: tuple[str, ...]

    @property
    def path(self) -> Path:
        return ROOT / self.relative


def solidify_list(value: object, source: str) -> tuple[str, ...]:
    """Return the checked `solidify` entries: repository-relative `.be` files, each once."""

    if value is None:
        return ()
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise BuildError(f"{source}: solidify must be a list of file paths")
    seen: set[str] = set()
    for item in value:
        path = (ROOT / item).resolve()
        if not path.is_relative_to(ROOT.resolve()):
            raise BuildError(f"{source}: solidify file {item} is outside the repository")
        if path.suffix != ".be" or not path.is_file():
            raise BuildError(f"{source}: solidify file {item} is not an existing .be file")
        if item in seen:
            raise BuildError(f"{source}: solidify lists {item} twice")
        seen.add(item)
    return tuple(value)


def read_driver_file(relative: str) -> DriverFile:
    """Parse one driver file; only classes and comments may stand at its top level."""

    text = (ROOT / relative).read_text(encoding="utf-8")
    bases: dict[str, str | None] = {}
    inside = False
    for number, line in enumerate(text.splitlines(), 1):
        if inside:
            inside = line != "end"
            continue
        if not line.strip() or line.startswith("#"):
            continue
        match = CLASS_LINE.fullmatch(line)
        if match is None:
            raise BuildError(
                f"{relative}:{number}: only classes may stand at the top level of a solidified "
                "file; move the code into a method or into autoexec.be"
            )
        bases[match.group(1)] = match.group(2)
        inside = True
    classes: list[str] = []
    # A mark names one class; a second part is the solidifier's `weak` flag.
    for mark in SOLIDIFY_MARK.findall(text):
        name = mark.split(",")[0]
        if name not in bases:
            raise BuildError(f"{relative}: #@ solidify:{name} names no class of this file")
        classes.append(name)
    if not classes:
        raise BuildError(f"{relative}: no #@ solidify:<Class> line marks a class to build in")
    unmarked = [name for name in bases if name not in classes]
    if unmarked:
        raise BuildError(
            f"{relative}: class {', '.join(unmarked)} is not marked; add #@ solidify:{unmarked[0]}"
        )
    return DriverFile(relative, tuple(classes), bases, file_requires(relative, text))


def file_requires(relative: str, text: str) -> tuple[str, ...]:
    """Return the features the `#@ requires:` lines of a Berry file name."""

    requires: list[str] = []
    for names in REQUIRES_MARK.findall(text):
        for name in names.split(","):
            if not FEATURE_NAME.fullmatch(name):
                raise BuildError(f"{relative}: #@ requires:{name} is not a USE_ feature name")
            requires.append(name)
    return tuple(requires)


def _defining_file(name: str) -> str | None:
    """Name the driver file in berry/drivers/ that declares class @p name, if one does."""

    pattern = re.compile(rf"^class\s+{re.escape(name)}\b", re.MULTILINE)
    for path in sorted(DRIVER_DIR.rglob("*.be")):
        if pattern.search(path.read_text(encoding="utf-8")):
            return str(path.relative_to(ROOT))
    return None


def _embedded_classes() -> set[str]:
    """Return the classes the firmware's own solidified sources build in."""

    from newmota.berry import SOLIDIFIED_SOURCES

    return {
        name
        for sources in SOLIDIFIED_SOURCES.values()
        for source in sources
        for names in SOLIDIFY_MARK.findall(source.read_text(encoding="utf-8"))
        for name in names.split(",")[:1]
    }


def check_solidify(entries: tuple[str, ...], values: dict[str, bool]) -> tuple[DriverFile, ...]:
    """Check a `solidify` list against the build's features; return the parsed files."""

    if not entries:
        return ()
    if not values.get("USE_BERRY", False):
        raise BuildError("solidify requires USE_BERRY")
    files = tuple(read_driver_file(entry) for entry in entries)
    reserved = set(FIRMWARE_GLOBALS) | _embedded_classes()
    defined: dict[str, str] = {}
    later = {name: item.relative for item in files for name in item.classes}
    for item in files:
        missing = [name for name in item.requires if not values.get(name, False)]
        if missing:
            raise BuildError(f"{item.relative} requires {', '.join(missing)}")
        for name in item.classes:
            if name in reserved:
                raise BuildError(f"{item.relative}: class {name} takes a firmware name")
            if name in defined:
                raise BuildError(f"{item.relative}: class {name} is also in {defined[name]}")
            base = item.bases[name]
            if base is not None and base not in defined:
                if base in reserved:
                    raise BuildError(
                        f"{item.relative}: class {name} extends the firmware class {base}; "
                        "a built-in driver class can only extend another solidify class"
                    )
                if base in later:
                    raise BuildError(
                        f"{item.relative}: class {name} extends {base}; list {later[base]} "
                        f"before {item.relative} in solidify"
                    )
                source = _defining_file(base)
                hint = f"; add {source} before it" if source else ""
                raise BuildError(
                    f"{item.relative}: class {name} extends {base}, which no earlier solidify "
                    f"file defines{hint}"
                )
            defined[name] = item.relative
    return files
