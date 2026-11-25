"""Host processes, SDK pins, managed toolchains, and source staging (stdlib only)."""

from __future__ import annotations

import hashlib
import json
import os
import platform as host_platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
import zipfile
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from newmota.model import (
    ROOT,
    BuildContext,
    BuildError,
    SetupUnavailable,
)

CROSS_BUILD_VARIABLES = (
    "AR",
    "ARCHFLAGS",
    "AS",
    "ASFLAGS",
    "ASMFLAGS",
    "CC",
    "CFLAGS",
    "CMAKE_INCLUDE_PATH",
    "CMAKE_LIBRARY_PATH",
    "CMAKE_OSX_ARCHITECTURES",
    "CMAKE_OSX_DEPLOYMENT_TARGET",
    "CMAKE_PREFIX_PATH",
    "CMAKE_TOOLCHAIN_FILE",
    "COMPILER_PATH",
    "CPATH",
    "CPPFLAGS",
    "CPLUS_INCLUDE_PATH",
    "CXX",
    "CXXFLAGS",
    "C_INCLUDE_PATH",
    "GCC_EXEC_PREFIX",
    "INCLUDE",
    "LD",
    "LDFLAGS",
    "LIB",
    "LIBPATH",
    "LIBRARY_PATH",
    "MACOSX_DEPLOYMENT_TARGET",
    "OBJCFLAGS",
    "OBJCXXFLAGS",
    "OBJC_INCLUDE_PATH",
    "PKG_CONFIG_LIBDIR",
    "PKG_CONFIG_PATH",
    "PKG_CONFIG_SYSROOT_DIR",
    "SDKROOT",
)


def run(
    command: list[str],
    *,
    cwd: Path = ROOT,
    environment: dict[str, str] | None = None,
    capture: bool = False,
    dry_run: bool = False,
    timeout: float | None = None,
) -> subprocess.CompletedProcess[str]:
    print(f"$ {shlex.join(command)}", flush=True)
    if dry_run:
        return subprocess.CompletedProcess(command, 0, "", "")
    return subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        check=True,
        text=True,
        capture_output=capture,
        timeout=timeout,
    )


# tools/compile_status.sh prints this before each compiler invocation.
COMPILE_STATUS_PREFIX = "@@newmota-compile "
COMPILE_STATUS_WRAPPER = ROOT / "tools" / "compile_status.sh"
# Lines a logged build still shows on the terminal: compiler and linker diagnostics.
BUILD_DIAGNOSTIC = re.compile(
    r"\b(warning|error|fatal|note)\b|undefined reference|multiple definition|"
    r"region .* overflowed|\*\*\*",
    re.IGNORECASE,
)
FAILURE_TAIL_LINES = 40
# CMake Unix Makefiles rule message: "[ 45%] Building C object dir/CMakeFiles/x.dir/a.c.obj".
CMAKE_BUILDING = re.compile(r"^\[\s*(\d+)%\] Building \S+ object (\S+)")
CMAKE_TARGET_DIR = re.compile(r"CMakeFiles/[^/]+\.dir/")

# Maps one build output line to (progress tag or None, source path), or None.
StatusParser = Callable[[str], "tuple[str | None, str] | None"]


def wrapper_status(line: str) -> tuple[str | None, str] | None:
    """Recognize a source reported by tools/compile_status.sh."""

    if line.startswith(COMPILE_STATUS_PREFIX):
        return None, line[len(COMPILE_STATUS_PREFIX) :].strip()
    return None


def no_status(line: str) -> tuple[str | None, str] | None:
    """Recognize nothing; for logged steps that compile no sources."""

    return None


def cmake_status(line: str) -> tuple[str | None, str] | None:
    """Recognize a CMake Makefile rule message and recover the source path."""

    match = CMAKE_BUILDING.match(line)
    if match is None:
        return None
    source = CMAKE_TARGET_DIR.sub("", match[2]).removesuffix(".obj").removesuffix(".o")
    return f"{match[1]}%", source


def compile_status_compiler(compiler: str) -> str:
    """Return a CC value that reports each compiled source to run_build."""

    return f"sh {COMPILE_STATUS_WRAPPER} {compiler}"


def _status_text(tag: str, label: str, source: str, cwd: Path, width: int) -> str:
    path = Path(source)
    for base in (cwd, ROOT):
        try:
            source = str((cwd / path).resolve().relative_to(base.resolve()))
            break
        except ValueError:
            continue
    prefix = f"[{tag}] {label}: "
    room = max(width - 1 - len(prefix), 10)
    if len(source) > room:
        source = "..." + source[-(room - 3) :]
    return prefix + source


