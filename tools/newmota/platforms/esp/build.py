"""ESP-IDF setup and native build adapter."""

from __future__ import annotations

import json
import os
import platform as host_platform
import re
import shutil
import subprocess
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from newmota.berry import generate_berry
from newmota.host import (
    SdkVersion,
    SdkVersionError,
    check_sdk_version,
    command_succeeds,
    cross_build_environment,
    find_ccache,
    host_key,
    load_sdk_version,
    run,
    setup_python,
    verbose_build_enabled,
)
from newmota.i18n import generate_recovery_web_assets, generate_web_assets
from newmota.model import (
    ROOT,
    ApplicationUpdate,
    Artifact,
    BoardDefinition,
    BuildContext,
    BuildError,
    Environment,
    NativeConfigSpec,
    PackageResult,
    PlatformAdapter,
    SetupUnavailable,
)
from newmota.package import LayoutFacts, PackageError
from newmota.platforms.esp.images import (
    esp_application_partition,
    esp_partition_compatibility,
    package_esp_idf,
    validate_esp_idf_safeboot,
)

ESP_PYTHON_CONSTRAINTS = ROOT / "config" / "sdk" / "esp-idf-v5.5.constraints.txt"


def python_constraints(sdk_key: str) -> Path:
    track = "6.1" if sdk_key == "esp-idf-v6.1" else "5.5"
    return ROOT / "config" / "sdk" / f"esp-idf-v{track}.constraints.txt"


# WPA3 reaches the build through ESP-IDF's WPA supplicant. Keep every exposed
# feature switch disabled below 4 MiB; v6.1 has both ESP_WIFI and WIFI_RMT
# Kconfig switches, and leaving the remote-driver defaults enabled retains the
# WPA3 crypto code even when the ESP_WIFI switches are off.
WPA3_MINIMUM_FLASH_KB = 4096
WPA3_SETTINGS = (
    "CONFIG_ESP_WIFI_ENABLE_WPA3_SAE",
    "CONFIG_ESP_WIFI_ENABLE_SAE_PK",
    "CONFIG_ESP_WIFI_ENABLE_SAE_H2E",
    "CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT",
    "CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_STA",
    "CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT",
    "CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_SOFTAP",
    "CONFIG_WIFI_RMT_ENABLE_WPA3_SAE",
    "CONFIG_WIFI_RMT_ENABLE_SAE_PK",
    "CONFIG_WIFI_RMT_ENABLE_SAE_H2E",
    "CONFIG_WIFI_RMT_SOFTAP_SAE_SUPPORT",
    "CONFIG_WIFI_RMT_ENABLE_WPA3_OWE_STA",
    "CONFIG_WIFI_RMT_WPA3_COMPATIBLE_SUPPORT",
    "CONFIG_WIFI_RMT_ENABLE_WPA3_OWE_SOFTAP",
)


def install_environment(tools_path: Path, sdk_key: str = "esp-idf") -> dict[str, str]:
    constraints = python_constraints(sdk_key)
    if not constraints.is_file():
        raise SetupUnavailable(f"missing ESP-IDF Python constraints: {constraints}")

    environment = os.environ.copy()
    for name in (
        "IDF_PATH",
        "IDF_PYTHON_ENV_PATH",
        "ESP_IDF_VERSION",
        "PIP_CONFIG_FILE",
        "PIP_CONSTRAINT",
        "PIP_EXTRA_INDEX_URL",
        "PIP_FIND_LINKS",
        "PIP_INDEX_URL",
        "PIP_NO_INDEX",
        "PIP_TRUSTED_HOST",
        "PYTHONHOME",
        "PYTHONPATH",
        "VIRTUAL_ENV",
    ):
        environment.pop(name, None)

    base_python = Path(setup_python()).resolve()
    current_path = environment.get("PATH", "")
    environment["PATH"] = os.pathsep.join((str(base_python.parent), current_path))
    environment["IDF_TOOLS_PATH"] = str(tools_path)
    environment["ESP_PYTHON"] = str(base_python)
    environment["PIP_CONFIG_FILE"] = os.devnull
    environment["PIP_CONSTRAINT"] = str(constraints)
    return environment


