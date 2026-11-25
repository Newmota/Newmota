"""Raspberry Pi Pico SDK setup and native build adapter."""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from newmota.berry import generate_berry
from newmota.host import (
    SdkVersion,
    SdkVersionError,
    arm_none_eabi_toolchain,
    check_sdk_version,
    cmake_status,
    cross_build_environment,
    display_path,
    ensure_sdk_source,
    find_ccache,
    host_key,
    install_archive_bundle,
    install_arm_none_eabi_toolchain,
    load_sdk_version,
    no_status,
    run,
    run_build,
    tools_root,
    verbose_build_enabled,
    verify_toolchain,
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
from newmota.platforms.pico.images import (
    PicoLayoutError,
    package_pico,
    resolve_layout,
    validate_pico_safeboot,
)

PICOTOOL_PACKAGES = {
    ("2.3.1", "darwin-arm64"): (
        "mac.zip",
        "7bb3cea5d9f1b000fc758cf4b54774fda5c16a9899d342ef6b0c30138988793b",
    ),
    ("2.3.1", "darwin-x64"): (
        "mac.zip",
        "7bb3cea5d9f1b000fc758cf4b54774fda5c16a9899d342ef6b0c30138988793b",
    ),
    ("2.3.1", "linux-arm64"): (
        "aarch64-lin.tar.gz",
        "8cce5579c92e77c0439f34d4f49d37b1dc7d1e7c455233398f2e7b49599f67aa",
    ),
    ("2.3.1", "linux-x64"): (
        "x86_64-lin.tar.gz",
        "878a1a1e7d8336b8be60300c7218f366d9790c0d3a211b181d6d172e80b1248e",
    ),
    ("2.3.1", "win32-x64"): (
        "x64-win.zip",
        "68730be0813f8f35be2cca147cf7f1572662d5dcf0f5ba468e02a6dd9e85db2b",
    ),
}


def _picotool_root(version: str) -> Path:
    return tools_root() / "pico-sdk" / f"picotool-{version}"


def _picotool_executable(version: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return _picotool_root(version) / "picotool" / f"picotool{suffix}"


def _verify_picotool(version: str) -> None:
    executable = _picotool_executable(version)
    if not executable.is_file():
        raise SetupUnavailable(f"picotool executable is missing: {executable}")
    subprocess.run(
        [str(executable), "version", version],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def _install_picotool(sdk: SdkVersion, dry_run: bool) -> Path:
    if sdk.programmer != "picotool" or sdk.programmer_status == "unselected":
        raise SetupUnavailable("Pico SDK requires a selected picotool pin")
    host = host_key()
    version = sdk.programmer_version
    package = PICOTOOL_PACKAGES.get((version, host))
    if package is None:
        raise SetupUnavailable(f"picotool {version} has no managed package for {host}")
    archive_suffix, sha256 = package
    archive_name = f"picotool-{version}-{archive_suffix}"
    destination = install_archive_bundle(
        "https://github.com/raspberrypi/pico-sdk-tools/releases/download/"
        f"v{version}-0/{archive_name}",
        sha256,
        _picotool_root(version),
        dry_run,
    )
    if not dry_run:
        executable = _picotool_executable(version)
        if os.name != "nt" and executable.is_file():
            executable.chmod(executable.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        _verify_picotool(version)
    return destination


def setup_issue(_chips: list[str]) -> str | None:
    try:
        sdk = check_sdk_version("pico-sdk")
        for relative in (
            "lib/btstack",
            "lib/cyw43-driver",
            "lib/lwip",
            "lib/mbedtls",
            "lib/tinyusb",
        ):
            dependency = sdk.source / relative
            if not dependency.is_dir() or not any(dependency.iterdir()):
                return f"missing Pico SDK dependency {dependency}"
        toolchain = arm_none_eabi_toolchain()
        if not toolchain.is_dir():
            return f"missing Arm GNU toolchain {toolchain}"
        verify_toolchain(toolchain, "arm-none-eabi-")
        _verify_picotool(sdk.programmer_version)
    except (OSError, SdkVersionError, SetupUnavailable, subprocess.CalledProcessError) as error:
        return str(error)
    return None


def setup(_chips: list[str], dry_run: bool) -> None:
    version = load_sdk_version("pico-sdk")
    sdk = version.source
    print(f"Pico SDK source:  {sdk}")
    print(f"Pico SDK version: {version.version} ({version.revision[:12]})")
    print(f"Pico tools:       {arm_none_eabi_toolchain()}")
    ensure_sdk_source("pico-sdk", dry_run)
    compiler = install_arm_none_eabi_toolchain(dry_run)
    print(f"Pico compiler:    {compiler}")
    picotool = _install_picotool(version, dry_run)
    print(f"Pico picotool:    {picotool}")


class PicoSdkAdapter(PlatformAdapter):
    native_config = NativeConfigSpec("pico-sdk", "cmake.defaults")

    @classmethod
    def sdk_log_settings(cls, level: str) -> Mapping[str, int]:
        """Translate the policy into Pico SDK logging configuration macros."""

        quiet = level in {"none", "error", "warn"}
        return {
            "PICO_CYW43_ARCH_DEBUG_ENABLED": int(not quiet),
            "PICO_CYW43_LOGGING_ENABLED": int(not quiet),
            "PICO_DEBUG_MALLOC": int(level in {"debug", "verbose"}),
        }

    @classmethod
    def board_native_settings(
        cls,
        environment: Environment,
        source: str,
        partition: Path | None = None,
    ) -> Mapping[str, Any]:
        if environment.sdk_board is None:
            raise BuildError(f"{source}: Pico SDK sdk_board is required")
        if partition is None or partition.suffix != ".json":
            raise BuildError(f"{source}: Pico SDK requires a JSON partition default")
        return {"PICO_BOARD": environment.sdk_board}

    @classmethod
    def resolve_layout(
        cls, environment: Environment, sdk: SdkVersion, work_dir: Path
    ) -> LayoutFacts | None:
        """Resolve this target's checked Pico flash map into normalized layout facts."""

        try:
            return resolve_layout(environment, sdk, work_dir)
        except PicoLayoutError as error:
            raise BuildError(str(error)) from error

    def __init__(self, context: BuildContext) -> None:
        super().__init__(context)
        self.native = context.work_dir / "native"
        self.environment: dict[str, str] = {}

    def prepare(self) -> None:
        if self.context.native_config.changed and self.native.exists():
            print("Discarding stale Pico SDK build tree after native config change.")
            shutil.rmtree(self.native)
        self.context.work_dir.mkdir(parents=True, exist_ok=True)

    def configure(self) -> None:
        sdk = self.context.sdk
        item = self.context.environment
        values = self.context.native_config.values
        if values.get("PICO_PLATFORM") != item.sdk_target:
            raise BuildError(f"{item.name} native PICO_PLATFORM does not match {item.sdk_target}")
        if values.get("PICO_BOARD") != item.sdk_board:
            raise BuildError(f"{item.name} native PICO_BOARD does not match {item.sdk_board}")

        self.environment = cross_build_environment()
        self.environment["PICO_SDK_PATH"] = str(sdk.source)
        self.environment["PICO_TOOLCHAIN_PATH"] = str(arm_none_eabi_toolchain())
        self.environment["PICOTOOL_FETCH_FROM_GIT_PATH"] = str(
            _picotool_root(sdk.programmer_version)
        )
        layout = self.context.layout
        if layout is None:
            raise BuildError("Pico SDK configure called before layout resolution")
        try:
            filesystem = layout.region("littlefs")
            application = layout.region("application")
            safeboot = layout.region("safeboot")
            marker = layout.region("boot-marker")
        except ValueError as error:
            raise BuildError(str(error)) from error
        rp2040 = item.sdk_target == "rp2040"
        if rp2040:
            data_size = layout.flash_bytes - marker.offset
            # The RP2040 has no ROM partition remapping.  The application is
            # linked at its physical XIP address and Safeboot transfers to its
            # vector table after the SDK's 256-byte boot2 prefix.
            slot_base = 0x10000000 + (
                safeboot.offset if item.role == "safeboot" else application.offset
            )
        else:
            try:
                e10 = layout.region("e10-reserve")
            except ValueError as error:
                raise BuildError(str(error)) from error
            data_size = e10.offset - marker.offset
            # RP2350 rom_chain_image() rolls the selected physical partition
            # into XIP_BASE, so both roles link at the same runtime address.
            slot_base = 0x10000000
        partition = item.partition
        if partition is None:
            raise BuildError("Pico SDK partition default was not resolved")
        layout_digest = layout.identity
        if item.role == "safeboot":
            slot_size = safeboot.size
        else:
            slot_size = application.size
        command = [
            "cmake",
            "--no-warn-unused-cli",
            "-S",
            str(ROOT / "src" / "platform" / "pico-sdk" / "native"),
            "-B",
            str(self.native),
            "-G",
            "Unix Makefiles",
            "-DCMAKE_BUILD_TYPE=MinSizeRel",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            # Rule messages name each compiled source; run_build keeps them off the terminal.
            "-DCMAKE_RULE_MESSAGES=ON",
            f"-DNEWMOTA_ROOT={ROOT}",
            f"-DNEWMOTA_ENV={item.name}",
            f"-DNEWMOTA_CHIP={item.chip}",
            f"-DNEWMOTA_SDK_VERSION={sdk.version}",
            f"-DNEWMOTA_PICO_BLE_SDK_REVISION={sdk.revision}",
            f"-DNEWMOTA_VERSION={self.context.version}",
            f"-DNEWMOTA_BUILD_DATE={self.context.build_date}",
            f"-DNEWMOTA_BUILD_TIME={self.context.build_time}",
            f"-DNEWMOTA_IMAGE_ROLE={item.role}",
            f"-DNEWMOTA_USE_WEB_HTTPS={'ON' if self.context.features.enabled('USE_WEB_HTTPS') else 'OFF'}",
            f"-DNEWMOTA_USE_AUDIO_OPUS={'ON' if self.context.features.enabled('USE_AUDIO_OPUS') else 'OFF'}",
            f"-DNEWMOTA_PICO_RP2040={'ON' if rp2040 else 'OFF'}",
            f"-DNEWMOTA_PARTITION_JSON={partition}",
            f"-DNEWMOTA_SLOT_BASE={slot_base:#x}",
            f"-DNEWMOTA_SLOT_SIZE={slot_size:#x}",
            f"-DNEWMOTA_FILESYSTEM_OFFSET={filesystem.offset:#x}",
            f"-DNEWMOTA_FILESYSTEM_SIZE={filesystem.size:#x}",
            f"-DNEWMOTA_FILESYSTEM_BLOCK_SIZE={layout.erase_size:#x}",
            f"-DNEWMOTA_APPLICATION_OFFSET={application.offset:#x}",
            f"-DNEWMOTA_APPLICATION_SIZE={application.size:#x}",
            f"-DNEWMOTA_SAFEBOOT_OFFSET={safeboot.offset:#x}",
            f"-DNEWMOTA_SAFEBOOT_SIZE={safeboot.size:#x}",
            f"-DNEWMOTA_BOOT_MARKER_OFFSET={marker.offset:#x}",
            f"-DNEWMOTA_DATA_OFFSET={marker.offset:#x}",
            f"-DNEWMOTA_DATA_SIZE={data_size:#x}",
            f"-DNEWMOTA_UPDATE_CHIP_ID={chip_id(item.chip)}",
            f"-DNEWMOTA_UPDATE_FLASH_BYTES={layout.flash_bytes}",
            f"-DNEWMOTA_UPDATE_LAYOUT_SHA256={layout_digest}",
            f"-DNEWMOTA_FEATURE_DIR={self.context.features.header.parent}",
            f"-DNEWMOTA_HARDWARE_DIR={self.context.hardware.header.parent}",
            f"-DNEWMOTA_BERRY_GENERATED_DIR={self.context.work_dir / 'generated' / 'berry'}",
        ]
        command.extend(f"-D{key}={value}" for key, value in self.context.native_config.assignments)
        cache = find_ccache()
        if cache:
            command.extend(
                [
                    f"-DCMAKE_C_COMPILER_LAUNCHER={cache}",
                    f"-DCMAKE_CXX_COMPILER_LAUNCHER={cache}",
                ]
            )
        if verbose_build_enabled():
            run(command, environment=self.environment)
            return
        run_build(
            command,
            label="Pico configure",
            log=self.context.work_dir / "native-configure.log",
            environment=self.environment,
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
        command = ["cmake", "--build", str(self.native)]
        if self.context.jobs is not None:
            command.extend(["--parallel", str(self.context.jobs)])
        if verbose_build_enabled():
            command.extend(["--", "VERBOSE=1"])
            run(command, environment=self.environment)
            return
        command.extend(["--", "-s", "--no-print-directory"])
        run_build(
            command,
            label="Pico",
            log=self.context.work_dir / "native-build.log",
            environment=self.environment,
            status=cmake_status,
        )

    def build_application(self) -> None:
        """Build the single application image used for a wired update."""

        self.build()

    def application_update(self) -> ApplicationUpdate:
        """Validate the application image against its resolved slot."""

        layout = self.context.layout
        if layout is None:
            raise BuildError("Pico application output requested before prepare")
        try:
            application = layout.region("application")
        except ValueError as error:
            raise BuildError(str(error)) from error
        image = self.native / "newmota.bin"
        if not image.is_file():
            raise BuildError(f"{self.context.environment.name} did not produce a Pico image")
        image_size = image.stat().st_size
        if image_size == 0 or image_size > application.size:
            raise BuildError(
                f"Pico application image is {image_size} bytes but its slot allows "
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
            raise BuildError("Pico SDK CMake did not generate compile_commands.json")
        return database

    def post_build(self) -> tuple[Artifact, ...]:
        elf = self.native / "newmota.elf"
        binary = self.native / "newmota.bin"
        uf2 = self.native / "newmota.uf2"
        if not elf.is_file() or not binary.is_file() or not uf2.is_file():
            raise BuildError(
                f"{self.context.environment.name} did not produce Pico ELF, BIN, and UF2 files"
            )
        layout = self.context.layout
        if layout is not None:
            try:
                budget = layout.region(
                    "safeboot" if self.context.environment.role == "safeboot" else "application"
                )
            except ValueError as error:
                raise BuildError(str(error)) from error
            image_size = binary.stat().st_size
            if image_size == 0 or image_size > budget.size:
                raise BuildError(
                    f"{self.context.environment.name} image is {image_size} bytes but "
                    f"its {budget.name} slot allows {budget.size} bytes"
                )
        return (
            Artifact(elf, "debug"),
            Artifact(binary, "native"),
            Artifact(uf2, "native"),
        )

    def package(self) -> PackageResult:
        if self.context.environment.role == "safeboot":
            return validate_pico_safeboot(self.context)
        return package_pico(self.context)


def native_requirements(
    environment: Environment, values: dict[str, bool], layout: LayoutFacts | None
) -> tuple[tuple[str, ...], tuple[str, ...], dict[str, str]]:
    """Translate common feature selection into this SDK's sources and settings."""
    sources = [
        "platform/pico-sdk/digital_gpio.cpp",
        "platform/pico-sdk/boot_trace.cpp",
        "platform/pico-sdk/flash_access.cpp",
        "platform/pico-sdk/platform.cpp",
        "platform/pico-sdk/native/entry.cpp",
    ]
    if environment.role == "safeboot":
        sources.append("platform/pico-sdk/recovery_update_writer.cpp")
    sdk_requirements: list[str] = []
    if values.get("USE_DEEPSLEEP", False):
        sources.append("platform/pico-sdk/deep_sleep.cpp")
        sdk_requirements.append("pico_aon_timer")
        if environment.sdk_target.startswith("rp2350"):
            sdk_requirements.append("hardware_powman")
    if values.get("USE_ADC", False):
        sdk_requirements.append("hardware_adc")
    if values.get("USE_I2S", False):
        sdk_requirements.extend(("hardware_pio", "hardware_dma", "hardware_clocks", "hardware_irq"))
    if values.get("USE_CMSIS_DSP", False):
        # CMSIS-DSP takes its compiler intrinsics from the SDK's CMSIS-Core headers.
        sdk_requirements.append("cmsis_core_headers")
    native_settings: dict[str, str] = {}
    if values.get("USE_BLE", False):
        if environment.sdk_board not in ("pico_w", "pico2_w"):
            raise BuildError("Pico BLE requires a supported Pico W or Pico 2 W radio board")
        sources.append("platform/pico-sdk/ble.cpp")
        sdk_requirements.extend(
            (
                "pico_btstack_ble",
                "pico_btstack_hci_transport_cyw43",
                "pico_btstack_run_loop_async_context",
                "pico_rand",
            )
        )
    if values.get("USE_DISPLAY", False) or values.get("USE_SDCARD", False):
        # Both reach SPI0/SPI1 through platform/pico-sdk/spi_controller.cpp.
        sdk_requirements.extend(("hardware_spi", "hardware_clocks"))
    if values.get("USE_DISPLAY_QSPI", False):
        sdk_requirements.extend(("hardware_pio", "hardware_dma"))
    if values.get("USE_SERIAL", False):
        sdk_requirements.append("hardware_uart")
    if values.get("USE_WS2812", False):
        sdk_requirements.extend(("hardware_pio", "hardware_dma", "hardware_clocks"))
    if values.get("USE_PWM", False) or values.get("USE_WS2812", False):
        sources.append("platform/pico-sdk/pwm.cpp")
        sdk_requirements.append("hardware_pwm")
    if layout is not None:
        sources.extend(
            (
                "app/core/storage/littlefs_crc.cpp",
                "app/support/crc32_portable.cpp",
                "platform/pico-sdk/filesystem.cpp",
                "third_party/littlefs/lfs.c",
            )
        )
    else:
        sources.append("app/support/crc32_portable.cpp")
    sdk_requirements.extend(
        (
            "pico_stdlib",
            "hardware_watchdog",
            "hardware_flash",
            "pico_flash",
            "pico_bootrom",
            "m",
        )
    )
    if values["USE_WIFI"]:
        if values.get("USE_WEB_HTTPS", False):
            # Mbed TLS performs its altcp handshake in lwIP callbacks. The
            # background architecture runs those callbacks from a
            # low-priority IRQ, so HTTPS services lwIP from the app loop.
            sdk_requirements.append("pico_cyw43_arch_lwip_poll")
        else:
            sdk_requirements.append("pico_cyw43_arch_lwip_threadsafe_background")
        sources.append("platform/pico-sdk/dhcp_server.cpp")
    if (
        values.get("USE_MQTT", False)
        or values.get("USE_OTA_PULL", False)
        or (values.get("USE_I2S", False) and values.get("USE_WIFI", False))
    ):
        sources.append("platform/lwip/tcp_stream.cpp")
    if values.get("USE_WEB", False) or values.get("USE_RECOVERY_WEB", False):
        sources.append("platform/pico-sdk/web.cpp")
    if values.get("USE_WEB_HTTPS", False):
        sdk_requirements.extend(("pico_mbedtls", "pico_lwip_mbedtls"))
    return tuple(sources), tuple(sdk_requirements), native_settings


def native_target_issue(board: BoardDefinition) -> str | None:
    """Read-only check that the pinned SDK contains the selected target."""
    sdk = load_sdk_version("pico-sdk").source
    target = board.chip.sdk_target
    platform_name = "rp2350" if target.startswith("rp2350-") else target
    platform_dir = sdk / "src" / platform_name
    if not platform_dir.is_dir():
        return f"missing Pico SDK target {display_path(platform_dir)}"
    board_header = sdk / "src" / "boards" / "include" / "boards" / f"{board.sdk_board}.h"
    return (
        None if board_header.is_file() else f"missing Pico SDK board {display_path(board_header)}"
    )


def board_issues(board: BoardDefinition) -> list[str]:
    """Additional native board constraints, beyond catalog and target validation."""
    return []