def run_build(
    command: list[str],
    *,
    label: str,
    log: Path,
    cwd: Path = ROOT,
    environment: dict[str, str] | None = None,
    status: StatusParser = wrapper_status,
) -> None:
    """Run a native build, redrawing one status line for the source being compiled.

    status recognizes the lines that name a compiled source. The complete output goes to
    log. The terminal shows the status line (only on a terminal), compiler and linker
    diagnostics, the final count, and on failure the log's last lines.
    """

    print(f"$ {shlex.join(command)}", flush=True)
    log.parent.mkdir(parents=True, exist_ok=True)
    recorded = log.open("w", encoding="utf-8")
    interactive = sys.stdout.isatty()
    width = shutil.get_terminal_size().columns
    count = 0
    status_shown = False
    process = subprocess.Popen(
        command,
        cwd=cwd,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        bufsize=1,
    )
    try:
        assert process.stdout is not None
        for line in process.stdout:
            parsed = status(line)
            if parsed is not None:
                count += 1
                if interactive:
                    tag, source = parsed
                    text = _status_text(tag or str(count), label, source, cwd, width)
                    sys.stdout.write(f"\r{text}\x1b[K")
                    sys.stdout.flush()
                    status_shown = True
                # Wrapper markers are not build output; CMake rule messages are logged.
                if status is not wrapper_status:
                    recorded.write(line)
                continue
            recorded.write(line)
            if not BUILD_DIAGNOSTIC.search(line):
                continue
            if status_shown:
                sys.stdout.write("\r\x1b[K")
                status_shown = False
            sys.stdout.write(line)
            sys.stdout.flush()
        returncode = process.wait()
    finally:
        recorded.close()
        if process.poll() is None:
            process.kill()
            process.wait()
        if status_shown:
            sys.stdout.write("\r\x1b[K")
    shown = log.relative_to(ROOT) if log.is_relative_to(ROOT) else log
    if returncode:
        tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-FAILURE_TAIL_LINES:]
        print(f"{label}: build failed; last lines of {shown}:", flush=True)
        print("\n".join(tail), flush=True)
        raise subprocess.CalledProcessError(returncode, command)
    if count:
        print(f"{label}: compiled {count} files (log: {shown})", flush=True)
    else:
        print(f"{label}: done (log: {shown})", flush=True)


def verbose_build_enabled() -> bool:
    """Return whether native SDK compiler commands should be shown."""

    value = os.environ.get("NEWMOTA_VERBOSE") or os.environ.get("VERBOSE", "")
    normalized = value.strip().lower()
    if normalized in {"", "0", "false", "no", "off"}:
        return False
    if normalized in {"1", "true", "yes", "on"}:
        return True
    raise BuildError(f"VERBOSE must be a boolean; received {value!r}")


def command_succeeds(
    command: list[str], *, cwd: Path = ROOT, environment: dict[str, str] | None = None
) -> bool:
    try:
        return (
            subprocess.run(
                command,
                cwd=cwd,
                env=environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            ).returncode
            == 0
        )
    except OSError:
        return False


def cross_build_environment(
    environment: dict[str, str] | None = None,
) -> dict[str, str]:
    isolated = (environment if environment is not None else os.environ).copy()
    for name in CROSS_BUILD_VARIABLES:
        isolated.pop(name, None)
    return isolated


def find_ccache() -> str | None:
    program = shutil.which("ccache")
    if program:
        program = str(Path(program).resolve())
        print(f"Compiler cache: {program}")
    else:
        print("Compiler cache: disabled (ccache not found)")
    return program


def _python_version(program: str) -> tuple[int, int] | None:
    try:
        result = subprocess.run(
            [program, "-S", "-c", "import sys; print('%d.%d' % sys.version_info[:2])"],
            check=True,
            capture_output=True,
            text=True,
        )
        major, minor = result.stdout.strip().split(".")
        return int(major), int(minor)
    except (OSError, subprocess.SubprocessError, ValueError):
        return None


def _base_python_candidates() -> tuple[Path, ...]:
    base = Path(sys.base_prefix)
    if os.name == "nt":
        return (base / "python.exe", base / "python3.exe")
    bindir = base / "bin"
    versioned = bindir / f"python{sys.version_info.major}.{sys.version_info.minor}"
    return (versioned, bindir / "python3", bindir / "python")


def _versioned_python_names(search_path: str, *, windows: bool | None = None) -> tuple[str, ...]:
    if windows is None:
        windows = os.name == "nt"
    suffix = ".exe" if windows else ""
    minors: set[int] = set()
    for entry in search_path.split(os.pathsep):
        if not entry:
            continue
        try:
            candidates = Path(entry).iterdir()
            for candidate in candidates:
                match = re.fullmatch(r"python3\.(\d+)(\.exe)?", candidate.name, re.IGNORECASE)
                candidate_suffix = (match.group(2) or "").lower() if match else ""
                if match and candidate_suffix == suffix and int(match.group(1)) >= 10:
                    minors.add(int(match.group(1)))
        except OSError:
            continue
    return tuple(f"python3.{minor}{suffix}" for minor in sorted(minors, reverse=True))


