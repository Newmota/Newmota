"""Format tracked, first-party C and C++ sources with the pinned clang-format."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("src/app/", "src/platform/", "tools/tests/")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
CLANG_FORMAT_VERSION = "22.1.8"


def tracked_sources() -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    paths = (entry.decode("utf-8") for entry in result.stdout.split(b"\0") if entry)
    return sorted(
        path
        for path in paths
        if path.startswith(SOURCE_ROOTS) and Path(path).suffix.lower() in SOURCE_SUFFIXES
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="format files in place")
    mode.add_argument("--check", action="store_true", help="fail if formatting changes are needed")
    args = parser.parse_args()

    formatter = shutil.which("clang-format")
    if formatter is None:
        print(
            "clang-format is unavailable; install it with 'uv sync --extra dev'",
            file=sys.stderr,
        )
        return 2

    version = subprocess.run(
        [formatter, "--version"],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    version_text = (version.stdout + version.stderr).strip()
    if version.returncode != 0 or f"version {CLANG_FORMAT_VERSION}" not in version_text:
        print(
            f"expected clang-format {CLANG_FORMAT_VERSION}, found {version_text or 'unknown'}",
            file=sys.stderr,
        )
        return 2

    sources = tracked_sources()
    if not sources:
        print("no tracked first-party C/C++ sources found", file=sys.stderr)
        return 2

    file_list: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="\n", suffix=".txt", delete=False
        ) as output:
            file_list = Path(output.name)
            output.write("\n".join(sources))

        command = [formatter, "--style=file", "--fallback-style=none"]
        command.extend(("--dry-run", "--Werror") if args.check else ("-i",))
        command.append(f"--files={file_list}")
        result = subprocess.run(command, cwd=ROOT, check=False)
        if result.returncode == 0:
            action = "checked" if args.check else "formatted"
            print(
                f"{action} {len(sources)} first-party C/C++ files "
                f"with clang-format {CLANG_FORMAT_VERSION}"
            )
        return result.returncode
    finally:
        if file_list is not None:
            file_list.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
