"""BouffaloSDK setup and native build adapter."""

from __future__ import annotations

import os
import re
import shutil
import sys
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from newmota.berry import generate_berry
from newmota.config import SDK_LOG_LEVEL_VALUES
from newmota.host import (
    SdkVersion,
    SdkVersionError,
    check_sdk_version,
    cmake_status,
    compiler_path,
    copy_if_changed,
    cross_build_environment,
    display_path,
    ensure_sdk_source,
    host_key,
    install_archive_bundle,
    load_sdk_version,
    no_status,
    run,
    run_build,
    tools_root,
    verbose_build_enabled,
    verify_toolchain,
    write_if_changed,
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
from newmota.ota import chip_id
from newmota.package import LayoutFacts
from newmota.platforms.bouffalo.images import (
    BouffaloLayoutError,
    package_bouffalo,
    resolve_layout,
    validate_bouffalo_safeboot,
)


@dataclass(frozen=True)
class ToolchainSpec:
    """Source and integrity pin for one managed Bouffalo toolchain archive."""

    source: str
    pin: str
    archive_name: str


BOUFFALO_TOOLCHAIN_RELEASE = (
    "https://github.com/Newmota/bouffalo-toolchain-macos-arm64/releases/download/v1.0.30"
)
# All archives are root-layout install trees (bin/ at the top level), so they
# use the bundle installer rather than the one-top-level-directory toolchain
# installer. Every entry is a Xuantie GCC 14 build; the previous
# bouffalolab t-head GCC 10.2.0 git toolchains are retired. There is no
# darwin-x64 (Intel Mac) asset upstream, so that host reports as unsupported.
TOOLCHAINS = {
    "darwin-arm64": ToolchainSpec(
        BOUFFALO_TOOLCHAIN_RELEASE,
        "84e31f3e5118488652e536f5dd6a0baabbb9f957ba07751df231c6ecfa708ed2",
        "xuantie-gnu-toolchain-macos-26-arm64.tar.gz",
    ),
    "linux-x64": ToolchainSpec(
        BOUFFALO_TOOLCHAIN_RELEASE,
        "0889fc7700c33b6642d7ad630f07218f948cadcaf901d9a9bbb4d0ae6c743353",
        "xuantie-gnu-toolchain-linux-x86_64.tar.gz",
    ),
    "linux-arm64": ToolchainSpec(
        BOUFFALO_TOOLCHAIN_RELEASE,
        "98bfdc591456be4bda2cdf64a9a6410bedc125962a606a13bcb33ac3d868cc6b",
        "xuantie-gnu-toolchain-linux-arm64.tar.gz",
    ),
    "win32-x64": ToolchainSpec(
        BOUFFALO_TOOLCHAIN_RELEASE,
        "e4a2661425b5ed936c16d08f497fa638950b5b1db05dcc21f3e99c86ff16eb22",
        "xuantie-gnu-toolchain-windows-x86_64.tar.gz",
    ),
}


def _toolchain() -> tuple[Path, ToolchainSpec]:
    host = host_key()
    if host not in TOOLCHAINS:
        raise SetupUnavailable(f"Bouffalo has no toolchain package for {host}")
    spec = TOOLCHAINS[host]
    directory_name = f"xuantie-gcc-{spec.pin[:12]}"
    return tools_root() / "bouffalo" / directory_name, spec


def setup_issue(_chips: list[str]) -> str | None:
    try:
        check_sdk_version("bouffalo")
        destination, _ = _toolchain()
        if not destination.is_dir():
            return f"missing Bouffalo toolchain {destination}"
        verify_toolchain(destination, "riscv64-unknown-elf-")
    except (OSError, SdkVersionError, SetupUnavailable) as error:
        return str(error)
    return None


def setup(_chips: list[str], dry_run: bool) -> None:
    version = load_sdk_version("bouffalo")
    sdk = version.source
    destination, spec = _toolchain()
    print(f"BouffaloSDK source: {sdk}")
    print(f"BouffaloSDK version: {version.version} ({version.revision[:12]})")
    print(f"Bouffalo tools:     {destination}")
    ensure_sdk_source("bouffalo", dry_run)
    compiler = compiler_path(destination, "riscv64-unknown-elf-")
    install_archive_bundle(
        f"{spec.source}/{spec.archive_name}",
        spec.pin,
        destination,
        dry_run,
    )
    if not dry_run:
        verify_toolchain(destination, "riscv64-unknown-elf-")
    print(f"Bouffalo compiler:  {compiler}")


def _prepare_project(build_dir: Path, partition: Path) -> Path:
    source = ROOT / "src" / "platform" / "bouffalo" / "native"
    project = build_dir / "project"
    project.mkdir(parents=True, exist_ok=True)
    for path in source.iterdir():
        if path.is_file():
            copy_if_changed(path, project / path.name)
    copy_if_changed(partition, project / "partition.toml")
    return project


def _prepare_config(native: Path, context: BuildContext) -> None:
    generated = native / "generated"
    generated.mkdir(parents=True, exist_ok=True)
    environment = context.environment
    if environment.sdk_board is None:
        raise BuildError("Bouffalo Kconfig requires an SDK board")

    settings = dict(context.native_config.assignments)
    # CONFIG_LOG_LEVEL is a Bouffalo CMake compile-definition input, not a
    # symbol known by the pinned Kconfig when BFLB_LOG is disabled.  Keep it in
    # the resolved project configuration, but do not feed it to config_seed.
    settings.pop("CONFIG_LOG_LEVEL", None)
    if environment.role == "safeboot":
        # Preserve the recovery slot budget by omitting the SDK's CFI backtrace table.
        settings["CONFIG_BACKTRACE"] = "n"
    settings["CONFIG_PROJECT_SDK_VERSION"] = f'"{context.sdk.version}"'
    config_input = generated / "autoconfig.input"
    sdk = context.sdk.source
    config_environment = cross_build_environment()
    config_environment["BL_SDK_BASE"] = str(sdk)

    def seed() -> None:
        config_lines = [
            f"CHIP={environment.sdk_target}",
            f"BOARD={environment.sdk_board}",
            *(f"{key}={value}" for key, value in sorted(settings.items())),
        ]
        write_if_changed(config_input, "\n".join(config_lines) + "\n")
        run(
            [
                sys.executable,
                str(sdk / "tools" / "kconfig" / "config_seed.py"),
                "--defconfig",
                str(config_input),
                "--kconfig",
                str(sdk / "Kconfig"),
                "--def-out",
                str(generated / "autoconfig.def"),
                "--cmake-out",
                str(generated / "autoconfig.cmake"),
                "--header-out",
                str(generated / "autoconfig.h"),
                "--make-out",
                str(generated / "autoconfig.mk"),
            ],
            environment=config_environment,
        )

    seed()
    # The image packer copies boot2 with its mode and then rewrites the copy, which fails for the
    # read-only SDK archive cache. Stage a writable copy under its own name (the output keeps it).
    boot2 = _autoconfig_value(generated / "autoconfig.cmake", "CONFIG_BOARD_BOOT2FILE")
    if not boot2 or not Path(boot2).is_file():
        raise BuildError(f"Bouffalo Kconfig names no boot2 image: {boot2!r}")
    copy_if_changed(Path(boot2), context.work_dir / "project" / Path(boot2).name)
    settings["CONFIG_OVERRIDE_BOOT2FILE"] = Path(boot2).name
    seed()


def _autoconfig_value(path: Path, key: str) -> str | None:
    match = re.search(
        rf'^set\({re.escape(key)} "([^"]*)"\)$', path.read_text(encoding="utf-8"), re.MULTILINE
    )
    return match.group(1) if match else None


def _cmake_cache_value(cache: Path, key: str) -> str | None:
    if not cache.is_file():
        return None
    for line in cache.read_text(encoding="utf-8").splitlines():
        field, separator, value = line.partition("=")
        if separator and field.partition(":")[0] == key:
            return value
    return None


class BouffaloAdapter(PlatformAdapter):
    native_config = NativeConfigSpec("bouffalo", "defconfig")

    @classmethod
    def effective_sdk_log_level(cls, environment: Environment, requested: str) -> str:
        """Keep Bouffalo recovery images within their constrained flash budgets."""

        if (
            environment.sdk_target in {"bl602", "bl616", "bl618dg"}
            and environment.role == "safeboot"
        ):
            return "none"
        return requested

    @classmethod
    def sdk_log_settings(cls, level: str) -> Mapping[str, Any]:
        """Pass Bouffalo's numeric compile-time log threshold to CMake."""

        return {"CONFIG_LOG_LEVEL": SDK_LOG_LEVEL_VALUES[level]}

    @classmethod
    def board_native_settings(
        cls,
        environment: Environment,
        source: str,
        partition: Path | None = None,
    ) -> Mapping[str, Any]:
        if environment.sdk_board is None:
            raise BuildError(f"{source}: Bouffalo sdk_board is required")
        if partition is None or partition.suffix != ".toml":
            raise BuildError(f"{source}: Bouffalo requires a TOML partition default")
        return {"CONFIG_OVERRIDE_PTFILE": "partition.toml"}

    @classmethod
    def resolve_layout(
        cls, environment: Environment, sdk: SdkVersion, work_dir: Path
    ) -> LayoutFacts | None:
        """Resolve the native TOML table into normalized layout facts."""

        try:
            return resolve_layout(environment, sdk, work_dir)
        except BouffaloLayoutError as error:
            raise BuildError(str(error)) from error

    def __init__(self, context: BuildContext) -> None:
        super().__init__(context)
        self.native = context.work_dir / "native"
        self.project = context.work_dir / "project"
        self.environment: dict[str, str] = {}

    def prepare(self) -> None:
        if self.context.native_config.changed and self.native.exists():
            print("Discarding stale Bouffalo build tree after native config change.")
            shutil.rmtree(self.native)
        partition = self.context.environment.partition
        if partition is None:
            raise BuildError("Bouffalo partition default was not resolved")
        self.project = _prepare_project(self.context.work_dir, partition)

    def configure(self) -> None:
        sdk = self.context.sdk
        item = self.context.environment
        toolchain, _ = _toolchain()
        prefix = toolchain / "bin" / "riscv64-unknown-elf-"
        toolchain_file = sdk.source / "cmake" / "toolchain.cmake"
        cache = self.native / "CMakeCache.txt"
        configured_toolchain = _cmake_cache_value(cache, "CMAKE_TOOLCHAIN_FILE")
        if cache.is_file() and configured_toolchain != str(toolchain_file):
            print("Discarding stale Bouffalo CMake cache without the target toolchain.")
            shutil.rmtree(self.native)
        _prepare_config(self.native, self.context)
        self.environment = cross_build_environment()
        self.environment["BL_SDK_BASE"] = str(sdk.source)
        toolchain_bin = toolchain / "bin"
        self.environment["PATH"] = os.pathsep.join(
            (str(toolchain_bin), self.environment.get("PATH", ""))
        )
        layout = self.context.layout
        if layout is None:
            raise BuildError("Bouffalo configure called before layout resolution")
        try:
            filesystem = layout.region("littlefs")
            application = layout.region("application")
            safeboot = layout.region("safeboot")
        except ValueError as error:
            raise BuildError(str(error)) from error
        if item.partition is None:
            raise BuildError("Bouffalo OTA writer requires a resolved partition source")
        layout_digest = layout.identity
        run_configure = run if verbose_build_enabled() else self._logged_configure
        run_configure(
            [
                "cmake",
                "--no-warn-unused-cli",
                "-S",
                str(self.project),
                "-B",
                str(self.native),
                "-G",
                "Unix Makefiles",
                f"-DCROSS_COMPILE={prefix}",
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file}",
                f"-Dbouffalo_sdk_DIR={sdk.source / 'cmake'}",
                f"-DCHIP={item.sdk_target}",
                f"-DBOARD={item.sdk_board}",
                f"-DBUILD_DIR={self.native}",
                f"-DSDK_DEMO_PATH={self.project}",
                f"-DNEWMOTA_ROOT={ROOT}",
                f"-DNEWMOTA_SDK_ROOT={sdk.source}",
                f"-DNEWMOTA_ENV={item.name}",
                f"-DNEWMOTA_CHIP={item.chip}",
                f"-DNEWMOTA_SDK_VERSION={sdk.version}",
                f"-DNEWMOTA_SDK_REVISION={sdk.revision}",
                f"-DNEWMOTA_VERSION={self.context.version}",
                f"-DNEWMOTA_BUILD_DATE={self.context.build_date}",
                f"-DNEWMOTA_BUILD_TIME={self.context.build_time}",
                f"-DNEWMOTA_IMAGE_ROLE={item.role}",
                f"-DNEWMOTA_FILESYSTEM_OFFSET={filesystem.offset}",
                f"-DNEWMOTA_FILESYSTEM_SIZE={filesystem.size}",
                f"-DNEWMOTA_APPLICATION_OFFSET={application.offset}",
                f"-DNEWMOTA_APPLICATION_SIZE={application.size}",
                f"-DNEWMOTA_SAFEBOOT_OFFSET={safeboot.offset}",
                f"-DNEWMOTA_SAFEBOOT_SIZE={safeboot.size}",
                f"-DNEWMOTA_UPDATE_CHIP_ID={chip_id(item.chip)}",
                f"-DNEWMOTA_UPDATE_FLASH_BYTES={layout.flash_bytes}",
                f"-DNEWMOTA_UPDATE_LAYOUT_SHA256={layout_digest}",
                f"-DNEWMOTA_FEATURE_DIR={self.context.features.header.parent}",
                f"-DNEWMOTA_HARDWARE_DIR={self.context.hardware.header.parent}",
                f"-DNEWMOTA_BERRY_GENERATED_DIR={self.context.work_dir / 'generated' / 'berry'}",
                f"-DCONFIG_LOG_LEVEL={self.context.native_config.values['CONFIG_LOG_LEVEL']}",
                # Rule messages name each compiled source; run_build keeps them off the terminal.
                "-DCMAKE_RULE_MESSAGES=ON",
                *(
                    ["-DCONFIG_LOG_DISABLE=1"]
                    if self.context.native_config.sdk_log_level == "none"
                    else []
                ),
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            ],
            environment=self.environment,
        )

    def _logged_configure(self, command: list[str], environment: dict[str, str]) -> None:
        """Keep CMake's configure chatter in a log; show only its diagnostics."""

        run_build(
            command,
            label="Bouffalo configure",
            log=self.context.work_dir / "native-configure.log",
            environment=environment,
            status=no_status,
        )

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

    def build(self) -> None:
        for target in ("pre_build", None, "post_build", "combine"):
            command = ["cmake", "--build", str(self.native)]
            if target:
                command.extend(["--target", target])
            if self.context.jobs is not None:
                command.extend(["--parallel", str(self.context.jobs)])
            if verbose_build_enabled():
                command.extend(["--", "VERBOSE=1"])
                run(command, environment=self.environment)
                continue
            command.extend(["--", "-s", "--no-print-directory"])
            name = target or "application"
            run_build(
                command,
                label=f"Bouffalo {name}",
                log=self.context.work_dir / f"native-{name}.log",
                environment=self.environment,
                status=cmake_status,
            )

    def build_application(self) -> None:
        """Build the single application image used for a wired update."""

        self.build()

    def application_update(self) -> ApplicationUpdate:
        """Validate the application image against its resolved FW slot."""

        layout = self.context.layout
        if layout is None:
            raise BuildError("Bouffalo application output requested before prepare")
        try:
            application = layout.region("application")
        except ValueError as error:
            raise BuildError(str(error)) from error
        output = self.native / "build_out"
        stem = f"newmota_{self.context.environment.sdk_target}"
        images = sorted(output.glob(f"{stem}*.bin"))
        if not images:
            raise BuildError(f"{self.context.environment.name} did not produce a Bouffalo image")
        image = images[0]
        image_size = image.stat().st_size
        if image_size == 0 or image_size > application.size:
            raise BuildError(
                f"Bouffalo application image is {image_size} bytes but its slot allows "
                f"{application.size} bytes"
            )
        print(
            f"Application update: {image.relative_to(ROOT)} -> application at "
            f"{application.offset:#x} ({image_size}/{application.size} bytes)"
        )
        return ApplicationUpdate(image, application.offset, application.size)

    def compilation_database(self) -> Path:
        database = self.native / "compile_commands.json"
        if not database.is_file():
            raise BuildError("Bouffalo CMake did not generate compile_commands.json")
        return database

    def post_build(self) -> tuple[Artifact, ...]:
        output = self.native / "build_out"
        stem = f"newmota_{self.context.environment.sdk_target}"
        elf_files = sorted(output.glob(f"{stem}*.elf"))
        bin_files = sorted(output.glob(f"{stem}*.bin"))
        map_files = sorted(output.glob(f"{stem}*.map"))
        boot_files = sorted(output.glob("boot2_*.bin"))
        partition_files = sorted(output.glob("partition*.bin"))
        if not elf_files or not bin_files or not map_files:
            raise BuildError(
                f"{self.context.environment.name} did not produce Bouffalo ELF, BIN, and MAP files"
            )
        if len(bin_files) != 1 or len(boot_files) != 1 or len(partition_files) != 1:
            raise BuildError(
                f"{self.context.environment.name} did not produce exactly one application, "
                "boot2, and partition image"
            )
        layout = self.context.layout
        if layout is not None:
            try:
                budget = layout.region(
                    "safeboot" if self.context.environment.role == "safeboot" else "application"
                )
            except ValueError as error:
                raise BuildError(str(error)) from error
            image_size = bin_files[0].stat().st_size
            if image_size == 0 or image_size > budget.size:
                raise BuildError(
                    f"{self.context.environment.name} image is {image_size} bytes but "
                    f"its {budget.name} slot allows {budget.size} bytes"
                )
        return (
            Artifact(elf_files[0], "debug"),
            Artifact(bin_files[0], "native"),
            Artifact(boot_files[0], "native"),
            Artifact(partition_files[0], "native"),
            Artifact(map_files[0], "debug"),
        )

    def package(self) -> PackageResult:
        if self.context.environment.role == "safeboot":
            return validate_bouffalo_safeboot(self.context)
        return package_bouffalo(self.context)


