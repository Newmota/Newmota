"""Beken SDK setup and native build adapter."""

from __future__ import annotations

import json
import shutil
import subprocess
from collections.abc import Mapping
from pathlib import Path

from newmota.berry import generate_berry, stage_berry_generated_headers, stage_berry_runtime
from newmota.compdb import recorder_command, write_recorded_database
from newmota.config import SDK_LOG_LEVEL_VALUES
from newmota.host import (
    SdkVersion,
    SdkVersionError,
    arm_none_eabi_toolchain,
    check_sdk_version,
    compile_status_compiler,
    copy_if_changed,
    copy_tree,
    cross_build_environment,
    display_path,
    ensure_sdk_source,
    install_arm_none_eabi_toolchain,
    load_sdk_version,
    run,
    run_build,
    stage_project_sources,
    tool_path,
    verbose_build_enabled,
    verify_toolchain,
    write_if_changed,
)
from newmota.i18n import generate_beken_web_assets
from newmota.lvgl import LVGL_INCLUDE_DIRS, stage_lvgl_runtime
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
from newmota.package import LayoutFacts, create_filesystem_image
from newmota.platforms.beken.bdk import generate_makefile, validate_runtime_symbols
from newmota.platforms.beken.codec import (
    BekenImageSet,
    load_image_config,
    maximum_ota_raw_size,
    ota_compatibility_tag,
    package_images,
)
from newmota.platforms.beken.images import package_beken
from newmota.platforms.beken.images import resolve_layout as resolve_beken_layout
from newmota.platforms.beken.profile import TARGET_SPECS, BekenTargetProfile, resolve_target_profile

RBL_ENCRYPTION_CODES = {"none": 0, "xor": 1, "aes256": 2}
RBL_COMPRESSION_CODES = {"none": 0, "gzip": 1}

SOURCE_ALIASES = {
    "app.cpp": "newmota.cpp",
    "serial_console.cpp": "console.cpp",
    "crc32_portable.cpp": "crc32.cpp",
    "udp.cpp": "network_udp.cpp",
    "ping.cpp": "network_ping.cpp",
}
RELATIVE_SOURCE_ALIASES = {
    "src/platform/pwm.cpp": "newmota_pwm.cpp",
    "src/platform/beken/critical_settings.cpp": "beken_critical_settings.cpp",
    "src/platform/beken/filesystem.cpp": "beken_filesystem.cpp",
    "src/platform/beken/flash_access.cpp": "beken_flash_access.cpp",
    "src/platform/beken/net_param_tlv.cpp": "beken_net_param_tlv.cpp",
    "src/platform/beken/pwm.cpp": "beken_pwm.cpp",
}


def _destination() -> Path:
    return arm_none_eabi_toolchain()


def setup_issue(_chips: list[str]) -> str | None:
    try:
        check_sdk_version("beken")
        destination = _destination()
        if not destination.is_dir():
            return f"missing Beken toolchain {destination}"
        verify_toolchain(destination, "arm-none-eabi-")
    except (OSError, SdkVersionError, SetupUnavailable, subprocess.CalledProcessError) as error:
        return str(error)
    return None


def setup(_chips: list[str], dry_run: bool) -> None:
    version = load_sdk_version("beken")
    sdk = version.source
    destination = _destination()
    print(f"Beken SDK source: {sdk}")
    print(f"Beken SDK version: {version.version} ({version.revision[:12]})")
    print(f"Beken tools:      {destination}")
    ensure_sdk_source("beken", dry_run)
    compiler = install_arm_none_eabi_toolchain(dry_run)
    print(f"Beken compiler:   {compiler}")


