"""Execute one compiler command and append it to a JSON-lines recording."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}


def main() -> int:
    if len(sys.argv) < 4:
        print("usage: record_compile.py <log> <compiler> -- <arguments...>", file=sys.stderr)
        return 2

    log = Path(sys.argv[1])
    compiler = sys.argv[2]
    arguments = sys.argv[3:]
    if arguments and arguments[0] == "--":
        arguments = arguments[1:]

    result = subprocess.run([compiler, *arguments], check=False)
    if result.returncode != 0:
        return result.returncode

    if "-c" not in arguments or "-M" in arguments or "-MM" in arguments:
        return 0
    source = next(
        (argument for argument in arguments if Path(argument).suffix.lower() in SOURCE_SUFFIXES),
        None,
    )
    if source is None:
        return 0

    directory = Path.cwd()
    source_path = Path(source)
    if not source_path.is_absolute():
        source_path = directory / source_path
    record = {
        "directory": str(directory),
        "arguments": [compiler, *arguments],
        "file": str(source_path.resolve()),
    }
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("a", encoding="utf-8") as output:
        output.write(json.dumps(record, separators=(",", ":")) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