def setup_python() -> str:
    override = os.environ.get("NEWMOTA_SETUP_PYTHON")
    if override:
        program = shutil.which(override)
        if program is None:
            raise OSError(f"NEWMOTA_SETUP_PYTHON does not exist or is not executable: {override}")
        version = _python_version(program)
        if version is None:
            raise OSError(f"NEWMOTA_SETUP_PYTHON is not a runnable Python interpreter: {override}")
        if version < (3, 10):
            raise OSError(
                f"NEWMOTA_SETUP_PYTHON must use Python 3.10 or newer; {override} is "
                f"Python {version[0]}.{version[1]}"
            )
        return str(Path(program).resolve())

    excluded: set[Path] = set()
    active_environment = os.environ.get("VIRTUAL_ENV")
    if active_environment:
        excluded.add(Path(active_environment).expanduser().resolve())
    if sys.prefix != sys.base_prefix:
        excluded.add(Path(sys.prefix).resolve())

    search_path = os.pathsep.join(
        entry
        for entry in os.environ.get("PATH", "").split(os.pathsep)
        if entry
        and not any(
            Path(entry).expanduser().resolve()
            == environment / ("Scripts" if os.name == "nt" else "bin")
            for environment in excluded
        )
    )
    names = (
        ("python.exe", "python3.exe", "python", *_versioned_python_names(search_path))
        if os.name == "nt"
        else ("python3", "python", *_versioned_python_names(search_path))
    )
    checked: set[Path] = set()
    for name in names:
        program = shutil.which(name, path=search_path)
        if program is None:
            continue
        resolved = Path(program).resolve()
        if resolved in checked:
            continue
        checked.add(resolved)
        version = _python_version(str(resolved))
        if version is not None and version >= (3, 10):
            return str(resolved)

    for candidate in _base_python_candidates():
        program = shutil.which(str(candidate))
        if program is None:
            continue
        resolved = Path(program).resolve()
        if resolved in checked:
            continue
        checked.add(resolved)
        version = _python_version(str(resolved))
        if version is not None and version >= (3, 10):
            return str(resolved)

    raise OSError(
        "no host Python 3.10 or newer found outside the active virtual environment; "
        "set NEWMOTA_SETUP_PYTHON or make SDK_PYTHON=/path/to/python"
    )


def write_if_changed(path: Path, content: str, encoding: str = "utf-8") -> None:
    """Preserve timestamps of identical generated inputs."""

    if path.is_file() and path.read_text(encoding=encoding) == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding=encoding)


def load_toml(path: Path) -> dict:
    """Load build-time TOML without making Python 3.10 SDK setup depend on tomli."""
    try:
        import tomllib
    except ModuleNotFoundError:  # pragma: no cover - Python 3.10 compatibility
        import tomli as tomllib

    return tomllib.loads(path.read_text(encoding="utf-8"))


def copy_if_changed(source: Path, destination: Path) -> str:
    """Copy changed bytes, preserving unchanged destination timestamps."""

    source, destination = Path(source), Path(destination)
    if not destination.is_file() or source.read_bytes() != destination.read_bytes():
        destination.parent.mkdir(parents=True, exist_ok=True)
        # A changed checkout may carry older mtimes: changed bytes must look new to make.
        shutil.copyfile(source, destination)
        shutil.copymode(source, destination)
    return str(destination)


def copy_tree(source: Path, destination: Path) -> None:
    """Stage a header tree without touching identical files."""

    shutil.copytree(source, destination, dirs_exist_ok=True, copy_function=copy_if_changed)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_state() -> dict[str, str | bool]:
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    ).stdout.strip()
    dirty = bool(
        subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=all"],
            cwd=ROOT,
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()
    )
    return {"revision": revision, "dirty": dirty}


SDK_SOURCE_DIR = ROOT / ".sdk"
SDK_CONFIG_DIR = ROOT / "config" / "sdk"
SDK_MODES = ("checkout", "archive")
SDK_ARCHIVE_STAMP = ".newmota-sdk.json"
SDK_NAMES = ("esp-idf", "esp-idf-v6.1", "bouffalo", "beken", "amebad", "pico-sdk")
STRING_VALUE = re.compile(r'^([a-z0-9_]+)\s*=\s*"([^"\\]*)"\s*$')
REVISION = re.compile(r"[0-9a-f]{40}")


class SdkVersionError(Exception):
    pass