def _prepare_overlay(
    context: BuildContext, profile: BekenTargetProfile
) -> tuple[Path, dict[Path, Path]]:
    if context.layout is None:
        raise BuildError("Beken overlay preparation requires resolved layout facts")
    overlay = context.work_dir / "overlay"
    application = overlay / "apps" / "newmota"
    source_dir = application / "src"
    include_dir = application / "include"
    source_dir.mkdir(parents=True, exist_ok=True)
    include_dir.mkdir(parents=True, exist_ok=True)
    sources = stage_project_sources(
        context,
        source_dir,
        include_dir,
        aliases=SOURCE_ALIASES,
        relative_aliases=RELATIVE_SOURCE_ALIASES,
    )
    for header in sorted((ROOT / "src" / "platform" / "beken").glob("*.h")):
        sources[header] = source_dir / header.name
        copy_if_changed(header, sources[header])
    if profile.printf_source is not None:
        sources[profile.printf_source] = source_dir / "sdk_printf.c"
        copy_if_changed(profile.printf_source, sources[profile.printf_source])
    sources.update(stage_berry_runtime(context, source_dir, include_dir))
    sources.update(stage_lvgl_runtime(context, source_dir))
    component_lines = [
        "# Newmota's LittleFS core selects an older LittleFS disk version.",
        "CFLAGS += -DLFS_MULTIVERSION",
    ]
    if context.features.enabled("USE_LVGL"):
        component_lines.append(
            "CFLAGS += "
            + " ".join(f"-Iapps/newmota/src/lvgl/{directory}" for directory in LVGL_INCLUDE_DIRS)
        )
    if context.features.enabled("USE_CRYPTO"):
        component_lines.append("CFLAGS += -DBR_LOMUL=1")
    if context.features.enabled("USE_MQTT"):
        component_lines.append("CFLAGS += -DMQTT_DO_NOT_USE_CUSTOM_CONFIG=1")
    if context.features.enabled("USE_BERRY"):
        component_lines.extend(
            (
                "# Berry's plain math module uses the toolchain math library.",
                "LIBFLAGS += -lm",
            )
        )
    write_if_changed(
        application / "components.mk",
        "\n".join(component_lines) + "\n",
        encoding="utf-8",
    )
    # Vendor dependency files cover headers, but not changed compiler/configuration flags.
    if context.native_config.changed:
        for pattern in ("*.o", "*.d"):
            for generated in source_dir.rglob(pattern):
                generated.unlink()
    copy_tree(ROOT / "src" / "app" / "include", include_dir)
    copy_tree(ROOT / "src" / "platform" / "include", include_dir)
    copy_if_changed(
        ROOT / "src" / "platform" / "beken" / "native" / "FreeRTOSConfig.h",
        include_dir / "FreeRTOSConfig.h",
    )
    _write_update_config(context, include_dir)
    for header in ("lfs.h", "lfs_util.h"):
        origin = ROOT / "src" / "third_party" / "littlefs" / header
        copy_if_changed(origin, source_dir / header)
        copy_if_changed(origin, include_dir / header)
    sdk_include = profile.sdk_root / "sdk" / "include"
    if sdk_include.is_dir():
        copy_tree(sdk_include, overlay / "sdk" / "include")
    return overlay, sources


def _write_update_config(context: BuildContext, include_dir: Path) -> None:
    """Generate the target-specific native RBL and flash-layout facts."""

    layout = context.layout
    if layout is None:
        raise BuildError("Beken update configuration requires resolved layout facts")
    image = load_image_config(context.environment)
    regions = (
        ("bootloader", layout.region("bootloader")),
        ("application", layout.region("application")),
        ("ota", layout.region("ota")),
        ("rf", layout.region("rf")),
        ("net", layout.region("net")),
        ("ble_bonding", layout.region("ble-bonding")),
        ("filesystem", layout.region("filesystem")),
    )
    region_lines = "\n".join(
        f"inline constexpr Region k{label.title().replace('_', '')}"
        f"{{{region.offset}U, {region.size}U}};"
        for label, region in regions
    )
    tag = ota_compatibility_tag(image, layout)
    content = f"""/* Generated for this resolved Beken image and native layout. */
#pragma once

#include <cstdint>

namespace newmota::platform::beken::update_config {{
struct Region final {{
    std::uint32_t offset;
    std::uint32_t size;
}};

inline constexpr std::uint32_t kFlashBytes = {layout.flash_bytes}U;
inline constexpr std::uint32_t kEraseSize = {layout.erase_size}U;
inline constexpr std::uint32_t kApplicationRawLimit = {maximum_ota_raw_size(image)}U;
inline constexpr std::uint8_t kRblEncryption = {RBL_ENCRYPTION_CODES[image.ota_encryption]}U;
inline constexpr std::uint8_t kRblCompression = {RBL_COMPRESSION_CODES[image.ota_compression]}U;
inline constexpr char kRblCompatibilityTag[] = "{tag}";
{region_lines}
}}  // namespace newmota::platform::beken::update_config
"""
    destination = include_dir / "newmota" / "platform" / "beken" / "update_config.h"
    write_if_changed(destination, content, encoding="utf-8")


