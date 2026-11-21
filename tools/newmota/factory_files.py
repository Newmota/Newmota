"""Check the files a personal environment ships in the factory filesystem with `filesystem`."""

from __future__ import annotations

from pathlib import PurePosixPath

from newmota.berry_drivers import file_requires
from newmota.model import ROOT, BuildError, FactoryFile

# The firmware's path buffer holds 192 bytes with the terminating null.
PATH_LIMIT = 191
# Firmware state lives in /.newmota; /sd is where the SD card mounts.
RESERVED_TARGETS = ("/.newmota", "/sd")
ENTRY_KEYS = {"source", "target"}


def _source(item: object, source: str) -> str:
    if not isinstance(item, str):
        raise BuildError(f"{source}: filesystem source must be a file path")
    path = (ROOT / item).resolve()
    if not path.is_relative_to(ROOT.resolve()):
        raise BuildError(f"{source}: filesystem file {item} is outside the repository")
    if not path.is_file():
        raise BuildError(f"{source}: filesystem file {item} is not an existing file")
    return item


def _target(item: object, source: str) -> str:
    if not isinstance(item, str) or not item.startswith("/"):
        raise BuildError(
            f"{source}: filesystem target {item} must be an absolute path such as /a.be"
        )
    parts = item[1:].split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise BuildError(f"{source}: filesystem target {item} must name a file without . or ..")
    if len(item.encode("utf-8")) > PATH_LIMIT:
        raise BuildError(f"{source}: filesystem target {item} is longer than {PATH_LIMIT} bytes")
    for reserved in RESERVED_TARGETS:
        if item == reserved or item.startswith(f"{reserved}/"):
            raise BuildError(f"{source}: filesystem target {item} is inside {reserved}")
    return item


def filesystem_list(value: object, source: str) -> tuple[FactoryFile, ...]:
    """Return the checked `filesystem` entries; a string lands at `/<basename>`."""

    if value is None:
        return ()
    if not isinstance(value, list):
        raise BuildError(f"{source}: filesystem must be a list of files")
    files: list[FactoryFile] = []
    targets: dict[str, str] = {}
    for item in value:
        if isinstance(item, str):
            entry = FactoryFile(_source(item, source), f"/{PurePosixPath(item).name}")
        elif isinstance(item, dict) and set(item) == ENTRY_KEYS:
            entry = FactoryFile(_source(item["source"], source), _target(item["target"], source))
        else:
            raise BuildError(
                f"{source}: filesystem entry {item} must be a file path or a mapping with "
                "exactly source and target"
            )
        if entry.target in targets:
            raise BuildError(
                f"{source}: filesystem puts {targets[entry.target]} and {entry.source} "
                f"at {entry.target}"
            )
        targets[entry.target] = entry.source
        files.append(entry)
    return tuple(files)


def check_filesystem(
    entries: tuple[FactoryFile, ...], solidify: tuple[str, ...], values: dict[str, bool]
) -> None:
    """Check the shipped files against the solidify list and the build's features."""

    built_in = {(ROOT / item).resolve() for item in solidify}
    for entry in entries:
        if (ROOT / entry.source).resolve() in built_in:
            raise BuildError(
                f"{entry.source} is in both solidify and filesystem; the shipped file would "
                "replace its built-in classes"
            )
        if entry.source.endswith(".be"):
            text = (ROOT / entry.source).read_text(encoding="utf-8")
            missing = [
                name for name in file_requires(entry.source, text) if not values.get(name, False)
            ]
            if missing:
                raise BuildError(f"{entry.source} requires {', '.join(missing)}")
