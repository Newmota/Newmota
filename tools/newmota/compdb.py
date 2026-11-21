"""Compilation-database recording, normalization, and activation."""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any

from newmota.host import cross_build_environment
from newmota.model import ROOT, BuildError

CLANGD_DIR = ROOT / "build" / "clangd"
ACTIVE_DATABASE = ROOT / "compile_commands.json"
PROJECT_SOURCE = ROOT / "src"
PROJECT_INCLUDE_ARGUMENTS = (
    f"-I{ROOT / 'src' / 'app'}",
    f"-I{ROOT / 'src' / 'app' / 'include'}",
    f"-I{ROOT / 'src' / 'platform' / 'include'}",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
CPP_SUFFIXES = {".cc", ".cpp", ".cxx"}
UNSUPPORTED_GCC_ARGUMENTS = {
    "-fno-shrink-wrap",
    "-fno-tree-switch-conversion",
    "-fstrict-volatile-bitfields",
    "-mthumb-interwork",
}


def recorder_command(log: Path, compiler: str, arguments: list[str] | None = None) -> str:
    """Return a host-shell command which records one real compiler invocation."""

    command = [
        sys.executable,
        str(ROOT / "tools" / "record_compile.py"),
        str(log),
        compiler,
        "--",
        *(arguments or []),
    ]
    if os.name == "nt":
        return subprocess.list2cmdline(command)
    return shlex.join(command)


def _load_database(path: Path) -> list[dict[str, Any]]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BuildError(f"cannot read compilation database {path}: {error}") from error
    if not isinstance(data, list) or not all(isinstance(entry, dict) for entry in data):
        raise BuildError(f"compilation database is not a JSON array of objects: {path}")
    return data


def _canonical_file(entry: dict[str, Any]) -> Path:
    file = entry.get("file")
    directory = entry.get("directory")
    if not isinstance(file, str) or not isinstance(directory, str):
        raise BuildError("compilation database entry has no string file or directory")
    path = Path(file)
    if not path.is_absolute():
        path = Path(directory) / path
    return path.resolve()


def _entry_arguments(entry: dict[str, Any]) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(argument, str) for argument in arguments):
        return list(arguments)
    command = entry.get("command")
    if not isinstance(command, str):
        raise BuildError("compilation database entry has neither arguments nor command")
    return shlex.split(command, posix=os.name != "nt")


def _expand_response_files(arguments: list[str], directory: Path) -> list[str]:
    expanded: list[str] = []
    for argument in arguments:
        if not argument.startswith("@"):
            expanded.append(argument)
            continue
        response = Path(argument[1:])
        if not response.is_absolute():
            response = directory / response
        try:
            content = response.read_text(encoding="utf-8")
        except OSError:
            expanded.append(argument)
            continue
        expanded.extend(shlex.split(content, posix=os.name != "nt"))
    return expanded


def _compiler(arguments: list[str]) -> tuple[int, str]:
    if not arguments:
        raise BuildError("compilation database entry has an empty command")
    index = 0
    if Path(arguments[0]).name in {"ccache", "sccache"}:
        index = 1
    if index >= len(arguments):
        raise BuildError("compiler launcher has no compiler argument")
    return index, arguments[index]


def _system_includes(compiler: str, language: str) -> tuple[str, ...]:
    try:
        result = subprocess.run(
            [compiler, "-E", "-x", language, "-", "-v"],
            input="",
            text=True,
            capture_output=True,
            check=False,
            env=cross_build_environment(),
        )
    except OSError as error:
        raise BuildError(f"cannot query cross compiler {compiler}: {error}") from error
    if result.returncode != 0:
        raise BuildError(f"cannot query cross compiler includes: {compiler}")

    includes: list[str] = []
    collecting = False
    for line in result.stderr.splitlines():
        stripped = line.strip()
        if stripped == "#include <...> search starts here:":
            collecting = True
            continue
        if collecting and stripped == "End of search list.":
            break
        if collecting and line[:1].isspace():
            suffix = " (framework directory)"
            path = stripped.removesuffix(suffix)
            if path and path not in includes:
                includes.append(path)
    if not includes:
        raise BuildError(f"cross compiler reported no {language} system includes: {compiler}")
    return tuple(includes)