def _remove_generated(path: Path) -> None:
    """Remove one generated build input without following a directory link."""

    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)


def _prepare_bdk_linker_script(source: Path, destination: Path) -> None:
    """Stage BDK linker scripts with ARM markers, constructors and no-init RAM.

    The released BDK script contains the ``.ARM.exidx`` section but omits the
    ``__exidx_*`` symbols expected by the ARM runtime pulled in by Newmota's
    C++ support.  Keep the SDK checkout immutable and make the small, explicit
    compatibility adjustments only in the generated build tree. Constructors
    belong in flash before the script moves its location counter into RAM. No-init
    RAM belongs after BSS and before ``_empty_ram``, which starts the SDK heap.
    """

    content = source.read_text(encoding="utf-8")
    if "__exidx_start" not in content and "__exidx_end" not in content:
        section = """\t.ARM.exidx :
\t{
\t    *(.ARM.exidx*)
\t    *(.gnu.linkonce.armexidx.*)
\t} > flash"""
        replacement = """\t.ARM.exidx :
\t{
\t    __exidx_start = .;
\t    *(.ARM.exidx*)
\t    *(.gnu.linkonce.armexidx.*)
\t    __exidx_end = .;
\t} > flash"""
        if section not in content:
            raise BuildError(f"unsupported Beken BDK linker script shape: {source}")
        content = content.replace(section, replacement, 1)
    elif "__exidx_start" not in content or "__exidx_end" not in content:
        raise BuildError(f"incomplete ARM exception markers in linker script: {source}")
    if "__init_array_start" not in content:
        anchor = "\t. = ORIGIN(ram);" if "\t. = ORIGIN(ram);" in content else "\t_begin_data = .;"
        if anchor not in content:
            raise BuildError(f"unsupported Beken constructor section placement: {source}")
        content = content.replace(
            anchor,
            """\t.init_array :
\t{
\t    . = ALIGN(4);
\t    __init_array_start = .;
\t    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
\t    KEEP(*(.init_array))
\t    __init_array_end = .;
\t} > flash

"""
            + anchor,
            1,
        )
    if "__newmota_noinit_start" not in content:
        anchor = "\t. = ALIGN (8);\n\t_empty_ram = .;"
        if content.count(anchor) != 1:
            raise BuildError(f"unsupported Beken no-init section placement: {source}")
        content = content.replace(
            anchor,
            """\t.noinit ALIGN(4) (NOLOAD) :
\t{
\t    __newmota_noinit_start = .;
\t    KEEP(*(.noinit .noinit.*))
\t    . = ALIGN(4);
\t    __newmota_noinit_end = .;
\t} > ram

"""
            + anchor,
            1,
        )
    write_if_changed(destination, content, encoding="utf-8")


def _prepare_native(build_dir: Path, profile: BekenTargetProfile, overlay: Path) -> Path:
    native = build_dir / "native"
    native.mkdir(parents=True, exist_ok=True)
    # The BDK checkout is immutable.  The generated build tree exposes it
    # under the paths expected by the Newmota-owned wrapper.
    for relative, target in (
        (Path("beken378"), profile.source_root),
        (Path("os"), profile.sdk_root / "os"),
        (Path("release"), profile.sdk_root / "release"),
        (Path("apps") / "newmota", overlay / "apps" / "newmota"),
    ):
        link = native / relative
        if link.is_symlink() or link.exists():
            _remove_generated(link)
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(target, target_is_directory=True)
    if profile.linker_source is None:
        raise BuildError(f"Beken BDK linker script is missing for {profile.spec.sdk_target}")
    _prepare_bdk_linker_script(
        profile.linker_source,
        native / "build" / profile.spec.linker_script,
    )
    write_if_changed(
        native / "config" / "sys_config.h",
        f"""#include "newmota_features.h"
#include "../beken378/app/config/sys_config_{profile.spec.config_target}.h"

/* Keep vendor Wi-Fi/PTA ABI settings intact while selecting BLE from the
 * common Newmota feature map. The target header enables BLE by default. */
#undef CFG_SUPPORT_BLE
#define CFG_SUPPORT_BLE USE_BLE
#undef CFG_USE_BLE_PS
#define CFG_USE_BLE_PS USE_BLE
#ifdef CFG_USE_BLE
#undef CFG_USE_BLE
#endif
#define CFG_USE_BLE USE_BLE

/* BK7252 and BK7252N enable the BDK SD-card host, audio, camera and video
 * demo code, which this build does not use; Newmota owns SD cards itself. */
#undef CFG_USE_SDCARD_HOST
#define CFG_USE_SDCARD_HOST 0
#undef CFG_USE_AUDIO
#define CFG_USE_AUDIO 0
#undef CFG_USE_CAMERA_INTF
#define CFG_USE_CAMERA_INTF 0
#undef CFG_USE_APP_DEMO_VIDEO_TRANSFER
#define CFG_USE_APP_DEMO_VIDEO_TRANSFER 0
""",
        encoding="utf-8",
    )
    (native / "Debug" / "obj").mkdir(parents=True, exist_ok=True)
    (native / "Debug" / "bin").mkdir(parents=True, exist_ok=True)
    return native