def _esp_python(tools_path: Path, idf_version: str) -> Path:
    match = re.fullmatch(r"v?(\d+)\.(\d+)(?:\.\d+)?", idf_version)
    if not match:
        raise SetupUnavailable(f"cannot derive Python environment from {idf_version}")
    idf_series = ".".join(match.groups())
    try:
        result = subprocess.run(
            [
                setup_python(),
                "-c",
                "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise SetupUnavailable(f"cannot inspect setup Python: {setup_python()}") from error
    python_series = result.stdout.strip()
    if not re.fullmatch(r"\d+\.\d+", python_series):
        raise SetupUnavailable(f"setup Python returned an invalid version: {python_series!r}")
    environment = tools_path / "python_env" / f"idf{idf_series}_py{python_series}_env"
    if os.name == "nt":
        return environment / "Scripts" / "python.exe"
    return environment / "bin" / "python"


def _verify_install(
    sdk: Path,
    tools_path: Path,
    idf_version: str,
    environment: dict[str, str],
    dry_run: bool,
) -> None:
    idf_tools = sdk / "tools" / "idf_tools.py"
    base_python = setup_python()
    run(
        [base_python, str(idf_tools), "--idf-path", str(sdk), "check"],
        cwd=sdk,
        dry_run=dry_run,
        environment=environment,
    )
    run(
        [
            base_python,
            str(idf_tools),
            "--idf-path",
            str(sdk),
            "check-python-dependencies",
        ],
        cwd=sdk,
        dry_run=dry_run,
        environment=environment,
    )
    python = _esp_python(tools_path, idf_version)
    if not dry_run and not python.is_file():
        raise SetupUnavailable(f"ESP-IDF Python environment is missing: {python}")
    run(
        [str(python), "-m", "pip", "check"],
        cwd=sdk,
        dry_run=dry_run,
        environment=environment,
    )


def _idf_host_platform() -> str:
    return {
        "darwin-arm64": "macos-arm64",
        "darwin-x64": "macos",
        "linux-arm64": "linux-arm64",
        "linux-x64": "linux-amd64",
        "win32-arm64": "win-arm64",
        "win32-x64": "win64",
    }.get(host_key(), "")


def _tools_issue(sdk: Path, tools_path: Path, chips: list[str]) -> str | None:
    manifest_path = sdk / "tools" / "tools.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return f"cannot read {manifest_path.relative_to(ROOT)}: {error}"

    host = _idf_host_platform()
    if not host:
        return f"ESP-IDF has no host mapping for {host_platform.system()}-{host_platform.machine()}"
    selected_targets = set(chips)
    for tool in manifest.get("tools", []):
        install_type = tool.get("install")
        for override in tool.get("platform_overrides", []):
            if host in override.get("platforms", []):
                install_type = override.get("install", install_type)
        supported_targets = set(tool.get("supported_targets", []))
        supports_selection = "all" in supported_targets or bool(
            selected_targets & supported_targets
        )
        if install_type != "always" or not supports_selection:
            continue
        version = next(
            (
                candidate.get("name")
                for candidate in tool.get("versions", [])
                if candidate.get("status") == "recommended"
                and (host in candidate or "any" in candidate)
            ),
            None,
        )
        if not version:
            return f"no recommended {tool.get('name', 'unknown')} package for {host}"
        destination = tools_path / "tools" / tool["name"] / version
        if not destination.is_dir():
            return f"missing ESP-IDF tool {tool['name']}@{version}"
    return None


def _registered_targets_issue(sdk: Path, tools_path: Path, chips: list[str]) -> str | None:
    environment_path = tools_path / "idf-env.json"
    try:
        data = json.loads(environment_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return f"cannot read {environment_path}: {error}"

    installed = data.get("idfInstalled", {})
    if not isinstance(installed, dict):
        return f"{environment_path} has no idfInstalled object"
    registered: set[str] = set()
    for record in installed.values():
        if not isinstance(record, dict):
            continue
        path = record.get("path")
        targets = record.get("targets")
        if not isinstance(path, str) or not isinstance(targets, list):
            continue
        try:
            same_sdk = Path(path).expanduser().resolve() == sdk.resolve()
        except OSError:
            same_sdk = False
        if same_sdk:
            registered.update(target for target in targets if isinstance(target, str))

    missing = sorted(set(chips) - registered)
    if missing:
        return "ESP-IDF tools environment has not registered targets: " + ", ".join(missing)
    return None


def setup_issue(chips: list[str], sdk_key: str = "esp-idf") -> str | None:
    try:
        version = check_sdk_version(sdk_key)
    except (OSError, SdkVersionError) as error:
        return str(error)
    sdk = version.source
    tools_path = Path(os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif")).expanduser()
    issue = _registered_targets_issue(sdk, tools_path, chips)
    if issue:
        return issue
    issue = _tools_issue(sdk, tools_path, chips)
    if issue:
        return issue
    try:
        environment = install_environment(tools_path, sdk_key)
        python = _esp_python(tools_path, version.version)
    except (OSError, SetupUnavailable) as error:
        return str(error)
    if not python.is_file():
        return f"missing ESP-IDF Python environment {python}"
    idf_tools = sdk / "tools" / "idf_tools.py"
    if not command_succeeds(
        [
            setup_python(),
            str(idf_tools),
            "--idf-path",
            str(sdk),
            "check-python-dependencies",
        ],
        cwd=sdk,
        environment=environment,
    ):
        return "ESP-IDF Python requirements are missing or incompatible"
    if not command_succeeds([str(python), "-m", "pip", "check"], cwd=sdk, environment=environment):
        return "ESP-IDF Python environment has broken package requirements"
    return None


def setup_checkout(dry_run: bool, sdk_key: str) -> None:
    """Initialize and verify a pinned IDF checkout without installing target tools."""
    sdk = load_sdk_version(sdk_key).source
    run(
        ["git", "submodule", "update", "--init", "--recursive", str(sdk.relative_to(ROOT))],
        dry_run=dry_run,
    )
    if not dry_run:
        check_sdk_version(sdk_key)


def setup(chips: list[str], dry_run: bool, sdk_key: str = "esp-idf") -> None:
    version = load_sdk_version(sdk_key)
    sdk = version.source
    targets = ",".join(chips)
    if not targets:
        raise SetupUnavailable("no ESP environments define an ESP-IDF target")

    tools_path = Path(os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif")).expanduser()
    print(f"ESP-IDF source: {sdk}")
    print(f"ESP-IDF version: {version.version} ({version.revision[:12]})")
    print(f"ESP-IDF tools:  {tools_path}")
    print(f"IDF targets:    {targets}")
    print(f"Host Python:    {setup_python()}")
    print("Python indexes: isolated from user and global pip configuration")
    print(f"Python constraints: {python_constraints(sdk_key).relative_to(ROOT)}")
    setup_checkout(dry_run, sdk_key)
    environment = install_environment(tools_path, sdk_key)
    if os.name == "nt":
        command = ["cmd.exe", "/d", "/c", str(sdk / "install.bat"), targets]
    else:
        command = ["bash", str(sdk / "install.sh"), targets]
    run(command, cwd=sdk, dry_run=dry_run, environment=environment)
    _verify_install(sdk, tools_path, version.version, environment, dry_run)


def ensure_flash_tools(sdk_key: str = "esp-idf") -> tuple[SdkVersion, dict[str, str], Path]:
    """Prepare ESP-IDF's Python programming tools without installing a compiler."""

    version = load_sdk_version(sdk_key)
    if not version.source.is_dir():
        run(
            [
                "git",
                "submodule",
                "update",
                "--init",
                "--recursive",
                str(version.source.relative_to(ROOT)),
            ]
        )
    version = check_sdk_version(sdk_key)
    tools_path = Path(os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif")).expanduser()
    environment = install_environment(tools_path, sdk_key)
    python = _esp_python(tools_path, version.version)
    version_check = (
        "import importlib.metadata, esp_idf_monitor, serial; "
        f"assert importlib.metadata.version('esptool') == '{version.programmer_version}'"
    )
    ready = python.is_file() and command_succeeds(
        [str(python), "-c", version_check],
        cwd=version.source,
        environment=environment,
    )
    if not ready:
        print("Installing ESP-IDF programming and monitoring Python tools (no compiler).")
        run(
            [
                setup_python(),
                str(version.source / "tools" / "idf_tools.py"),
                "--idf-path",
                str(version.source),
                "install-python-env",
                *([] if not python.exists() else ["--reinstall"]),
            ],
            cwd=version.source,
            environment=environment,
        )
    if not python.is_file():
        raise SetupUnavailable(f"ESP-IDF Python environment is missing: {python}")
    if not command_succeeds(
        [str(python), "-c", version_check],
        cwd=version.source,
        environment=environment,
    ):
        raise SetupUnavailable("ESP-IDF programming Python tools are incomplete")
    run(
        [str(python), "-m", "pip", "check"],
        cwd=version.source,
        environment=environment,
    )
    environment["IDF_PATH"] = str(version.source)
    return version, environment, python


def _build_environment(sdk: SdkVersion) -> tuple[dict[str, str], Path]:
    tools_path = Path(os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif")).expanduser()
    environment = cross_build_environment(install_environment(tools_path, sdk.key))
    environment["IDF_PATH"] = str(sdk.source)
    environment["ESP_PYTHON"] = setup_python()
    environment["IDF_CCACHE_ENABLE"] = "1" if find_ccache() else "0"
    if sdk.key == "esp-idf":
        # IDF 5.5.5's soc/lldesc.c uses NULL without including <stddef.h>.
        # Keep the pinned SDK immutable and apply the header fix to application
        # and bootloader C compilation until that track contains the include.
        extra_cflags = environment.get("EXTRA_CFLAGS", "")
        environment["EXTRA_CFLAGS"] = f"{extra_cflags} -include stddef.h".strip()
    result = run(
        [
            setup_python(),
            str(sdk.source / "tools" / "idf_tools.py"),
            "--idf-path",
            str(sdk.source),
            "export",
            "--format",
            "key-value",
        ],
        cwd=sdk.source,
        environment=environment,
        capture=True,
    )
    original_path = environment.get("PATH", "")
    for line in result.stdout.splitlines():
        key, separator, value = line.partition("=")
        if separator:
            environment[key] = value.replace("$PATH", original_path)
    python_environment = environment.get("IDF_PYTHON_ENV_PATH")
    if not python_environment:
        raise BuildError("ESP-IDF did not export IDF_PYTHON_ENV_PATH")
    executable = "python.exe" if os.name == "nt" else "python"
    executable_dir = "Scripts" if os.name == "nt" else "bin"
    python = Path(python_environment) / executable_dir / executable
    if not python.is_file():
        raise BuildError(f"ESP-IDF Python executable is missing: {python}")
    return environment, python


# BLE is never a stripped ESP profile: the NimBLE host keeps every role and
# the SDK's feature defaults. BLE 4 is the ESP default; extended advertising
# stays off. Tuning follows measurement of the full stack.
BLE_SETTINGS = {
    "CONFIG_BT_ENABLED": "y",
    "CONFIG_BT_NIMBLE_ENABLED": "y",
    "CONFIG_BT_NIMBLE_EXT_ADV": "n",
}
# Global addresses let controllers on other subnets reach the device; link-local alone is not enough.
MATTER_SETTINGS = {
    "CONFIG_LWIP_IPV6_AUTOCONFIG": "y",
    # Matter's Berry callbacks share the main task with commands and settings
    # writes. Reserve reusable stack space for their temporary buffers.
    "CONFIG_ESP_MAIN_TASK_STACK_SIZE": "10240",
}


class EspIdfAdapter(PlatformAdapter):
    native_config = NativeConfigSpec("esp-idf", "sdkconfig.defaults")

    @classmethod
    def effective_sdk_log_level(cls, environment: Environment, requested: str) -> str:
        """Reserve C5 recovery space while retaining SDK error diagnostics."""

        if environment.chip == "esp32c5" and environment.role == "safeboot":
            return "none" if requested == "none" else "error"
        return requested

    @classmethod
    def sdk_log_settings(cls, level: str) -> Mapping[str, Any]:
        """Set ESP-IDF application and bootloader logging at compile time."""

        selected = level.upper()
        levels = ("NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE")
        settings = {
            f"CONFIG_LOG_DEFAULT_LEVEL_{candidate}": "y" if candidate == selected else "n"
            for candidate in levels
        }
        settings.update(
            {
                f"CONFIG_BOOTLOADER_LOG_LEVEL_{candidate}": "y" if candidate == selected else "n"
                for candidate in levels
            }
        )
        settings.update(
            {
                "CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT": "y",
                "CONFIG_LOG_MAXIMUM_LEVEL_ERROR": "n",
                "CONFIG_LOG_MAXIMUM_LEVEL_WARN": "n",
                "CONFIG_LOG_MAXIMUM_LEVEL_INFO": "n",
                "CONFIG_LOG_MAXIMUM_LEVEL_DEBUG": "n",
                "CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE": "n",
                "CONFIG_LOG_DYNAMIC_LEVEL_CONTROL": "n",
                "CONFIG_LOG_TAG_LEVEL_IMPL_NONE": "y",
            }
        )
        return settings

    @classmethod
    def board_native_settings(
        cls,
        environment: Environment,
        source: str,
        partition: Path | None = None,
    ) -> Mapping[str, Any]:
        board = environment.board_data
        flash = board.get("flash")
        if not isinstance(flash, Mapping):
            raise BuildError(f"{source}: flash must be an object")
        flash_kb = flash.get("size_kb")
        if not isinstance(flash_kb, int) or flash_kb <= 0 or flash_kb % 1024:
            raise BuildError(f"{source}: flash.size_kb must be a positive whole MiB value")
        if partition is None or partition.suffix != ".csv":
            raise BuildError(f"{source}: ESP-IDF requires a CSV partition default")
        settings = {
            f"CONFIG_ESPTOOLPY_FLASHSIZE_{flash_kb // 1024}MB": "y",
            "CONFIG_PARTITION_TABLE_CUSTOM": "y",
            "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": f'"{partition}"',
        }
        flash_mode = flash.get("mode")
        if flash_mode not in {"dio", "dout", "qio", "qout", "opi"}:
            raise BuildError(f"{source}: unsupported ESP-IDF flash mode '{flash_mode}'")
        settings[f"CONFIG_ESPTOOLPY_FLASHMODE_{str(flash_mode).upper()}"] = "y"
        if flash_mode == "opi":
            settings["CONFIG_ESPTOOLPY_OCT_FLASH"] = "y"
        flash_frequency = flash.get("frequency_mhz")
        if not isinstance(flash_frequency, int):
            raise BuildError(f"{source}: ESP-IDF flash.frequency_mhz is required")
        settings[f"CONFIG_ESPTOOLPY_FLASHFREQ_{flash_frequency}M"] = "y"
        if flash_frequency > 80:
            settings["CONFIG_IDF_EXPERIMENTAL_FEATURES"] = "y"
            if flash_mode == "opi":
                settings["CONFIG_ESPTOOLPY_FLASH_SAMPLE_MODE_DTR"] = "y"
            else:
                settings["CONFIG_ESPTOOLPY_FLASH_SAMPLE_MODE_STR"] = "y"
                settings["CONFIG_SPI_FLASH_HPM_AUTO"] = "y"

        psram = board.get("psram")
        if psram is not None:
            if not isinstance(psram, Mapping):
                raise BuildError(f"{source}: psram must be an object")
            psram_mode = psram.get("mode")
            if psram_mode not in {"quad", "octal", "hex"}:
                raise BuildError(f"{source}: unsupported ESP-IDF PSRAM mode '{psram_mode}'")
            mode_symbol = {"quad": "QUAD", "octal": "OCT", "hex": "HEX"}[str(psram_mode)]
            settings.update(
                {
                    "CONFIG_SPIRAM": "y",
                    f"CONFIG_SPIRAM_MODE_{mode_symbol}": "y",
                    "CONFIG_SPIRAM_BOOT_INIT": "y",
                }
            )
            psram_frequency = psram.get("frequency_mhz")
            if psram_frequency is not None:
                settings[f"CONFIG_SPIRAM_SPEED_{psram_frequency}M"] = "y"
                if psram_frequency > 80:
                    settings["CONFIG_IDF_EXPERIMENTAL_FEATURES"] = "y"

        # Newmota owns runtime USB selection.  Native USB CDC must remain the
        # IDF primary so its ROM driver is initialized.  USB Serial/JTAG uses
        # the always-present hardware directly, leaving UART0 available for
        # early SDK output and runtime fallback without IDF's permanent SOF
        # connection-monitor hook.
        console_symbol = {
            "uart0": "CONFIG_ESP_CONSOLE_UART_DEFAULT",
            "usb-cdc": "CONFIG_ESP_CONSOLE_USB_CDC",
            "usb-serial-jtag": "CONFIG_ESP_CONSOLE_UART_DEFAULT",
        }.get(environment.console)
        if console_symbol is None:
            raise BuildError(f"{source}: ESP-IDF console is required")
        settings[console_symbol] = "y"
        settings["CONFIG_ESP_CONSOLE_SECONDARY_NONE"] = "y"
        if environment.console == "usb-serial-jtag":
            settings["CONFIG_USJ_ENABLE_USB_SERIAL_JTAG"] = "y"
        revision = board.get("revision")
        if revision is not None:
            # Revisions before 3.0 are a different chip to IDF: their minimum revision
            # choices only exist after selecting that family, or Kconfig drops them.
            if revision < 300:
                settings["CONFIG_ESP32P4_SELECTS_REV_LESS_V3"] = "y"
            settings[f"CONFIG_ESP32P4_REV_MIN_{revision}"] = "y"
        if board.get("cpu_cores") == 1:
            settings["CONFIG_FREERTOS_UNICORE"] = "y"
        return settings

    def __init__(self, context: BuildContext) -> None:
        super().__init__(context)
        self.sdkconfig = context.work_dir / "config" / "sdkconfig"
        self.environment: dict[str, str] = {}
        self.python = Path()

    def prepare(self) -> None:
        self.context.work_dir.mkdir(parents=True, exist_ok=True)
        sdk_marker = self.context.work_dir / "idf-sdk.json"
        sdk_identity = {
            "key": self.context.sdk.key,
            "revision": self.context.sdk.revision,
            "source": str(self.context.sdk.source.resolve()),
        }
        previous_sdk = None
        if sdk_marker.is_file():
            previous_sdk = json.loads(sdk_marker.read_text(encoding="utf-8"))
        else:
            description = self.context.work_dir / "project_description.json"
            if description.is_file():
                previous_path = json.loads(description.read_text(encoding="utf-8")).get("idf_path")
                if (
                    previous_path
                    and Path(previous_path).resolve() != self.context.sdk.source.resolve()
                ):
                    previous_sdk = {"source": previous_path}
        if previous_sdk is not None and previous_sdk != sdk_identity:
            print("Discarding native ESP-IDF outputs after SDK selection change.")
            for path in self.context.work_dir.iterdir():
                if path.name in {"config", "generated"}:
                    continue
                if path.is_dir() and not path.is_symlink():
                    shutil.rmtree(path)
                else:
                    path.unlink()
        sdk_marker.write_text(json.dumps(sdk_identity, indent=2) + "\n", encoding="utf-8")
        if self.context.native_config.changed and self.sdkconfig.exists():
            print("Discarding stale ESP-IDF sdkconfig after native config change.")
            self.sdkconfig.unlink()
        elif (
            self.context.environment.board_data.get("psram") is not None
            and self.sdkconfig.is_file()
            and "CONFIG_SPIRAM=y"
            not in self.sdkconfig.read_text(encoding="utf-8", errors="replace")
        ):
            print("Discarding stale ESP-IDF sdkconfig without board-required PSRAM.")
            self.sdkconfig.unlink()

    def configure(self) -> None:
        self.environment, self.python = _build_environment(self.context.sdk)
        self.environment["IDF_TARGET"] = self.context.environment.sdk_target
        self.environment["IDF_PY_BUILD_JOBS"] = str(self.context.jobs)

    def pre_build(self) -> None:
        generate_berry(self.context)
        if self.context.features.enabled("USE_WEB"):
            generate_web_assets(
                self.context.work_dir / "config",
                self.context.environment.language,
                dict(self.context.features.assignments),
                encoding="br" if self.context.features.enabled("USE_WEB_HTTPS") else "gzip",
            )
        if self.context.features.enabled("USE_RECOVERY_WEB"):
            generate_recovery_web_assets(self.context.work_dir / "config")

    def _run_idf(self, action: str) -> None:
        item = self.context.environment
        if item.partition is None:
            raise BuildError("ESP-IDF build requires a resolved partition layout")
        compatibility = esp_partition_compatibility(
            item.partition,
            item.flash_kb * 1024,
            item.name,
            item.sdk_target,
            (item.psram_kb or 0) * 1024,
        )
        source = ROOT / "src" / "platform" / "esp-idf" / "native"
        idf = self.context.sdk.source / "tools" / "idf.py"
        command = [
            str(self.python),
            str(idf),
            "-C",
            str(source),
            "-B",
            str(self.context.work_dir),
            f"-DIDF_TARGET={item.sdk_target}",
            f"-DSDKCONFIG={self.sdkconfig}",
            f"-DSDKCONFIG_DEFAULTS={self.context.native_config.defaults}",
            f"-DNEWMOTA_ROOT={ROOT}",
            f"-DNEWMOTA_ENV={item.name}",
            f"-DNEWMOTA_CONSOLE={item.console}",
            f"-DNEWMOTA_IMAGE_ROLE={item.role}",
            f"-DNEWMOTA_VERSION={self.context.version}",
            f"-DNEWMOTA_ESP_LAYOUT_ID={compatibility['layout_id']}",
            f"-DNEWMOTA_ESP_APP0_OFFSET={compatibility['app0_offset']}",
            f"-DNEWMOTA_ESP_SAFEBOOT_OFFSET={compatibility['safeboot_offset']}",
            f"-DNEWMOTA_ESP_OTADATA_OFFSET={compatibility['otadata_offset']}",
            f"-DNEWMOTA_ESP_NVS_OFFSET={compatibility['nvs_offset']}",
            f"-DNEWMOTA_ESP_PSRAM_BYTES={compatibility['psram_bytes']}",
            f"-DNEWMOTA_ESP_SDK_TARGET={item.sdk_target}",
            f"-DNEWMOTA_BUILD_DATE={self.context.build_date}",
            f"-DNEWMOTA_BUILD_TIME={self.context.build_time}",
            f"-DNEWMOTA_FEATURE_DIR={self.context.features.header.parent}",
            f"-DNEWMOTA_HARDWARE_DIR={self.context.hardware.header.parent}",
            f"-DNEWMOTA_BERRY_GENERATED_DIR={self.context.work_dir / 'generated' / 'berry'}",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ]
        if verbose_build_enabled():
            command.append("-v")
        command.append(action)
        run(command, environment=self.environment)

    def build(self) -> None:
        self._run_idf("build")

    def build_application(self) -> None:
        """Ask ESP-IDF to compile only the normal application image."""

        # ESP-IDF's public ``app`` target also generates a partition-table
        # binary solely for its own size check. Newmota validates against the
        # catalog CSV below, so target the ELF-to-image boundary directly.
        self._run_idf("gen_project_binary")

    def application_update(self) -> ApplicationUpdate:
        """Validate the raw application against the fixed app0 partition."""

        item = self.context.environment
        if item.partition is None:
            raise BuildError("ESP-IDF application update requires a partition layout")
        partition = esp_application_partition(item.partition, item.flash_kb * 1024)
        offset = partition.get("offset")
        size_limit = partition.get("size")
        if not isinstance(offset, int) or not isinstance(size_limit, int):
            raise BuildError("ESP-IDF app0 partition has invalid offset or size")
        image = self.context.work_dir / "newmota.bin"
        if not image.is_file():
            raise BuildError("ESP-IDF app-only build did not produce newmota.bin")
        if image.stat().st_size > size_limit:
            raise BuildError(
                f"application image is {image.stat().st_size} bytes but app0 allows "
                f"{size_limit} bytes"
            )
        print(
            f"Application update: {image.relative_to(ROOT)} -> app0 at {offset:#x} "
            f"({image.stat().st_size}/{size_limit} bytes)"
        )
        return ApplicationUpdate(image, offset, size_limit)

    def compilation_database(self) -> Path:
        self._run_idf("reconfigure")
        database = self.context.work_dir / "compile_commands.json"
        if not database.is_file():
            raise BuildError("ESP-IDF did not generate compile_commands.json")
        return database

    def post_build(self) -> tuple[Artifact, ...]:
        work = self.context.work_dir
        return (
            Artifact(work / "newmota.elf", "debug"),
            Artifact(work / "newmota.bin", "native"),
            Artifact(work / "newmota.map", "debug"),
            Artifact(work / "bootloader" / "bootloader.bin", "native"),
            Artifact(work / "partition_table" / "partition-table.bin", "native"),
            Artifact(work / "flasher_args.json", "metadata"),
        )

    def package(self) -> PackageResult:
        item = self.context.environment
        if item.role == "safeboot":
            return validate_esp_idf_safeboot(self.context)
        if item.safeboot is None:
            print(
                "ESP-IDF package unqualified: application environment has no verified "
                "safeboot dependency"
            )
            return PackageResult.unqualified()
        flasher_path = self.context.work_dir / "flasher_args.json"
        try:
            flasher = json.loads(flasher_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise PackageError(f"cannot read {flasher_path.relative_to(ROOT)}: {error}") from error
        if not isinstance(flasher, dict):
            raise PackageError("ESP-IDF flasher_args.json must contain an object")
        return package_esp_idf(self.context, self.python, flasher, self.environment)


# The single-stream hardware encoder of the vendored esp_h264 1.4.1 subset.
ESP_H264_SOURCES = (
    "interface/src/esp_h264_enc_single.c",
    "interface/src/esp_h264_enc_param.c",
    "interface/src/esp_h264_enc_param_hw.c",
    "interface/src/esp_h264_version.c",
    "hw/src/esp_h264_enc_single_hw.c",
    "hw/src/esp_h264_enc_hw_param.c",
    "hw/src/h264_nal.c",
    "hw/src/h264_rc.c",
    "hw/hal/esp32p4/h264_hal.c",
    "hw/hal/esp32p4/h264_dma_hal.c",
    "port/src/esp_h264_alloc.c",
    "port/src/esp_h264_cache.c",
)


# esp-dsp's float radix-2 FFT for the spectrum analyzer: the C helpers, the bit-reversal
# tables, and the S3 (aes3) and P4 (arp4) kernels.
ESP_DSP_SOURCES = (
    "common/misc/dsps_pwroftwo.cpp",
    "fft/float/dsps_fft2r_fc32_ansi.c",
    "fft/float/dsps_fft2r_bitrev_tables_fc32.c",
    "fft/float/dsps_fft2r_fc32_aes3_.S",
    "fft/float/dsps_bit_rev_lookup_fc32_aes3.S",
    "fft/float/dsps_fft2r_fc32_arp4.S",
)


# The registry Ethernet drivers that ESP-IDF 6.x no longer carries in esp_eth.
ETH_DRIVER_SOURCES = (
    "ip101/src/esp_eth_phy_ip101.c",
    "lan87xx/src/esp_eth_phy_lan87xx.c",
    "rtl8201/src/esp_eth_phy_rtl8201.c",
    "dp83848/src/esp_eth_phy_dp83848.c",
    "ksz80xx/src/esp_eth_phy_ksz80xx.c",
    "dm9051/src/esp_eth_mac_dm9051.c",
    "dm9051/src/esp_eth_phy_dm9051.c",
    "ksz8851snl/src/esp_eth_mac_ksz8851snl.c",
    "ksz8851snl/src/esp_eth_phy_ksz8851snl.c",
    "w5500/src/esp_eth_mac_w5500.c",
    "w5500/src/esp_eth_phy_w5500.c",
    "wiznet_common/src/wiznet_mac_common.c",
    "wiznet_common/src/wiznet_phy_common.c",
    "wiznet_common/src/wiznet_spi.c",
    "wiznet_common/src/wiznet_utils.c",
)


# The coprocessor Tasmota's ULP module drives, with the RTC slow memory its Arduino builds
# reserve. The S2 and S3 also have an FSM; like Tasmota, they use the RISC-V core.
ULP_COPROCESSORS = {
    "esp32": ("FSM", 4096),
    "esp32s2": ("RISCV", 4096),
    "esp32s3": ("RISCV", 4096),
    "esp32c5": ("LP_CORE", 8192),
    "esp32c6": ("LP_CORE", 8192),
    "esp32p4": ("LP_CORE", 8192),
    "esp32s31": ("LP_CORE", 8192),
}


def native_requirements(
    environment: Environment, values: dict[str, bool], layout: LayoutFacts | None
) -> tuple[tuple[str, ...], tuple[str, ...], dict[str, str]]:
    """Translate common feature selection into this SDK's sources and settings."""
    sources = [
        "platform/esp-idf/digital_gpio.cpp",
        "platform/esp-idf/platform.cpp",
        "platform/esp-idf/native/main/entry.cpp",
    ]
    sdk_requirements: list[str] = []
    if values.get("USE_ADC", False):
        sdk_requirements.append("esp_adc")
    if values.get("USE_I2S", False):
        sdk_requirements.append("esp_driver_i2s")
    if values.get("USE_WS2812", False):
        sdk_requirements.extend(("esp_driver_rmt", "esp_driver_spi"))
    if values.get("USE_DISPLAY", False):
        # UDisplay owns direct SPI transports and uses ESP-IDF's public LCD
        # interfaces for I80, RGB and MIPI-DSI panels. The LDO/cache helpers
        # used by those native framebuffers are supplied by esp_hw_support.
        sdk_requirements.extend(("esp_driver_spi", "esp_hw_support", "esp_lcd", "esp_mm"))
    if values.get("USE_DEEPSLEEP", False) or values.get("USE_BERRY_ULP", False):
        sources.append("platform/esp-idf/deep_sleep.cpp")
    if values.get("USE_ESP_DSP", False):
        # esp-dsp's float radix-2 FFT; each target's assembly file builds empty on the other.
        sources.append("platform/esp-idf/fft.cpp")
        sources.extend(f"third_party/esp_dsp/modules/{source}" for source in ESP_DSP_SOURCES)
    if values.get("USE_WEBCAM", False):
        # DVP capture through the LCD_CAM controller and a separate port-81 stream server.
        sources.extend(("platform/esp-idf/camera.cpp", "platform/esp-idf/camera_web.cpp"))
        sdk_requirements.extend(("esp_driver_cam", "esp_http_server", "esp_mm", "lwip"))
    if values.get("USE_CSI_WEBCAM", False):
        # MIPI-CSI with the ISP, the JPEG and H.264 hardware encoders, the port-81 server,
        # RTSP/WebSocket and WebRTC. The H.264 hardware encoder is vendored from esp_h264.
        sources.extend(
            (
                "platform/esp-idf/camera_csi.cpp",
                "platform/esp-idf/camera_csi_isp.cpp",
                "platform/esp-idf/camera_csi_jpeg.cpp",
                "platform/esp-idf/camera_csi_h264.cpp",
                "platform/esp-idf/camera_csi_rtsp.cpp",
                "platform/esp-idf/camera_csi_webrtc.cpp",
                "platform/esp-idf/camera_web.cpp",
                *(f"third_party/esp_h264/{source}" for source in ESP_H264_SOURCES),
            )
        )
        sdk_requirements.extend(
            (
                "esp_driver_cam",
                "esp_driver_isp",
                "esp_driver_jpeg",
                "esp_hw_support",
                "esp_http_server",
                "esp_mm",
                "efuse",
                "lwip",
            )
        )
        if environment.idf_version != "6.1":
            sdk_requirements.append("bootloader_support")
    # Keep the 8 KiB application-task floor unless a selected feature needs more.
    native_settings: dict[str, str] = {"CONFIG_ESP_MAIN_TASK_STACK_SIZE": "8192"}
    if any(
        values.get(name, False)
        for name in ("USE_CRYPTO_HW_SHA1", "USE_CRYPTO_HW_SHA256", "USE_CRYPTO_HW_SHA512")
    ):
        sdk_requirements.append("mbedtls")
        native_settings["CONFIG_MBEDTLS_HARDWARE_SHA"] = "y"
    if values.get("USE_CRYPTO_HW_P256", False):
        sdk_requirements.extend(("esp_security", "esp_hw_support", "hal"))
    if values.get("USE_CRYPTO_HW_AES", False):
        sdk_requirements.append("mbedtls")
        native_settings["CONFIG_MBEDTLS_HARDWARE_AES"] = "y"
    if values.get("USE_WEBCAM", False) or values.get("USE_CSI_WEBCAM", False):
        # The stream servers' listener, control and client sockets (three MJPEG viewers) come on
        # top of the main web server's and MQTT's.
        native_settings["CONFIG_LWIP_MAX_SOCKETS"] = "20"
    if values.get("USE_BERRY_ULP", False):
        coprocessor = ULP_COPROCESSORS.get(environment.sdk_target)
        if coprocessor is None:
            raise BuildError(
                f"USE_BERRY_ULP is unavailable: {environment.sdk_target} has no ULP coprocessor"
            )
        kind, reserve = coprocessor
        sources.append("platform/esp-idf/ulp.cpp")
        sdk_requirements.extend(("ulp", "esp_adc", "esp_driver_gpio"))
        native_settings.update(
            {
                "CONFIG_ULP_COPROC_ENABLED": "y",
                f"CONFIG_ULP_COPROC_TYPE_{kind}": "y",
                "CONFIG_ULP_COPROC_RESERVE_MEM": str(reserve),
            }
        )
    if values.get("USE_BLE", False):
        if environment.sdk_target in {"esp32s2", "esp32p4"}:
            raise BuildError(
                f"USE_BLE is unavailable: {environment.sdk_target} has no Bluetooth radio"
            )
        sources.extend(
            (
                "platform/esp-idf/ble.cpp",
                "platform/esp-idf/ble_scan.cpp",
                "platform/esp-idf/ble_connection.cpp",
            )
        )
        sdk_requirements.extend(("bt", "esp_ringbuf"))
        native_settings.update(BLE_SETTINGS)
    if values.get("USE_MATTER", False):
        native_settings.update(MATTER_SETTINGS)
    else:
        # Matter is the only IPv6 user, as on Beken.
        native_settings["CONFIG_LWIP_IPV6"] = "n"
    if values.get("USE_PWM", False) or values.get("USE_WS2812", False):
        sources.append("platform/esp-idf/pwm.cpp")
        sdk_requirements.append("esp_driver_ledc")
    sources.extend(
        (
            "app/core/storage/littlefs_crc.cpp",
            "platform/esp-idf/crc32.cpp",
            "platform/esp-idf/filesystem.cpp",
            "platform/esp-idf/console.cpp",
            "third_party/littlefs/lfs.c",
        )
    )
    sdk_requirements.extend(
        (
            "app_update",
            "bootloader_support",
            "esp_driver_gpio",
            "esp_driver_uart",
            "esp_partition",
            "esp_system",
            "esp_rom",
            "esp_timer",
            "hal",
            "heap",
            "log",
            "spi_flash",
        )
    )
    if environment.board_data.get("psram") is not None:
        sdk_requirements.append("esp_psram")
    if environment.console == "usb-serial-jtag":
        sdk_requirements.append("esp_driver_usb_serial_jtag")
    wpa3_removed = False
    if values["USE_WIFI"]:
        sdk_requirements.extend(
            (
                "esp_event",
                "esp_netif",
                "esp_phy",
                "esp_wifi",
                "lwip",
                "nvs_flash",
            )
        )
        native_settings.update(
            {
                "CONFIG_ESP_WIFI_SOFTAP_SUPPORT": "y",
                "CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT": "n",
                "CONFIG_WIFI_RMT_ENTERPRISE_SUPPORT": "n",
            }
        )
        # These Wi-Fi drivers also need WPA3 removed from Safeboot to fit the
        # fixed 832 KiB recovery slot used by Tasmota's 4 MiB layout.
        if environment.flash_kb < WPA3_MINIMUM_FLASH_KB or (
            environment.chip in {"esp32c5", "esp32c6", "esp32c61"}
            and environment.role == "safeboot"
        ):
            native_settings.update(dict.fromkeys(WPA3_SETTINGS, "n"))
            wpa3_removed = True
    if any(
        values.get(name, False)
        for name in ("USE_DISPLAY", "USE_ETHERNET", "USE_WS2812", "USE_SDCARD")
    ):
        # Every SPI host user leases its controller from the one owner.
        sources.append("platform/esp-idf/spi_host.cpp")
        sdk_requirements.append("esp_driver_spi")
    if values.get("USE_SDCARD", False):
        # esp_driver_sdmmc compiles no sources on chips without the SDMMC host.
        sdk_requirements.extend(("esp_driver_sdspi", "esp_driver_sdmmc", "sdmmc"))
    if values.get("USE_ETHERNET", False):
        # ESP-IDF 5.5 has every driver in esp_eth; 6.1 compiles the vendored registry copies.
        sources.append("platform/esp-idf/ethernet.cpp")
        sdk_requirements.extend(("esp_eth", "esp_driver_spi", "esp_driver_gpio", "esp_netif"))
        if environment.idf_version == "6.1":
            sources.extend(f"third_party/esp_eth_drivers/{source}" for source in ETH_DRIVER_SOURCES)
        else:
            native_settings.update(
                {
                    "CONFIG_ETH_USE_SPI_ETHERNET": "y",
                    "CONFIG_ETH_SPI_ETHERNET_DM9051": "y",
                    "CONFIG_ETH_SPI_ETHERNET_W5500": "y",
                    "CONFIG_ETH_SPI_ETHERNET_KSZ8851SNL": "y",
                }
            )
    if values.get("USE_ESP_HOSTED", False):
        # Wi-Fi over SDIO to an ESP32-C6, as on Tasmota's P4 builds. The defaults are the
        # Function EV Board's pins; template HSDIO pins replace them before the transport
        # starts. The vendored components come in through EXTRA_COMPONENT_DIRS.
        sources.append("platform/esp-idf/hosted.cpp")
        sdk_requirements.extend(("esp_hosted", "esp_driver_sdmmc"))
        if environment.idf_version != "6.1":
            sdk_requirements.append("esp_wifi_remote")
        native_settings.update(
            {
                # ESP-Hosted recommends a 1 ms tick to reduce SDIO timing jitter.
                "CONFIG_FREERTOS_HZ": "1000",
                "CONFIG_ESP_HOSTED_ENABLED": "y",
                "CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6": "y",
                "CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD": "y",
                "CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE": "y",
                "CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED": "y",
                "CONFIG_ESP_WIFI_REMOTE_EAP_ENABLED": "n",
            }
        )
    if (
        values.get("USE_MQTT", False)
        or values.get("USE_OTA_PULL", False)
        or (values.get("USE_I2S", False) and values.get("USE_WIFI", False))
    ):
        sources.append("platform/lwip/tcp_stream.cpp")
    if values.get("USE_WEB", False) or values.get("USE_RECOVERY_WEB", False):
        sources.append("platform/esp-idf/web.cpp")
        sdk_requirements.append(
            "esp_https_server" if values.get("USE_WEB_HTTPS", False) else "esp_http_server"
        )
        native_settings.update({"CONFIG_HTTPD_WS_SUPPORT": "n", "CONFIG_HTTPD_MAX_URI_LEN": "96"})
        if values.get("USE_WEB_HTTPS", False) and environment.idf_version == "6.1":
            native_settings.update(
                {
                    "CONFIG_MBEDTLS_PK_WRITE_C": "y",
                    "CONFIG_MBEDTLS_X509_CREATE_C": "y",
                    "CONFIG_MBEDTLS_X509_CRT_WRITE_C": "y",
                    "CONFIG_MBEDTLS_PEM_WRITE_C": "y",
                }
            )
    if environment.role == "safeboot":
        sources.append("platform/esp-idf/recovery_update_writer.cpp")
    if (
        wpa3_removed
        and not values.get("USE_WEB_HTTPS", False)
        and not any(
            values.get(name, False)
            for name in ("USE_CRYPTO_HW_SHA1", "USE_CRYPTO_HW_SHA256", "USE_CRYPTO_HW_SHA512")
        )
    ):
        # Without WPA3, Enterprise and TLS the supplicant's built-in WPA2 crypto
        # suffices and is smaller than the MbedTLS/PSA crypto that its MbedTLS
        # backend pulls in through the cipher and md tables. Keep SHA-1 on that
        # internal path as well; otherwise wpa_supplicant's sha1_vector hook is
        # left undefined when MbedTLS crypto is disabled. The hardware SHA
        # options need CONFIG_MBEDTLS_HARDWARE_SHA, which disables that path.
        native_settings.update(
            {
                "CONFIG_ESP_WIFI_MBEDTLS_CRYPTO": "n",
                "CONFIG_WIFI_RMT_MBEDTLS_CRYPTO": "n",
                "CONFIG_MBEDTLS_SHA1_C": "n",
                "CONFIG_MBEDTLS_HARDWARE_SHA": "n",
            }
        )
    if environment.role == "safeboot" and (
        environment.chip == "esp32c3"
        or (environment.idf_version == "6.1" and environment.chip == "esp32s3")
    ):
        # Preserve WPA3-Personal and H2E in the recovery image, but omit SAE-PK,
        # OWE, WPA3 SoftAP, and MbedTLS self-tests. Newmota does not expose OWE,
        # configures its recovery AP as open/WPA2, and does not invoke MbedTLS
        # self-tests. SAE-PK otherwise pulls public-key parsing into the fixed
        # 0xD0000 Safeboot partition on these targets.
        native_settings.update(
            {
                "CONFIG_ESP_WIFI_ENABLE_SAE_PK": "n",
                "CONFIG_WIFI_RMT_ENABLE_SAE_PK": "n",
                "CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT": "n",
                "CONFIG_WIFI_RMT_SOFTAP_SAE_SUPPORT": "n",
                "CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_STA": "n",
                "CONFIG_WIFI_RMT_ENABLE_WPA3_OWE_STA": "n",
                "CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_SOFTAP": "n",
                "CONFIG_WIFI_RMT_ENABLE_WPA3_OWE_SOFTAP": "n",
                "CONFIG_MBEDTLS_SELF_TEST": "n",
            }
        )
    if environment.idf_version != "6.1":
        v6_only_settings = {
            "CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT",
            "CONFIG_ESP_WIFI_ENABLE_WPA3_OWE_SOFTAP",
            "CONFIG_MBEDTLS_SELF_TEST",
        }
        native_settings = {
            key: value
            for key, value in native_settings.items()
            if not key.startswith("CONFIG_WIFI_RMT_") and key not in v6_only_settings
        }
    return tuple(sources), tuple(sdk_requirements), native_settings


def native_target_issue(board: BoardDefinition) -> str | None:
    """Read-only check that the pinned SDK contains the selected target."""
    sdk = load_sdk_version(board.chip.sdk_key).source
    target = board.chip.sdk_target
    expected = sdk / "components" / "soc" / target
    return None if expected.is_dir() else f"missing ESP-IDF target {expected.relative_to(ROOT)}"


def _has_kconfig_symbol(path: Path, symbol: str) -> bool:
    if not path.is_file():
        return False
    content = path.read_text(encoding="utf-8", errors="replace")
    return re.search(rf"^\s*config\s+{re.escape(symbol)}\b", content, re.MULTILINE) is not None


def board_issues(board: BoardDefinition) -> list[str]:
    sdk = load_sdk_version(board.chip.sdk_key).source
    target = board.chip.sdk_target
    issues: list[str] = []
    flash_kconfig = sdk / "components" / "spi_flash" / target / "Kconfig.flash_freq"
    esptool_kconfig = sdk / "components" / "esptool_py" / "Kconfig.projbuild"
    soc_caps = sdk / "components" / "soc" / target / "include" / "soc" / "soc_caps.h"

    flash_size = board.flash.size_kb // 1024 if board.flash.size_kb else 0
    if not _has_kconfig_symbol(esptool_kconfig, f"ESPTOOLPY_FLASHSIZE_{flash_size}MB"):
        issues.append(f"ESP-IDF has no {flash_size} MiB flash-size setting")
    frequency = board.flash.frequency_mhz
    if frequency is None or not _has_kconfig_symbol(
        flash_kconfig, f"ESPTOOLPY_FLASHFREQ_{frequency}M"
    ):
        issues.append(f"ESP-IDF {target} does not support {frequency} MHz flash")

    if board.flash.mode == "opi":
        caps = soc_caps.read_text(encoding="utf-8", errors="replace") if soc_caps.is_file() else ""
        if "SOC_SPI_MEM_SUPPORT_FLASH_OPI_MODE" not in caps:
            issues.append(f"ESP-IDF {target} does not support OPI flash")

    if board.psram is not None:
        if board.chip.capabilities.get("psram") is not True:
            issues.append(f"ESP-IDF {target} does not report PSRAM support")
        psram_kconfig = sdk / "components" / "esp_psram" / target / "Kconfig.spiram"
        mode = {"quad": "QUAD", "octal": "OCT", "hex": "HEX"}.get(board.psram.mode)
        if mode is None or not _has_kconfig_symbol(psram_kconfig, f"SPIRAM_MODE_{mode}"):
            issues.append(f"ESP-IDF {target} does not support {board.psram.mode} PSRAM")
        psram_frequency = board.psram.frequency_mhz
        if psram_frequency is not None and not _has_kconfig_symbol(
            psram_kconfig, f"SPIRAM_SPEED_{psram_frequency}M"
        ):
            issues.append(f"ESP-IDF {target} does not support {psram_frequency} MHz PSRAM")

    if board.console == "usb-cdc" and target not in {"esp32s2", "esp32s3"}:
        issues.append(f"ESP-IDF {target} cannot use its ROM USB CDC as the primary console")
    if (
        board.console == "usb-serial-jtag"
        and board.chip.capabilities.get("usb-serial-jtag") is not True
    ):
        issues.append(f"ESP-IDF {target} has no USB Serial/JTAG console")

    if board.revision is not None:
        revision_kconfig = (
            sdk / "components" / "esp_hw_support" / "port" / target / "Kconfig.hw_support"
        )
        if not _has_kconfig_symbol(revision_kconfig, f"ESP32P4_REV_MIN_{board.revision}"):
            issues.append(f"ESP-IDF {target} has no revision {board.revision} setting")
    if board.data.get("cpu_cores") == 1 and target != "esp32":
        issues.append(f"single-core selection is not qualified for ESP-IDF {target}")
    return issues