@dataclass(frozen=True)
class SdkVersion:
    key: str
    name: str
    version: str
    revision: str
    programmer: str
    programmer_version: str
    programmer_status: str
    programmer_sha256: str | None
    source_name: str | None
    archive: str | None = None
    archive_sha256: str | None = None

    @property
    def checkout(self) -> Path:
        """The submodule checkout under .sdk/, used for SDK patch work."""
        return SDK_SOURCE_DIR / (self.source_name or self.key)

    @property
    def archive_dir(self) -> Path | None:
        """The extracted release archive in the SDK cache, keyed by its checksum."""
        if self.archive_sha256 is None:
            return None
        return sdk_cache_root() / self.key / self.archive_sha256[:16]

    @property
    def mode(self) -> str:
        """checkout when .sdk/<source> is initialized or no archive is pinned, else archive.

        NEWMOTA_SDK_MODE=checkout|archive forces one mode.
        """
        forced = os.environ.get("NEWMOTA_SDK_MODE", "").strip()
        if forced:
            if forced not in SDK_MODES:
                raise SdkVersionError(
                    f"NEWMOTA_SDK_MODE must be checkout or archive, not '{forced}'"
                )
            if forced == "archive" and self.archive is None:
                raise SdkVersionError(f"NEWMOTA_SDK_MODE=archive, but {self.key} pins no archive")
            return forced
        if self.archive is None or (self.checkout / ".git").exists():
            return "checkout"
        return "archive"

    @property
    def source(self) -> Path:
        if self.mode == "archive":
            assert self.archive_dir is not None
            return self.archive_dir
        return self.checkout


def sdk_cache_root() -> Path:
    """Extracted SDK archives; NEWMOTA_SDK_PATH overrides ~/.newmota/sdk."""
    configured = os.environ.get("NEWMOTA_SDK_PATH", str(Path.home() / ".newmota" / "sdk"))
    return Path(configured).expanduser()


def sdk_download_root() -> Path:
    """Downloaded SDK archives, kept beside the SDK cache for offline re-extraction."""
    return sdk_cache_root().parent / "downloads"


def display_path(path: Path) -> str:
    """A repository-relative path when inside the checkout, otherwise the absolute path."""
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def load_sdk_version(key: str) -> SdkVersion:
    if key not in SDK_NAMES:
        raise SdkVersionError(f"unknown SDK '{key}'")

    path = SDK_CONFIG_DIR / f"{key}.toml"
    values: dict[str, str] = {}
    for line_number, original in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = original.strip()
        if not line or line.startswith("#"):
            continue
        match = STRING_VALUE.fullmatch(line)
        if not match:
            raise SdkVersionError(f"invalid {path.relative_to(ROOT)}:{line_number}")
        field, value = match.groups()
        if field in values:
            raise SdkVersionError(f"duplicate '{field}' in {path.relative_to(ROOT)}")
        values[field] = value

    required = {
        "name",
        "version",
        "revision",
        "programmer",
        "programmer_version",
        "programmer_status",
    }
    optional = {"programmer_sha256", "source", "archive", "archive_sha256"}
    missing = required - values.keys()
    extra = values.keys() - required - optional
    if missing:
        raise SdkVersionError(f"{path.relative_to(ROOT)} is missing: {', '.join(sorted(missing))}")
    if extra:
        raise SdkVersionError(
            f"{path.relative_to(ROOT)} has unknown fields: {', '.join(sorted(extra))}"
        )
    if not REVISION.fullmatch(values["revision"]):
        raise SdkVersionError(
            f"{path.relative_to(ROOT)} revision must be a full lowercase commit hash"
        )
    status = values["programmer_status"]
    if status not in {"active", "unselected"}:
        raise SdkVersionError(
            f"{path.relative_to(ROOT)} programmer_status must be active or unselected"
        )
    programmer_sha256 = values.get("programmer_sha256")
    if programmer_sha256 is not None and not re.fullmatch(r"[0-9a-f]{64}", programmer_sha256):
        raise SdkVersionError(
            f"{path.relative_to(ROOT)} programmer_sha256 must be a lowercase SHA-256"
        )
    archive = values.get("archive")
    archive_sha256 = values.get("archive_sha256")
    if (archive is None) != (archive_sha256 is None):
        raise SdkVersionError(
            f"{path.relative_to(ROOT)} archive and archive_sha256 must be set together"
        )
    if archive is not None:
        if not archive.startswith("https://") or not archive.endswith(".tar.xz"):
            raise SdkVersionError(f"{path.relative_to(ROOT)} archive must be an https .tar.xz URL")
        if not re.fullmatch(r"[0-9a-f]{64}", archive_sha256 or ""):
            raise SdkVersionError(
                f"{path.relative_to(ROOT)} archive_sha256 must be a lowercase SHA-256"
            )
    if status == "unselected" and (
        values["programmer"] != "unselected" or values["programmer_version"] != "unselected"
    ):
        raise SdkVersionError(
            f"{path.relative_to(ROOT)} unselected programmer fields must say 'unselected'"
        )
    return SdkVersion(
        key,
        values["name"],
        values["version"],
        values["revision"],
        values["programmer"],
        values["programmer_version"],
        status,
        programmer_sha256,
        values.get("source"),
        archive,
        archive_sha256,
    )