class BekenAdapter(PlatformAdapter):
    native_config = NativeConfigSpec("beken", "defconfig")
    # Beken images are tight in both the application and the update area.
    default_sdk_log_level = "none"

    @classmethod
    def sdk_log_settings(cls, level: str) -> Mapping[str, str]:
        """Expose the BDK's numeric threshold to its generated Makefile."""

        return {"CFG_LOG_LEVEL": str(SDK_LOG_LEVEL_VALUES[level])}

    @classmethod
    def resolve_layout(
        cls, environment: Environment, sdk: SdkVersion, work_dir: Path
    ) -> LayoutFacts:
        return resolve_beken_layout(environment, sdk, work_dir)

    def __init__(self, context: BuildContext) -> None:
        super().__init__(context)
        self.overlay = context.work_dir / "overlay"
        self.native = context.work_dir / "native"
        self.command: list[str] = []
        self.environment: dict[str, str] = {}
        self.compiler = ""
        self.compiler_arguments: list[str] = []
        self.source_map: dict[Path, Path] = {}
        self.filesystem_image = context.work_dir / "generated" / "littlefs.bin"

    def prepare(self) -> None:
        # make compares timestamps only, so objects built from another SDK source would be reused.
        self.context.work_dir.mkdir(parents=True, exist_ok=True)
        sdk_marker = self.context.work_dir / "beken-sdk.json"
        sdk_identity = {
            "key": self.context.sdk.key,
            "revision": self.context.sdk.revision,
            "source": str(self.context.sdk.source.resolve()),
        }
        previous_sdk = None
        if sdk_marker.is_file():
            previous_sdk = json.loads(sdk_marker.read_text(encoding="utf-8"))
        elif self.native.exists():
            previous_sdk = {}
        sdk_changed = previous_sdk is not None and previous_sdk != sdk_identity
        if sdk_changed:
            print("Discarding Beken build tree after SDK selection change.")
        elif self.context.native_config.changed:
            print("Discarding stale Beken build tree after native config change.")
        if sdk_changed or self.context.native_config.changed:
            shutil.rmtree(self.native, ignore_errors=True)
            shutil.rmtree(
                self.overlay / "apps" / "newmota" / "output",
                ignore_errors=True,
            )
        sdk_marker.write_text(json.dumps(sdk_identity, indent=2) + "\n", encoding="utf-8")
        profile = resolve_target_profile(self.context.environment, self.context.sdk)
        self.overlay, staged_files = _prepare_overlay(self.context, profile)
        self.source_map = {
            destination: source
            for source, destination in staged_files.items()
            if source.is_relative_to(ROOT / "src") and source.suffix == ".cpp"
        }
        self.native = _prepare_native(self.context.work_dir, profile, self.overlay)

    def configure(self) -> None:
        sdk = self.context.sdk
        profile = resolve_target_profile(self.context.environment, sdk)
        profile.require_build_support()
        prefix = _destination() / "bin" / "arm-none-eabi-"
        self.compiler = f"{prefix}gcc"
        # The compiler make runs as CC; build() wraps it to report each source.
        self.compile_command = self.compiler
        self.compiler_arguments = []
        makefile = generate_makefile(profile, self.context, self.native, self.overlay)
        self.command = [
            "make",
            "-j",
            str(self.context.jobs),
            "-C",
            str(self.native),
            "-f",
            str(makefile),
            "application",
            f"CROSS_COMPILE={prefix}",
        ]
        if not verbose_build_enabled():
            self.command[1:1] = ["-s", "--no-print-directory"]
        self.environment = cross_build_environment()

    def pre_build(self) -> None:
        generate_berry(self.context)
        stage_berry_generated_headers(
            self.context,
            self.overlay / "apps" / "newmota" / "include" / "berry" / "generated",
        )
        include = self.overlay / "apps" / "newmota" / "include"
        asset = include / "beken_web_assets.h"
        if self.context.features.enabled("USE_WEB"):
            # HTTPS applications embed Brotli; HTTP builds and Safeboot stay Gzip.
            copy_if_changed(
                generate_beken_web_assets(
                    self.context.work_dir / "config",
                    dict(self.context.features.assignments),
                    encoding="br" if self.context.features.enabled("USE_WEB_HTTPS") else "gzip",
                ),
                asset,
            )
        else:
            asset.unlink(missing_ok=True)
        # Remove headers from previous generic/second-page builds so Beken
        # embeds exactly one application page in this role.
        (include / "web_assets.h").unlink(missing_ok=True)
        (include / "beken_update_web_assets.h").unlink(missing_ok=True)
        if self.context.layout is None:
            raise BuildError("Beken pre-build requires resolved layout facts")
        create_filesystem_image(
            self.filesystem_image,
            self.context.layout.region("filesystem").size,
            self.context.environment.filesystem,
        )

    def build(self) -> None:
        if verbose_build_enabled():
            run(self.command, environment=self.environment)
            return
        # Report each compiled source on one status line, as the ESP-IDF build does.
        command = [argument for argument in self.command if not argument.startswith("CC=")]
        command.append(f"CC={compile_status_compiler(self.compile_command)}")
        run_build(
            command,
            label="Beken",
            log=self.context.work_dir / "native-build.log",
            environment=self.environment,
        )

    def compilation_database(self) -> Path:
        log = self.context.work_dir / "clangd-compile-records.jsonl"
        log.unlink(missing_ok=True)
        recorder = recorder_command(log, self.compiler, self.compiler_arguments)
        command = [argument for argument in self.command if not argument.startswith("CC=")]
        if any(argument.endswith("bdk.mk") for argument in command):
            # The recorder appends to one shared JSONL file; keep this diagnostic
            # compile serial even though the normal BDK build is parallel-safe.
            jobs_index = command.index("-j")
            del command[jobs_index : jobs_index + 2]
            command[1:1] = ["-j", "1"]
        command.append(f"CC={recorder}")
        run(command, environment=self.environment)
        return write_recorded_database(
            log,
            self.context.work_dir / "clangd-native-compile_commands.json",
            self.source_map,
        )

    def _package_images(self, binary: Path) -> BekenImageSet:
        if self.context.layout is None:
            raise BuildError("Beken image packaging requires resolved layout facts")
        if not binary.is_file():
            raise BuildError(f"Beken raw application image is missing: {binary}")
        application = self.context.layout.region("application")
        stem = binary.with_suffix("")
        application_image = Path(f"{stem}.0x{application.offset:06X}.rbl")
        ota_image = Path(f"{stem}.ota.rbl")
        return package_images(
            binary,
            application_image,
            ota_image,
            load_image_config(self.context.environment),
            self.context.layout,
            self.context.version,
        )

    def post_build(self) -> tuple[Artifact, ...]:
        output = self.overlay / "apps" / "newmota" / "output" / self.context.version
        stem = output / f"newmota_{self.context.version}"
        binary = Path(f"{stem}.bin")
        if self.context.layout is None:
            raise BuildError("Beken output validation requires resolved layout facts")
        symbol_table = subprocess.check_output(
            [
                str(tool_path(_destination(), "arm-none-eabi-", "nm")),
                "--defined-only",
                str(Path(f"{stem}.axf")),
            ],
            text=True,
        )
        validate_runtime_symbols(symbol_table)
        image_set = self._package_images(binary)
        application = self.context.layout.region("application")
        ota = self.context.layout.region("ota")
        print(
            f"Beken raw image: {binary.stat().st_size} bytes (diagnostic input); "
            f"factory app RBL: {image_set.application.stat().st_size}/"
            f"{application.size} bytes; OTA RBL: {image_set.ota.stat().st_size}/"
            f"{ota.size} bytes"
        )
        return (
            Artifact(Path(f"{stem}.axf"), "debug"),
            Artifact(binary, "native"),
            Artifact(Path(f"{stem}.map"), "debug"),
            Artifact(Path(f"{stem}.asm"), "debug"),
            Artifact(image_set.application, "factory-image"),
            Artifact(image_set.ota, "ota-image"),
            Artifact(self.filesystem_image, "filesystem-init"),
        )

    def build_application(self) -> None:
        """Reuse the native application target for stock OTA staging updates."""

        self.build()

    def application_update(self) -> ApplicationUpdate:
        """Describe a stock Beken OTA write, which may overwrite LittleFS."""

        if self.context.layout is None:
            raise BuildError("Beken application update requires resolved layout facts")
        output = self.overlay / "apps" / "newmota" / "output" / self.context.version
        binary = Path(output / f"newmota_{self.context.version}.bin")
        if not binary.is_file():
            raise BuildError(f"Beken application update image is missing: {binary}")
        image_set = self._package_images(binary)
        ota = self.context.layout.region("ota")
        limit = ota.image_budget or ota.size
        print(
            f"Application update: {image_set.ota.relative_to(ROOT)} -> Beken OTA at "
            f"{ota.offset:#x} ({image_set.ota.stat().st_size}/{limit} bytes)"
        )
        return ApplicationUpdate(image_set.ota, ota.offset, limit)

    def package(self) -> PackageResult:
        if self.context.layout is None:
            raise BuildError("Beken packaging requires resolved layout facts")
        return package_beken(self.context, self.context.layout)