def native_requirements(
    environment: Environment, values: dict[str, bool], layout: LayoutFacts | None
) -> tuple[tuple[str, ...], tuple[str, ...], dict[str, str]]:
    """Translate common feature selection into this SDK's sources and settings."""
    if environment.sdk_target != "bl602" and values.get("USE_WS2812", False):
        raise BuildError("Bouffalo WS2812 requires the BL602 SPI/DMA waveform adapter")
    if environment.sdk_target != "bl602" and values.get("USE_I2S", False):
        raise BuildError("Bouffalo I2S is not implemented for this target")
    if values.get("USE_DISPLAY_QSPI", False) and environment.sdk_target not in {"bl616", "bl618dg"}:
        raise BuildError("USE_DISPLAY_QSPI requires Bouffalo BL616 or BL618DG")
    sources = [
        "platform/bouffalo/digital_gpio.cpp",
        "platform/bouffalo/platform.cpp",
        "platform/bouffalo/native/entry.cpp",
    ]
    if environment.sdk_target == "bl602":
        sources.append("platform/bouffalo/atomic_bl602.c")
    if environment.role == "safeboot":
        sources.append("platform/bouffalo/recovery_update_writer.cpp")
    if values.get("USE_DEEPSLEEP", False):
        sources.append("platform/bouffalo/deep_sleep.cpp")
    sdk_requirements: list[str] = []
    native_settings: dict[str, str] = {}
    if values.get("USE_SDCARD", False) and environment.sdk_target in {"bl616", "bl618dg"}:
        # SDIO cards on the SDH controller; polling needs no SDH interrupt or semaphore.
        native_settings.update({"CONFIG_BSP_SDH_SD": "y", "CONFIG_BSP_SDH_OSAL_POLLING_MODE": "y"})
    if values.get("USE_BLE", False):
        if environment.role != "application" or environment.sdk_target not in {"bl602", "bl616"}:
            raise BuildError(
                "Bouffalo BLE requires a BL602 or BL616 application; BL618DG is deferred"
            )
        sources.append("platform/bouffalo/ble.cpp")
        native_settings.update(
            {
                "CONFIG_BLUETOOTH": "y",
                "CONFIG_NIMBLE": "n",
                # The SDK EasyFlash owner must never access Newmota's partitions.
                "CONFIG_BT_SETTINGS": "n",
                "CONFIG_BT_STACK_CLI": "n",
                # Seed the native PRNG through HCI LE Rand, not the SDK ble_rand shim.
                "CONFIG_BT_GEN_RANDOM_BY_SW": "y",
            }
        )
        if environment.sdk_target == "bl602":
            native_settings["CONFIG_BLECONTROLLER_LIB"] = '"m1s1"'
        else:
            # Retain the shipped archive's ten-link pools and 32 KiB exchange
            # RAM even though the adapter admits only one connection at a time.
            native_settings["CONFIG_BTBLECONTROLLER_LIB"] = '"ble1m10s1bredr0"'
    if values.get("USE_PWM", False) or values.get("USE_WS2812", False):
        sources.append("platform/bouffalo/pwm.cpp")
        if environment.sdk_target == "bl616":
            sources.append("platform/bouffalo/pwm_v2.cpp")
    if layout is not None:
        sources.extend(
            (
                "app/core/storage/littlefs_crc.cpp",
                "app/support/crc32_portable.cpp",
                "platform/bouffalo/filesystem.cpp",
                "third_party/littlefs/lfs.c",
            )
        )
    else:
        sources.append("app/support/crc32_portable.cpp")
    if (
        values.get("USE_MQTT", False)
        or values.get("USE_OTA_PULL", False)
        or (values.get("USE_I2S", False) and values.get("USE_WIFI", False))
    ):
        sources.append("platform/lwip/tcp_stream.cpp")
    if values.get("USE_WEB", False) or values.get("USE_RECOVERY_WEB", False):
        sources.append("platform/bouffalo/web.cpp")
    if values.get("USE_WIFI_WPA3_BEARSSL", False):
        if environment.sdk_target == "bl602":
            # The fork's CONFIG_WIFI4_EXTERNAL_HASH takes the supplicant's MD5, SHA-1 and
            # SHA-256 vectors from hostap_symmetric_bearssl.c instead of mbedTLS. SAE and AES
            # stay on the SDK's blcrypto_suite (features.py drops hostap_sae_crypto.c).
            native_settings["CONFIG_WIFI4_EXTERNAL_HASH"] = "y"
        else:
            # The fork's CONFIG_WIFI6_EXTERNAL_CRYPTO builds the supplicant without its mbedTLS
            # adapter; src/platform/crypto/hostap_*.c provide the functions instead.
            sources.append("platform/crypto/hostap_hash_bearssl.c")
            native_settings.update(
                {
                    "CONFIG_WIFI6_EXTERNAL_CRYPTO": "y",
                    # BearSSL's P-256 code keeps its work on the stack; libfhost gives the
                    # supplicant task 1408 words. A WPA2 join used 5268 B; the static SAE
                    # path adds about 3.6 KB more, so 2048 words would be too tight.
                    "CONFIG_WIFI6_WPA_STACK_DEPTH": "3072",
                }
            )
    # The supplicants' stock crypto backends and HTTPS link mbedTLS; with the BearSSL
    # backend and no HTTPS nothing in the image does.
    native_settings["CONFIG_MBEDTLS"] = (
        "n"
        if values.get("USE_WIFI_WPA3_BEARSSL", False) and not values.get("USE_WEB_HTTPS", False)
        else "y"
    )
    if values.get("USE_WEB_HTTPS", False):
        sources.append("platform/web_tls_socket.cpp")
        native_settings.update(
            {
                "CONFIG_MBEDTLS": "y",
                "CONFIG_MBEDTLS_PEM_CERTIFICATE_FORMAT": "y",
                "CONFIG_MBEDTLS_PK_WRITE_C": "y",
                "CONFIG_MBEDTLS_X509_CSR_WRITE_C": "y",
                "CONFIG_MBEDTLS_X509_CRT_WRITE_C": "y",
            }
        )
    return tuple(sources), tuple(sdk_requirements), native_settings


def native_target_issue(board: BoardDefinition) -> str | None:
    """Read-only check that the pinned SDK contains the selected target."""
    sdk = load_sdk_version("bouffalo").source
    target = board.chip.sdk_target
    target_dir = sdk / "drivers" / "soc" / target
    if not target_dir.is_dir():
        return f"missing Bouffalo target {display_path(target_dir)}"
    board_dir = sdk / "bsp" / "board" / str(board.sdk_board)
    return None if board_dir.is_dir() else f"missing Bouffalo board {display_path(board_dir)}"


def board_issues(board: BoardDefinition) -> list[str]:
    """Additional native board constraints, beyond catalog and target validation."""
    return []