def _archive_stamp(sdk: SdkVersion) -> dict[str, str]:
    return {
        "key": sdk.key,
        "revision": sdk.revision,
        "archive": sdk.archive or "",
        "archive_sha256": sdk.archive_sha256 or "",
    }


def checked_out_revision(sdk: SdkVersion) -> str:
    if sdk.mode == "archive":
        stamp = sdk.source / SDK_ARCHIVE_STAMP
        try:
            recorded = json.loads(stamp.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise SdkVersionError(
                f"{display_path(sdk.source)} is not installed; run make setup for its platform"
            ) from error
        if recorded.get("archive_sha256") != sdk.archive_sha256:
            raise SdkVersionError(f"{display_path(stamp)} does not match the pinned archive")
        return str(recorded.get("revision", ""))
    try:
        return subprocess.run(
            ["git", "-C", str(sdk.source), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise SdkVersionError(
            f"{display_path(sdk.source)} is not initialized; run make setup for its platform"
        ) from error


def check_sdk_version(key: str) -> SdkVersion:
    sdk = load_sdk_version(key)
    actual = checked_out_revision(sdk)
    if actual != sdk.revision:
        raise SdkVersionError(
            f"{display_path(sdk.source)} revision mismatch: expected {sdk.revision}, got {actual}"
        )
    return sdk


def check_sdk_versions(keys: tuple[str, ...] = SDK_NAMES) -> list[SdkVersion]:
    return [check_sdk_version(key) for key in keys]


def ensure_sdk_source(key: str, dry_run: bool) -> SdkVersion:
    """Install the pinned SDK source: the .sdk/ submodule or the checksummed release archive."""
    sdk = load_sdk_version(key)
    print(f"SDK mode:          {sdk.mode}")
    if sdk.mode == "checkout":
        run(
            ["git", "submodule", "update", "--init", "--recursive", display_path(sdk.checkout)],
            dry_run=dry_run,
        )
    else:
        install_sdk_archive(sdk, dry_run)
    if not dry_run:
        check_sdk_version(key)
    return sdk


def _require_lzma() -> None:
    try:
        import lzma  # noqa: F401
    except ImportError as error:
        raise SetupUnavailable(
            f"{sys.executable} was built without the lzma module, which SDK archives "
            "(.tar.xz) need; use a Python built with liblzma, for example one installed by uv"
        ) from error


def _make_read_only(root: Path, *, include_root: bool = True) -> None:
    for directory, _, files in os.walk(root):
        for name in files:
            path = Path(directory) / name
            if not path.is_symlink():
                path.chmod(path.stat().st_mode & ~0o222)
        if include_root or Path(directory) != root:
            Path(directory).chmod(Path(directory).stat().st_mode & ~0o222)


def install_sdk_archive(sdk: SdkVersion, dry_run: bool) -> Path:
    """Download, verify, and extract an SDK release archive into the read-only SDK cache."""
    assert sdk.archive is not None and sdk.archive_sha256 is not None
    destination = sdk.archive_dir
    assert destination is not None
    if (destination / SDK_ARCHIVE_STAMP).is_file():
        return destination
    if destination.exists():
        raise SetupUnavailable(f"incomplete SDK directory exists: {destination}")
    archive = sdk_download_root() / Path(urllib.parse.urlparse(sdk.archive).path).name
    print(f"SDK archive:       {sdk.archive}")
    print(f"SDK destination:   {destination}")
    if dry_run:
        if not archive.is_file():
            print(f"$ download {sdk.archive} -> {archive}")
        print(f"$ verify sha256 {sdk.archive_sha256}")
        print(f"$ extract -> {destination}")
        return destination

    _require_lzma()
    if not archive.is_file() or _file_sha256(archive) != sdk.archive_sha256:
        archive.parent.mkdir(parents=True, exist_ok=True)
        partial = archive.with_name(archive.name + ".part")
        _download(sdk.archive, partial)
        actual_hash = _file_sha256(partial)
        if actual_hash != sdk.archive_sha256:
            partial.unlink()
            raise SetupUnavailable(
                f"SDK archive checksum mismatch: expected {sdk.archive_sha256}, got {actual_hash}"
            )
        partial.replace(archive)

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".newmota-sdk-", dir=destination.parent) as temporary:
        extracted = Path(temporary) / "extracted"
        extracted.mkdir()
        _extract_archive(archive, extracted)
        contents = list(extracted.iterdir())
        if len(contents) != 1 or not contents[0].is_dir():
            raise SetupUnavailable("SDK archive must contain exactly one top-level directory")
        root = contents[0]
        (root / SDK_ARCHIVE_STAMP).write_text(
            json.dumps(_archive_stamp(sdk), indent=2) + "\n", encoding="utf-8"
        )
        # A read-only directory cannot be renamed (macOS), so the root follows the rename.
        _make_read_only(root, include_root=False)
        root.rename(destination)
    destination.chmod(destination.stat().st_mode & ~0o222)
    return destination


def sdk_main(keys: list[str], *, as_json: bool = False) -> int:
    """Report verified SDK pins without importing any platform implementation."""
    try:
        versions = check_sdk_versions(tuple(keys) if keys else SDK_NAMES)
        if as_json:
            document = {
                sdk.key: {
                    "name": sdk.name,
                    "version": sdk.version,
                    "revision": sdk.revision,
                    "mode": sdk.mode,
                    "source": str(sdk.source),
                    "programmer": {
                        "name": sdk.programmer,
                        "version": sdk.programmer_version,
                        "status": sdk.programmer_status,
                        "sha256": sdk.programmer_sha256,
                    },
                }
                for sdk in versions
            }
            print(json.dumps(document, indent=2))
        else:
            for sdk in versions:
                print(f"{sdk.name}: {sdk.version} ({sdk.revision[:12]}, {sdk.mode})")
                print(
                    f"  programmer: {sdk.programmer} {sdk.programmer_version} "
                    f"[{sdk.programmer_status}]"
                )
        return 0
    except (OSError, SdkVersionError) as error:
        print(f"SDK version error: {error}", file=sys.stderr)
        return 1


ARM_NONE_EABI_VERSION = "14.2.1-1.1"
ARM_NONE_EABI_RELEASE = (
    "https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/"
    f"v{ARM_NONE_EABI_VERSION}"
)
ARM_NONE_EABI_ARCHIVES = {
    "darwin-arm64": (
        f"xpack-arm-none-eabi-gcc-{ARM_NONE_EABI_VERSION}-darwin-arm64.tar.gz",
        "f52ea3760c53b25d726a7345be60a210736293db85f92daa39d1d22d34e2c995",
    ),
    "darwin-x64": (
        f"xpack-arm-none-eabi-gcc-{ARM_NONE_EABI_VERSION}-darwin-x64.tar.gz",
        "b5bf8d5af099fd464d1543e5b8901308fb64116fa7a244426cacf4ff1b882fc7",
    ),
    "linux-arm64": (
        f"xpack-arm-none-eabi-gcc-{ARM_NONE_EABI_VERSION}-linux-arm64.tar.gz",
        "a1ac95c8d9347020d61e387e644a2c1806556b77162958a494d2f5f3d5fe7053",
    ),
    "linux-x64": (
        f"xpack-arm-none-eabi-gcc-{ARM_NONE_EABI_VERSION}-linux-x64.tar.gz",
        "ed8c7d207a85d00da22b90cf80ab3b0b2c7600509afadf6b7149644e9d4790a6",
    ),
    "win32-x64": (
        f"xpack-arm-none-eabi-gcc-{ARM_NONE_EABI_VERSION}-win32-x64.zip",
        "0b2d496b383ba578182eb57b3f7d35ff510e36eda56257883b902fa07c3bba55",
    ),
}


def host_key() -> str:
    systems = {"Darwin": "darwin", "Linux": "linux", "Windows": "win32"}
    machines = {
        "x86_64": "x64",
        "amd64": "x64",
        "aarch64": "arm64",
        "arm64": "arm64",
    }
    system = systems.get(host_platform.system())
    machine = machines.get(host_platform.machine().lower())
    if not system or not machine:
        raise SetupUnavailable(
            f"unsupported setup host {host_platform.system()}-{host_platform.machine()}"
        )
    return f"{system}-{machine}"


def tools_root() -> Path:
    configured = os.environ.get("NEWMOTA_TOOLS_PATH", str(Path.home() / ".newmota" / "tools"))
    return Path(configured).expanduser()


def arm_none_eabi_toolchain() -> Path:
    """Return the shared managed Arm bare-metal toolchain location."""

    return tools_root() / "arm-none-eabi" / f"xpack-{ARM_NONE_EABI_VERSION}"


def tool_path(root: Path, prefix: str, name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return root / "bin" / f"{prefix}{name}{suffix}"


def compiler_path(root: Path, prefix: str) -> Path:
    return tool_path(root, prefix, "gcc")


def verify_toolchain(root: Path, prefix: str) -> None:
    for name in ("gcc", "g++"):
        tool = tool_path(root, prefix, name)
        if not tool.is_file():
            raise SetupUnavailable(f"toolchain executable is missing: {tool}")
        subprocess.run(
            [str(tool), "--version"],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


def _safe_archive_path(root: Path, name: str, relative_to: Path | None = None) -> None:
    base = relative_to or root
    target = (base / name).resolve()
    try:
        target.relative_to(root.resolve())
    except ValueError as error:
        raise SetupUnavailable(f"unsafe path in toolchain archive: {name}") from error


def _extract_archive(archive: Path, destination: Path) -> None:
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as bundle:
            for member in bundle.infolist():
                _safe_archive_path(destination, member.filename)
            bundle.extractall(destination)
        return

    with tarfile.open(archive, "r:*") as bundle:
        for member in bundle.getmembers():
            _safe_archive_path(destination, member.name)
            if member.issym():
                _safe_archive_path(destination, member.linkname, (destination / member.name).parent)
            elif member.islnk():
                _safe_archive_path(destination, member.linkname)
        bundle.extractall(destination)


def _download(url: str, destination: Path) -> None:
    print(f"Downloading {url}")
    with urllib.request.urlopen(url) as response, destination.open("wb") as output:
        shutil.copyfileobj(response, output)


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def install_archive_bundle(url: str, sha256: str, destination: Path, dry_run: bool) -> Path:
    """Install a checksummed archive while preserving its root contents."""

    if destination.is_dir() and not dry_run:
        return destination
    if destination.exists() and not dry_run:
        raise SetupUnavailable(f"incomplete tool directory exists: {destination}")

    print(f"Tool destination: {destination}")
    if dry_run:
        print(f"$ download {url}")
        print(f"$ verify sha256 {sha256}")
        print(f"$ extract -> {destination}")
        return destination

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="newmota-setup-") as temporary:
        temporary_path = Path(temporary)
        archive = temporary_path / Path(url).name
        extracted = temporary_path / "extracted"
        extracted.mkdir()
        _download(url, archive)
        actual_hash = _file_sha256(archive)
        if actual_hash != sha256:
            raise SetupUnavailable(f"tool checksum mismatch: expected {sha256}, got {actual_hash}")
        _extract_archive(archive, extracted)
        shutil.move(str(extracted), destination)
    return destination


def install_archive_toolchain(
    url: str,
    sha256: str,
    destination: Path,
    prefix: str,
    dry_run: bool,
    compiler_subdir: Path = Path(),
) -> Path:
    compiler_root = destination / compiler_subdir
    compiler = compiler_path(compiler_root, prefix)
    if compiler.is_file() and not dry_run:
        verify_toolchain(compiler_root, prefix)
        print(f"Toolchain already installed: {destination}")
        return compiler
    if destination.exists() and not dry_run:
        raise SetupUnavailable(f"incomplete toolchain directory exists: {destination}")

    print(f"Toolchain destination: {destination}")
    if dry_run:
        print(f"$ download {url}")
        print(f"$ verify sha256 {sha256}")
        print(f"$ extract -> {destination}")
        return compiler

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="newmota-setup-") as temporary:
        temporary_path = Path(temporary)
        archive = temporary_path / Path(url).name
        extracted = temporary_path / "extracted"
        extracted.mkdir()
        _download(url, archive)
        actual_hash = _file_sha256(archive)
        if actual_hash != sha256:
            raise SetupUnavailable(
                f"toolchain checksum mismatch: expected {sha256}, got {actual_hash}"
            )
        _extract_archive(archive, extracted)
        contents = list(extracted.iterdir())
        if len(contents) != 1 or not contents[0].is_dir():
            raise SetupUnavailable("toolchain archive has an unexpected directory layout")
        shutil.move(str(contents[0]), destination)
    verify_toolchain(compiler_root, prefix)
    return compiler


def install_arm_none_eabi_toolchain(dry_run: bool) -> Path:
    host = host_key()
    if host not in ARM_NONE_EABI_ARCHIVES:
        raise SetupUnavailable(f"Arm GNU has no managed package for {host}")
    archive_name, sha256 = ARM_NONE_EABI_ARCHIVES[host]
    return install_archive_toolchain(
        f"{ARM_NONE_EABI_RELEASE}/{archive_name}",
        sha256,
        arm_none_eabi_toolchain(),
        "arm-none-eabi-",
        dry_run,
    )


def install_git_toolchain(
    url: str, revision: str, destination: Path, prefix: str, dry_run: bool
) -> Path:
    compiler = compiler_path(destination, prefix)
    if compiler.is_file() and not dry_run:
        head = subprocess.run(
            ["git", "-C", str(destination), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        if head != revision:
            raise SetupUnavailable(
                f"toolchain pin mismatch at {destination}: expected {revision}, got {head}"
            )
        verify_toolchain(destination, prefix)
        print(f"Toolchain already installed: {destination}")
        return compiler
    if destination.exists() and not dry_run:
        raise SetupUnavailable(f"incomplete toolchain directory exists: {destination}")

    if not dry_run:
        destination.parent.mkdir(parents=True, exist_ok=True)
    run(
        ["git", "clone", "--filter=blob:none", "--no-checkout", url, str(destination)],
        dry_run=dry_run,
    )
    run(
        ["git", "-C", str(destination), "checkout", "--detach", revision],
        dry_run=dry_run,
    )
    if not dry_run:
        verify_toolchain(destination, prefix)
    return compiler


def stage_project_sources(
    context: BuildContext,
    source_dir: Path,
    include_dir: Path,
    *,
    aliases: dict[str, str],
    relative_aliases: dict[str, str],
) -> dict[Path, Path]:
    """Translate the common source selection into a flat-make SDK's single source folder."""

    from newmota.lvgl import is_lvgl_source

    sources = {}
    for relative in context.features.sources:
        original = ROOT / relative
        if original.is_relative_to(ROOT / "src/third_party/tasmota_audio"):
            continue  # Codec paths and unique object names are staged separately.
        if "berry" in original.parts or is_lvgl_source(original):
            continue  # Berry's nested runtime and configuration are staged separately.
        destination_name = relative_aliases.get(relative, aliases.get(original.name, original.name))
        destination = source_dir / destination_name
        if destination in sources.values():
            raise BuildError(f"flat-make SDK source filename collision: {destination.name}")
        sources[original] = destination
    inventory = source_dir / ".newmota-sources.json"
    previous = []
    if inventory.is_file():
        previous = json.loads(inventory.read_text(encoding="utf-8"))
        if not isinstance(previous, list) or any(
            not isinstance(name, str)
            or Path(name).name != name
            or Path(name).suffix not in (".c", ".cpp")
            for name in previous
        ):
            raise BuildError(f"invalid generated source inventory: {inventory}")
    selected = sorted(path.name for path in sources.values())
    # Remove only previously staged Newmota inputs, never arbitrary vendor sources.
    for name in previous:
        destination = source_dir / name
        if name not in selected:
            destination.unlink(missing_ok=True)
            destination.with_suffix(".o").unlink(missing_ok=True)
            destination.with_suffix(".d").unlink(missing_ok=True)
    for header in sorted((ROOT / "src/app").rglob("*.h")):
        sources[header] = include_dir / header.relative_to(ROOT / "src/app")
    if context.features.enabled("USE_PING"):
        # Raw Ping is flattened with application sources on Beken and AmebaD.
        # Keep its private quoted include available in the staged include path.
        sources[ROOT / "src/platform/lwip/ping_packet.h"] = include_dir / "ping_packet.h"
    coremqtt_root = ROOT / "src/third_party/coremqtt/v2.3.1"
    if context.features.enabled("USE_CRYPTO"):
        from newmota.crypto import BEARSSL_SOURCE_ROOT

        for header_root in (BEARSSL_SOURCE_ROOT, ROOT / "src/third_party/bearssl/extensions"):
            for header in sorted(header_root.glob("*.h")):
                sources[header] = include_dir / header.name
    coremqtt_source = coremqtt_root / "core_mqtt_serializer.c"
    if coremqtt_source in sources:
        for header_root in (coremqtt_root / "include", coremqtt_root / "interface"):
            for header in sorted(header_root.glob("*.h")):
                destination = include_dir / header.name
                if destination in sources.values():
                    raise BuildError(
                        f"staged coreMQTT header collides with another source: {destination.name}"
                    )
                sources[header] = destination
    for original, destination in sources.items():
        copy_if_changed(original, destination)
    copy_if_changed(context.features.header, include_dir / "newmota_features.h")
    if context.features.enabled("USE_DISPLAY_FONTS"):
        from newmota.fonts import GENERATED

        for name in GENERATED:
            (include_dir / name).parent.mkdir(parents=True, exist_ok=True)
            copy_if_changed(context.features.header.parent / name, include_dir / name)
    copy_if_changed(context.hardware.header, include_dir / context.hardware.header.name)
    copy_if_changed(
        context.hardware.names_header,
        include_dir / context.hardware.names_header.name,
    )
    copy_if_changed(
        context.hardware.topology_header,
        include_dir / context.hardware.topology_header.name,
    )
    write_if_changed(inventory, json.dumps(selected, indent=2) + "\n")
    return sources