def native_requirements(
    environment: Environment, values: dict[str, bool], layout: LayoutFacts | None
) -> tuple[tuple[str, ...], tuple[str, ...], dict[str, str]]:
    """Translate common feature selection into this SDK's sources and settings."""
    sources = [
        "platform/beken/digital_gpio.cpp",
        "platform/beken/platform.cpp",
        "platform/beken/native/atomic.c",
        "platform/beken/native/beken_app.c",
        "platform/beken/native/entry.cpp",
    ]
    if not values.get("USE_BLE", False):
        # The immutable Wi-Fi and calibration archives retain a small subset
        # of the vendor BLE ABI even when the BLE stack is not selected.
        sources.append("platform/beken/native/ble_disabled.c")
    sdk_requirements: list[str] = []
    native_settings: dict[str, str] = {}
    if values.get("USE_PWM", False):
        sources.append("platform/beken/pwm.cpp")
    if values.get("USE_DEEPSLEEP", False):
        sources.append("platform/beken/deep_sleep.cpp")
    if layout is not None:
        sources.extend(
            (
                "app/core/storage/littlefs_crc.cpp",
                "app/support/crc32_portable.cpp",
                "platform/beken/flash_access.cpp",
                "platform/beken/filesystem.cpp",
                "platform/beken/native/bdk_compat.c",
                "platform/beken/native/libc_heap.c",
                "platform/beken/native/libc_syscalls.c",
                "third_party/littlefs/lfs.c",
            )
        )
        if environment.role == "application":
            sources.append("platform/beken/update_writer.cpp")
        if values["NEWMOTA_CRITICAL_SETTINGS_STORE"]:
            sources.extend(
                (
                    "platform/beken/net_param_tlv.cpp",
                    "platform/beken/critical_settings.cpp",
                )
            )
    else:
        sources.append("app/support/crc32_portable.cpp")
    if values.get("USE_MQTT", False) or values.get("USE_OTA_PULL", False):
        sources.append("platform/lwip/tcp_stream.cpp")
    if values.get("USE_WEB", False):
        sources.append("platform/beken/web.cpp")
    if values.get("USE_WEB_HTTPS", False):
        sources.append("platform/web_tls_socket.cpp")
    return tuple(sources), tuple(sdk_requirements), native_settings


def native_target_issue(board: BoardDefinition) -> str | None:
    """Read-only check that the pinned SDK contains the selected target."""
    target = board.chip.sdk_target
    sdk = load_sdk_version("beken").source
    bdk_root = sdk / "beken378"
    config_target = TARGET_SPECS[target].config_target
    config = bdk_root / "app" / "config" / f"sys_config_{config_target}.h"
    if bdk_root.is_dir():
        return (
            None
            if config.is_file()
            else f"missing Beken BDK target configuration {display_path(config)}"
        )
    expected = sdk / "platforms" / target
    return None if expected.is_dir() else f"missing Beken target {display_path(expected)}"


def board_issues(board: BoardDefinition) -> list[str]:
    """Additional native board constraints, beyond catalog and target validation."""
    return []