def _clangd_arguments(
    entry: dict[str, Any], file: Path, include_cache: dict[tuple[str, str], tuple[str, ...]]
) -> list[str]:
    directory = Path(str(entry["directory"]))
    arguments = _expand_response_files(_entry_arguments(entry), directory)
    compiler_index, compiler = _compiler(arguments)
    compiler_name = Path(compiler).name
    is_riscv = "riscv" in compiler_name
    is_xtensa = "xtensa" in compiler_name
    saw_riscv32 = False
    compatible: list[str] = []
    for argument in arguments:
        if argument in UNSUPPORTED_GCC_ARGUMENTS:
            continue
        if is_riscv and argument.startswith("-march=rv"):
            saw_riscv32 = argument.startswith("-march=rv32")
            continue
        if is_riscv and argument.startswith("-mabi="):
            continue
        if is_xtensa and argument.startswith("-m"):
            continue
        compatible.append(argument)

    compiler_index, compiler = _compiler(compatible)
    language = "c++" if file.suffix.lower() in CPP_SUFFIXES else "c"
    key = (compiler, language)
    if key not in include_cache:
        include_cache[key] = _system_includes(compiler, language)
    additions = [item for path in include_cache[key] for item in ("-isystem", path)]
    if is_riscv and saw_riscv32 and "riscv64" in compiler_name:
        additions.insert(0, "--target=riscv32-unknown-elf")
    if is_xtensa:
        additions[:0] = [
            "-D__XTENSA__=1",
            "-D__XTENSA_EL__=1",
            "-D__XTENSA_WINDOWED_ABI__=1",
        ]
    compatible[compiler_index + 1 : compiler_index + 1] = additions
    return compatible


def write_recorded_database(log: Path, destination: Path, source_map: dict[Path, Path]) -> Path:
    """Convert compiler-wrapper records from staged sources to repository sources."""

    normalized_map = {staged.resolve(): source.resolve() for staged, source in source_map.items()}
    entries: dict[Path, dict[str, Any]] = {}
    try:
        lines = log.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise BuildError(f"cannot read compiler recording {log}: {error}") from error
    for number, line in enumerate(lines, 1):
        try:
            entry = json.loads(line)
        except json.JSONDecodeError as error:
            raise BuildError(f"invalid compiler recording {log}:{number}: {error}") from error
        if not isinstance(entry, dict):
            raise BuildError(f"invalid compiler recording {log}:{number}: expected object")
        staged = _canonical_file(entry)
        canonical = normalized_map.get(staged)
        if canonical is None:
            continue
        arguments = entry.get("arguments")
        if not isinstance(arguments, list) or not all(
            isinstance(argument, str) for argument in arguments
        ):
            raise BuildError(f"compiler recording {log}:{number} has invalid arguments")
        rewritten = [
            str(canonical) if argument == str(staged) else argument for argument in arguments
        ]
        rewritten[1:1] = PROJECT_INCLUDE_ARGUMENTS
        entries[canonical] = {
            "directory": entry["directory"],
            "arguments": rewritten,
            "file": str(canonical),
        }
    if not entries:
        raise BuildError(f"compiler recording contains no Newmota source commands: {log}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(list(entries.values()), indent=2) + "\n", encoding="utf-8")
    return destination


def activate_database(environment: str, source: Path) -> Path:
    """Filter one native database and make it the repository's active clangd target."""

    selected: dict[Path, dict[str, Any]] = {}
    include_cache: dict[tuple[str, str], tuple[str, ...]] = {}
    for entry in _load_database(source):
        file = _canonical_file(entry)
        if not file.is_relative_to(PROJECT_SOURCE) or file.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        normalized = {
            "directory": entry["directory"],
            "arguments": _clangd_arguments(entry, file, include_cache),
            "file": str(file),
        }
        selected[file] = normalized
    if not selected:
        raise BuildError(f"compilation database contains no project sources: {source}")

    content = json.dumps([selected[path] for path in sorted(selected)], indent=2) + "\n"
    destination = CLANGD_DIR / environment / "compile_commands.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(content, encoding="utf-8")

    temporary = ACTIVE_DATABASE.with_suffix(".json.tmp")
    temporary.write_text(content, encoding="utf-8")
    os.replace(temporary, ACTIVE_DATABASE)
    print(f"clangd database: {destination.relative_to(ROOT)} ({len(selected)} sources)")
    print(f"clangd active environment: {environment}")
    return destination
