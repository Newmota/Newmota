"""Realtek AmebaD setup and native build adapter."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from collections.abc import Mapping
from pathlib import Path

from newmota.audio import ameba_codec_settings, stage_audio_runtime
from newmota.berry import (
    BERRY_SOURCE,
    generate_berry,
    stage_berry_generated_headers,
    stage_berry_runtime,
)
from newmota.compdb import recorder_command, write_recorded_database
from newmota.crypto import BEARSSL_ROOT
from newmota.host import (
    SdkVersion,
    SdkVersionError,
    check_sdk_version,
    compile_status_compiler,
    copy_if_changed,
    copy_tree,
    cross_build_environment,
    display_path,
    ensure_sdk_source,
    find_ccache,
    host_key,
    install_archive_toolchain,
    load_sdk_version,
    run,
    run_build,
    sha256,
    stage_project_sources,
    tools_root,
    verbose_build_enabled,
    verify_toolchain,
    write_if_changed,
)
from newmota.i18n import generate_recovery_web_assets, generate_web_assets
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
from newmota.ota import chip_id
from newmota.package import LayoutFacts
from newmota.platforms.amebad.images import (
    FLASH_BASE,
    AmebadLayout,
    AmebadLayoutError,
    load_layout,
    package_amebad,
    resolve_layout,
)

TOOLCHAIN_RELEASE = (
    "https://github.com/Ameba-AIoT/ameba-toolchain/releases/download/V10.4.1-ameba-rtos-pro2"
)
TOOLCHAINS = {
    "darwin-arm64": (
        "asdk-10.4.1-darwin-newlib-build-3779-arm64.tar.bz2",
        "02879263cb92fe524dc501467b1c9b54c7b31dd491f5c04d4a2e76f4bf89964b",
        Path("darwin/newlib"),
        "asdk-10.4.1",
    ),
    "darwin-x64": (
        "asdk-10.4.1-darwin-newlib-build-3026-x86_64.tar.bz2",
        "8edeaa1da388973d2364c4f8e3b4bcef3406a6ac1274ea261631a44d2fcaf188",
        Path("darwin/newlib"),
        "asdk-6.5.0",
    ),
    "linux-x64": (
        "asdk-10.4.1-linux-newlib-build-3768-x86_64.tar.bz2",
        "4233325bb0b15b5e46c709bbb1faff0aa0dd282afdd51b202b1be98fdf7111c4",
        Path("linux/newlib"),
        "asdk-6.4.1",
    ),
}
PROGRAMMER_NAME = "newmota-amebad-python"
PROGRAMMER_SOURCE = Path(__file__).with_name("uploader.py")
OTA_CHECKSUM_SOURCE = Path("tools/newmota/ota_checksum.c")
OTA_CHECKSUM_BINARY = "newmota_ota_checksum"
COMPILER_GATE_SOURCE = Path(__file__).with_name("compiler_gate.c")
COMPILER_GATE_BINARY = "newmota_compiler_gate"


SOURCE_ALIASES = {
    "app.cpp": "newmota.cpp",
    "serial_console.cpp": "console.cpp",
    "crc32_portable.cpp": "crc32.cpp",
    "udp.cpp": "network_udp.cpp",
    "ping.cpp": "network_ping.cpp",
    "platform.cpp": "newmota_platform.cpp",
}
RELATIVE_SOURCE_ALIASES = {
    "src/platform/amebad/ble.cpp": "amebad_ble.cpp",
    "src/platform/amebad/filesystem.cpp": "amebad_filesystem.cpp",
    "src/platform/pwm.cpp": "newmota_pwm.cpp",
    "src/platform/amebad/pwm.cpp": "amebad_pwm.cpp",
}
# The firmware calls pre_example_entry and the cm_backtrace crash handlers from these sources.
VENDOR_EXAMPLE_SOURCES = ("example_entry.c", "cm_backtrace/example_cm_backtrace.c")


def _select_vendor_examples(makefile: str) -> str:
    """Drop the SDK example applications, keeping only sources the firmware links."""
    source = re.compile(r"^CSRC \+= *\$\(DIR\)/(\S+\.c)\n", re.MULTILINE)
    missing = set(VENDOR_EXAMPLE_SOURCES) - set(source.findall(makefile))
    if missing:
        raise BuildError(f"AmebaD example source selection changed: {sorted(missing)}")
    return source.sub(
        lambda match: match.group(0) if match.group(1) in VENDOR_EXAMPLE_SOURCES else "",
        makefile,
    )


def _align_image2_sections(script: str) -> str:
    """Declare BSS alignment and pad trace data without changing image contents."""
    # RAM data ends at a 16-byte image boundary, while SDK crypto BSS requires 32.
    # Declare the input-derived BSS alignment before its start symbol, keeping
    # the linker's existing placement and the startup zeroing range unchanged.
    bss = "\t.ram_image2.bss :\n"
    if script.count(bss) != 1:
        raise BuildError("AmebaD image2 BSS linker section changed")
    script = script.replace(bss, "\t.ram_image2.bss ALIGN(ALIGNOF(.ram_image2.bss)) :\n")

    # Pad the trace section so the orphan .rel.dyn starts without moving.
    anchor = "\t\t__btrace_end__ = .;\n"
    if script.count(anchor) != 1:
        raise BuildError("AmebaD Bluetooth trace linker section changed")
    return script.replace(anchor, anchor + "\t\t. = ALIGN(4);\n")


def _flashloader(sdk: Path) -> Path:
    return sdk / "tools" / "AmebaD" / "Image_Tool" / "imgtool_flashloader_amebad.bin"


def _load_amebad_layout(context: BuildContext) -> AmebadLayout:
    if context.environment.partition is None:
        raise BuildError("AmebaD requires a resolved partition layout")
    try:
        return load_layout(
            context.environment.partition,
            context.environment.flash_kb * 1024,
        )
    except AmebadLayoutError as error:
        raise BuildError(str(error)) from error


def _prepare_bootcfg(context: BuildContext, layout: AmebadLayout) -> Path:
    """Create a writable SDK config overlay with Newmota OTA addresses."""

    source = context.sdk.source / "component" / "soc" / "realtek" / "amebad" / "fwlib" / "usrcfg"
    destination = context.work_dir / "generated" / "amebad-usrcfg"
    if not source.is_dir():
        raise BuildError(f"AmebaD SDK user config directory is missing: {source}")
    copy_tree(source, destination)

    bootcfg = destination / "rtl8721d_bootcfg.c"
    try:
        content = bootcfg.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise BuildError(f"cannot read generated AmebaD boot config: {bootcfg}") from error
    marker = "u32 OTA_Region[2]"
    start = content.find(marker)
    if start < 0:
        raise BuildError(f"AmebaD boot config has no {marker} declaration")
    opening = content.find("{", start)
    closing = content.find("};", opening)
    if opening < 0 or closing < 0:
        raise BuildError("AmebaD boot config OTA_Region declaration is malformed")
    body = content[opening:closing]
    addresses = [
        f"0x{FLASH_BASE + layout.ota1.offset:08X}",
        f"0x{FLASH_BASE + layout.ota2.offset:08X}",
    ]
    import re

    values = list(re.finditer(r"0x[0-9A-Fa-f]+", body))
    if len(values) < 2:
        raise BuildError("AmebaD boot config OTA_Region has fewer than two addresses")
    rewritten = body
    for match, address in reversed(list(zip(values[:2], addresses))):
        rewritten = rewritten[: match.start()] + address + rewritten[match.end() :]
    content = content[:opening] + rewritten + content[closing:]
    write_if_changed(bootcfg, content, encoding="utf-8")
    return destination


def _programmer_issue(sdk: SdkVersion) -> str | None:
    if sdk.programmer != PROGRAMMER_NAME or sdk.programmer_status == "unselected":
        return f"AmebaD programmer is not selected: {PROGRAMMER_NAME}"
    if sdk.programmer_sha256 != sha256(PROGRAMMER_SOURCE):
        return f"AmebaD programmer hash does not match {PROGRAMMER_SOURCE.relative_to(ROOT)}"
    loader = _flashloader(sdk.source)
    if not loader.is_file():
        return f"missing AmebaD flashloader {loader}"
    return None


def _toolchain() -> tuple[Path, Path, str]:
    host = host_key()
    if host not in TOOLCHAINS:
        raise SetupUnavailable(f"AmebaD has no toolchain package for {host}")
    archive_name, _, compiler_subdir, expected_version = TOOLCHAINS[host]
    destination = tools_root() / "amebad" / archive_name.removesuffix(".tar.bz2")
    return destination, compiler_subdir, expected_version


def _ota_checksum_source(sdk: SdkVersion) -> Path:
    return sdk.source / OTA_CHECKSUM_SOURCE


def _ota_checksum_tool(sdk: SdkVersion, host: str) -> Path:
    source = _ota_checksum_source(sdk)
    source_hash = sha256(source)
    return tools_root() / "amebad" / "host-tools" / host / source_hash / OTA_CHECKSUM_BINARY


def _compile_ota_checksum_tool(sdk: SdkVersion, host: str) -> Path:
    source = _ota_checksum_source(sdk)
    if not source.is_file():
        raise SetupUnavailable(f"missing AmebaD host checksum source {source}")

    return _compile_host_tool(source, _ota_checksum_tool(sdk, host))


def _compile_host_tool(source: Path, destination: Path) -> Path:
    if destination.is_file() and os.access(destination, os.X_OK):
        return destination

    compiler = None
    for candidate in ("cc", "clang", "gcc"):
        compiler = shutil.which(candidate)
        if compiler is not None:
            break
    if compiler is None:
        raise SetupUnavailable(
            "AmebaD builds need a host C compiler (cc, clang, or gcc) for their host tools"
        )

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=".host-tool-build-", dir=destination.parent
    ) as temporary:
        temporary_binary = Path(temporary) / destination.name
        run(
            [
                compiler,
                "-std=c99",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(source),
                "-o",
                str(temporary_binary),
            ],
            environment=cross_build_environment(),
        )
        os.chmod(temporary_binary, 0o755)
        os.replace(temporary_binary, destination)
    return destination


def _compiler_gate() -> Path:
    """Return the launch gate for Realtek's gcc driver; see compiler_gate.c for the reason."""

    destination = (
        tools_root()
        / "amebad"
        / "host-tools"
        / host_key()
        / sha256(COMPILER_GATE_SOURCE)
        / COMPILER_GATE_BINARY
    )
    try:
        return _compile_host_tool(COMPILER_GATE_SOURCE, destination)
    except SetupUnavailable as error:
        raise BuildError(str(error)) from error


def _stage_ota_checksum_tool(project: Path, sdk: SdkVersion) -> Path:
    source = _ota_checksum_tool(sdk, host_key())
    if not source.is_file():
        raise BuildError("AmebaD host checksum tool is missing; rerun make setup PLATFORM=amebad")

    for core in ("project_lp", "project_hp"):
        utility = project / "GCC-RELEASE" / core / "asdk" / "gnu_utility"
        if not utility.is_dir():
            raise BuildError(f"AmebaD SDK utility directory is missing: {utility}")
        copy_if_changed(source, utility / OTA_CHECKSUM_BINARY)
    return source


def setup_issue(_chips: list[str]) -> str | None:
    try:
        sdk = check_sdk_version("amebad")
        programmer_issue = _programmer_issue(sdk)
        if programmer_issue is not None:
            return programmer_issue
        destination, compiler_subdir, _ = _toolchain()
        if not destination.is_dir():
            return f"missing AmebaD toolchain {destination}"
        verify_toolchain(destination / compiler_subdir, "arm-none-eabi-")
        checksum_source = _ota_checksum_source(sdk)
        if not checksum_source.is_file():
            return f"missing AmebaD host checksum source {checksum_source}"
        checksum_tool = _ota_checksum_tool(sdk, host_key())
        if not checksum_tool.is_file() or not os.access(checksum_tool, os.X_OK):
            return f"missing executable AmebaD host checksum tool {checksum_tool}"
    except (OSError, SdkVersionError, SetupUnavailable, subprocess.CalledProcessError) as error:
        return str(error)
    return None


def setup(_chips: list[str], dry_run: bool) -> None:
    version = load_sdk_version("amebad")
    sdk = version.source
    host = host_key()
    if host not in TOOLCHAINS:
        raise SetupUnavailable(f"AmebaD has no toolchain package for {host}")
    archive_name, sha256, compiler_subdir, _ = TOOLCHAINS[host]
    url = f"{TOOLCHAIN_RELEASE}/{archive_name}"
    destination = tools_root() / "amebad" / archive_name.removesuffix(".tar.bz2")
    print(f"AmebaD SDK source: {sdk}")
    print(f"AmebaD SDK version: {version.version} ({version.revision[:12]})")
    print(f"AmebaD programmer: {version.programmer} {version.programmer_version}")
    print(f"AmebaD tools:      {destination}")
    ensure_sdk_source("amebad", dry_run)
    if not dry_run:
        checked = check_sdk_version("amebad")
        programmer_issue = _programmer_issue(checked)
        if programmer_issue is not None:
            raise SetupUnavailable(programmer_issue)
        checksum_tool = _compile_ota_checksum_tool(checked, host)
        print(f"AmebaD checksum:  {checksum_tool}")
    compiler = install_archive_toolchain(
        url,
        sha256,
        destination,
        "arm-none-eabi-",
        dry_run,
        compiler_subdir,
    )
    print(f"AmebaD compiler:   {compiler}")


def _apply_config(project: Path, context: BuildContext) -> None:
    release = project / "GCC-RELEASE"
    for core in ("project_lp", "project_hp"):
        config_path = release / core / "asdk" / "config" / ".config"
        if not config_path.is_file():
            raise BuildError(f"AmebaD vendor config is missing: {config_path}")
        baseline = project / f".newmota-{core}.config"
        if not baseline.is_file():
            copy_if_changed(config_path, baseline)
        content = baseline.read_text(encoding="utf-8").rstrip()
        if context.native_config.assignments:
            content += "\n\n# Newmota resolved native settings\n"
            content += "\n".join(
                f"{key}={value}" for key, value in context.native_config.assignments
            )
            content += "\n"
        else:
            content += "\n"
        final_config = context.native_config.defaults.parent / f"amebad-{core}.config"
        write_if_changed(final_config, content, encoding="utf-8")
        write_if_changed(config_path, content, encoding="utf-8")
        _apply_sdk_log_header(
            project / "inc" / f"inc_{core.removeprefix('project_')}" / "platform_autoconf.h",
            context.native_config.values.get("CONFIG_DEBUG_LOG"),
            ble_enabled=context.features.enabled("USE_BLE"),
        )


def _apply_sdk_log_header(header: Path, setting: str | None, *, ble_enabled: bool = False) -> None:
    """Apply AmebaD's resolved debug-log switch to the compiler header.

    The vendor build reads ``config/.config`` for Make variables but only
    regenerates ``platform_autoconf.h`` from an interactive config command.
    Newmota owns the generated project overlay, so update that header directly
    and keep the checked-in SDK untouched.
    """

    if setting not in {"y", "n"}:
        raise BuildError(f"AmebaD CONFIG_DEBUG_LOG is invalid: {setting!r}")
    try:
        content = header.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise BuildError(f"cannot read AmebaD platform config header: {header}") from error

    marker = "/* Newmota resolved SDK log policy. */"
    content = re.sub(
        rf"\n{re.escape(marker)}\n#undef CONFIG_DEBUG_LOG\n#define CONFIG_DEBUG_LOG [01]\n",
        "\n",
        content,
    )
    ble_marker = "/* Newmota optional native BLE owner. */"
    content = re.sub(
        rf"\n{re.escape(ble_marker)}\n#undef CONFIG_BT\n#undef CONFIG_BT_EN\n"
        r"#define CONFIG_BT [01]\n#define CONFIG_BT_EN [01]\n",
        "\n",
        content,
    )
    guard_end = content.rfind("\n#endif")
    if guard_end < 0:
        raise BuildError(f"AmebaD platform config header has no include guard: {header}")
    override = f"\n\n{marker}\n#undef CONFIG_DEBUG_LOG\n"
    if setting == "y":
        override += "#define CONFIG_DEBUG_LOG 1\n"
    if ble_enabled:
        override += (
            f"\n{ble_marker}\n#undef CONFIG_BT\n#undef CONFIG_BT_EN\n"
            "#define CONFIG_BT 1\n#define CONFIG_BT_EN 1\n"
        )
    write_if_changed(
        header,
        content[:guard_end].rstrip() + override + content[guard_end:],
        encoding="utf-8",
    )


def _enable_stack_high_water_api(header: Path) -> None:
    """Enable FreeRTOS's existing stack watermark API in the generated project."""

    try:
        content = header.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise BuildError(f"cannot read AmebaD FreeRTOS config: {header}") from error

    pattern = re.compile(
        r"(?m)^(?P<prefix>[ \t]*#define[ \t]+INCLUDE_uxTaskGetStackHighWaterMark[ \t]+)"
        r"(?P<value>[01])(?P<suffix>[ \t]*(?:/\*.*\*/)?[ \t]*)$"
    )
    matches = list(pattern.finditer(content))
    if len(matches) != 1:
        raise BuildError(
            "AmebaD FreeRTOS config must define INCLUDE_uxTaskGetStackHighWaterMark once"
        )
    if matches[0].group("value") == "0":
        content = pattern.sub(r"\g<prefix>1\g<suffix>", content, count=1)
        write_if_changed(header, content, encoding="utf-8")


def _configure_web_tls_certificate_writers(project: Path, enabled: bool) -> None:
    """Configure AmebaD's Mbed TLS 2.4 writer sources and invalidate stale objects."""
    makefile = (
        project
        / "GCC-RELEASE"
        / "project_hp"
        / "asdk"
        / "make"
        / "mbedtls"
        / "mbedtls-2.4.0"
        / "Makefile"
    )
    try:
        content = makefile.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise BuildError(f"cannot read AmebaD Mbed TLS library makefile: {makefile}") from error

    marker = "# Newmota HTTPS certificate writer modules."
    writer_source = (
        marker
        + "\nCSRC += $(DIR)/library/pkwrite.c\n"
        + "MODULE_IFLAGS += -DMBEDTLS_ASN1_WRITE_C -DMBEDTLS_ECDSA_C \\\n"
        + "                -DMBEDTLS_ECDH_C -DMBEDTLS_ECP_C -DMBEDTLS_PEM_WRITE_C \\\n"
        + "                -DMBEDTLS_PK_WRITE_C -DMBEDTLS_X509_CREATE_C \\\n"
        + "                -DMBEDTLS_X509_CRT_WRITE_C\n"
    )
    original_content = content
    had_writer_source = marker in content
    if had_writer_source and "\n" + writer_source not in content:
        raise BuildError(
            "AmebaD Mbed TLS certificate writer source block is malformed; run make clean"
        )
    if enabled and not had_writer_source:
        anchor = "       $(DIR)/mbedtls_rom_test.c\n"
        if content.count(anchor) != 1:
            raise BuildError(f"AmebaD Mbed TLS source list has no unique {anchor.strip()} entry")
        content = content.replace(anchor, anchor + "\n" + writer_source)
    elif not enabled and had_writer_source:
        content = content.replace("\n" + writer_source, "\n")
    write_if_changed(makefile, content, encoding="utf-8")

    state = "enabled\n" if enabled else "disabled\n"
    state_file = project / ".newmota-mbedtls-web-https"
    try:
        previous_state = state_file.read_text(encoding="utf-8")
    except FileNotFoundError:
        previous_state = None
    except (OSError, UnicodeError) as error:
        raise BuildError(f"cannot read AmebaD Mbed TLS feature state: {state_file}") from error

    if previous_state != state or had_writer_source != enabled or content != original_content:
        for core in ("lp", "hp"):
            asdk = project / "GCC-RELEASE" / f"project_{core}" / "asdk"
            object_directory = asdk / "make" / "mbedtls" / "mbedtls-2.4.0"
            ram_directory = asdk / "build" / "ram"
            for object_file in object_directory.glob("*.o"):
                (ram_directory / object_file.name).unlink(missing_ok=True)
                object_file.unlink(missing_ok=True)
        write_if_changed(state_file, state, encoding="utf-8")


def _prepare_project(
    context: BuildContext, sdk: SdkVersion, sdk_board: str
) -> tuple[Path, dict[Path, Path]]:
    overlay = context.work_dir / "overlay"
    project_source = sdk.source / "project" / sdk_board
    project = overlay / "project" / project_source.name
    revision_file = project / ".newmota-sdk-revision"
    if project.is_dir() and (
        not revision_file.is_file() or revision_file.read_text().strip() != sdk.revision
    ):
        print("Refreshing generated AmebaD project for the pinned SDK revision.")
        shutil.rmtree(project)
    if not project.is_dir():
        overlay.mkdir(parents=True, exist_ok=True)
        for name in ("component", "tools"):
            if not (overlay / name).is_symlink():
                os.symlink(sdk.source / name, overlay / name, target_is_directory=True)
        shutil.copytree(project_source, project)
        write_if_changed(revision_file, sdk.revision + "\n")
    # Reconstruct generated makefiles from SDK inputs when toggling the feature.
    # The native FTL addresses overlap Newmota's OTA layout; BLE uses RAM hooks.
    make_root = Path("GCC-RELEASE/project_hp/asdk/make")
    filesystem_make = make_root / "file_system/Makefile"
    content = (project_source / filesystem_make).read_text(encoding="utf-8")
    if context.features.enabled("USE_BLE"):
        content = content.replace("\t$(MAKE) -C ftl all", "\t# Newmota BLE uses RAM key storage.")
    if context.features.enabled("USE_SDCARD"):
        anchor = "\t$(MAKE) -C fatfs all\n"
        if content.count(anchor) != 1:
            raise BuildError("AmebaD FatFs source selection changed; review SD card ownership")
        # The SDK's FatFs R0.10c and its disk glue collide with Newmota's vendored FatFs.
        content = content.replace(anchor, "\t# Newmota vendors FatFs for USE_SDCARD.\n")
    write_if_changed(project / filesystem_make, content)
    at_make = make_root / "api/at_cmd/Makefile"
    content = (project_source / at_make).read_text(encoding="utf-8")
    if context.features.enabled("USE_BLE"):
        anchor = "CSRC += $(DIR)/atcmd_bt.c\n"
        if content.count(anchor) != 1:
            raise BuildError("AmebaD Bluetooth AT source selection changed; review BLE ownership")
        content = content.replace(
            anchor, "# Newmota owns BLE; omit the SDK Bluetooth AT commands.\n"
        )
    write_if_changed(project / at_make, content)
    bsp_make = make_root / "rtl_bluetooth/amebad_bsp/Makefile"
    content = (project_source / bsp_make).read_text(encoding="utf-8")
    if context.features.enabled("USE_BLE"):
        for source in (
            "sdk/board/common/src/trace_task.c",
            "sdk/board/amebad/src/trace_uart.c",
            "sdk/board/common/src/bt_uart_bridge.c",
        ):
            anchor = f"\t$(DIR)/{source} \\\n"
            if content.count(anchor) != 1:
                raise BuildError(f"AmebaD BLE BSP source selection changed: {source}")
            content = content.replace(anchor, "")
        anchor = (
            "\t$(DIR)/sdk/board/amebad/src/hci/bt_mp_patch.c \\\n\t$(DIR)/sdk/bt_example_entry.c"
        )
        if content.count(anchor) != 1:
            raise BuildError("AmebaD BLE example-entry source selection changed")
        content = content.replace(anchor, "\t$(DIR)/sdk/board/amebad/src/hci/bt_mp_patch.c")
        hci_source = "sdk/board/amebad/src/hci/hci_board.c"
        anchor = f"$(DIR)/{hci_source}"
        if content.count(anchor) != 1:
            raise BuildError("AmebaD BLE BSP controller source changed; review its flash ownership")
        content = content.replace(anchor, "$(SRCDIR)/ble_vendor/hci_board.c")
        bluetooth = sdk.source / "component/common/bluetooth/realtek"
        hci_content = (bluetooth / hci_source).read_text(encoding="utf-8")
        function = re.compile(r"uint8_t \*hci_find_patch_address\(void\)\n\{.*?\n\}", re.DOTALL)
        if len(function.findall(hci_content)) != 1:
            raise BuildError(
                "AmebaD BLE patch selector changed; refusing vendor flash-patch offsets"
            )
        hci_content = function.sub(
            "uint8_t *hci_find_patch_address(void)\n{\n"
            "    /* Newmota owns the flash layout; use the pinned compiled controller patch. */\n"
            "    return (uint8_t *)rltk_bt_get_patch_code();\n}",
            hci_content,
        )
        write_if_changed(project / "src/src_hp/ble_vendor/hci_board.c", hci_content)
        hci_source = "sdk/board/amebad/src/hci/hci_uart.c"
        anchor = f"$(DIR)/{hci_source}"
        if content.count(anchor) != 1:
            raise BuildError("AmebaD BLE BSP UART source changed; review its bridge routing")
        content = content.replace(anchor, "$(SRCDIR)/ble_vendor/hci_uart.c")
        hci_content = (bluetooth / hci_source).read_text(encoding="utf-8")
        bridge = (
            "        if(hci_adapter->hci_uart_bridge_flag == true)\n"
            "        {\n"
            "            hci_uart_rx_bridge(ch);\n"
            "            return;\n"
            "        }"
        )
        if hci_content.count(bridge) != 1:
            raise BuildError("AmebaD BLE UART bridge branch changed; refusing external HCI routing")
        hci_content = hci_content.replace(
            bridge, "        /* Newmota keeps HCI bytes on the internal controller transport. */"
        )
        for function_name in (
            "uint8_t hci_rx_empty",
            "uint16_t hci_rx_data_len",
            "uint16_t hci_rx_space_len",
        ):
            anchor = f"{function_name}()\n"
            if hci_content.count(anchor) != 1:
                raise BuildError(f"AmebaD BLE UART prototype changed: {function_name}")
            hci_content = hci_content.replace(anchor, f"{function_name}(void)\n")
        write_if_changed(project / "src/src_hp/ble_vendor/hci_uart.c", hci_content)
        # Keep the SDK's diagnostics for every other source in this directory.
        anchor = "OBJS = $(CSRC:.c=.o)\n"
        if content.count(anchor) != 1:
            raise BuildError("AmebaD BLE BSP object list changed; review its warning overrides")
        content = content.replace(
            anchor,
            anchor
            + "$(DIR)/sdk/board/amebad/src/rtk_coex.o: GLOBAL_CFLAGS += -Wno-unused-function\n"
            + "$(DIR)/sdk/board/common/os/freertos/osif_freertos.o: "
            + "GLOBAL_CFLAGS += -Wno-unused-parameter\n",
        )
    write_if_changed(project / bsp_make, content)
    examples_make = make_root / "utilities_example/Makefile"
    write_if_changed(
        project / examples_make,
        _select_vendor_examples((project_source / examples_make).read_text(encoding="utf-8")),
    )
    image2_script = Path("GCC-RELEASE/project_hp/asdk/ld/rlx8721d_img2_is.ld")
    write_if_changed(
        project / image2_script,
        _align_image2_sections((project_source / image2_script).read_text(encoding="utf-8")),
    )
    _configure_web_tls_certificate_writers(project, context.features.enabled("USE_WEB_HTTPS"))
    _enable_stack_high_water_api(project / "inc" / "inc_hp" / "FreeRTOSConfig.h")
    _stage_ota_checksum_tool(project, sdk)
    for core in ("lp", "hp"):
        main_header = project / "inc" / f"inc_{core}" / "main.h"
        if not main_header.is_file():
            raise BuildError(f"AmebaD vendor main header is missing: {main_header}")
        main_header_text = main_header.read_text(encoding="utf-8")
        main_header_text = _disable_vendor_fast_reconnect(main_header_text)
        write_if_changed(main_header, main_header_text, encoding="utf-8")
        platform_header = project / "inc" / f"inc_{core}" / "platform_opts.h"
        if not platform_header.is_file():
            raise BuildError(f"AmebaD vendor platform header is missing: {platform_header}")
        platform_header_text = platform_header.read_text(encoding="utf-8")
        platform_header_text = _disable_vendor_fast_reconnect_platform_options(platform_header_text)
        write_if_changed(platform_header, platform_header_text, encoding="utf-8")
    source = project / "src" / "src_hp"
    include = source / "include"
    copy_tree(ROOT / "src" / "app" / "include", include)
    copy_tree(ROOT / "src" / "platform" / "include", include)
    image_header = ROOT / "src" / "platform" / "amebad" / "image_size.h"
    copy_if_changed(image_header, include / image_header.name)
    if context.features.enabled("USE_BLE"):
        bond_header = ROOT / "src/platform/amebad/ble_bond.h"
        copy_if_changed(bond_header, include / bond_header.name)
    sources = stage_project_sources(
        context,
        source,
        include,
        aliases=SOURCE_ALIASES,
        relative_aliases=RELATIVE_SOURCE_ALIASES,
    )
    if any(
        source.endswith("platform/amebad/recovery_update_writer.cpp")
        for source in context.features.sources
    ):
        writer_header = ROOT / "src" / "platform" / "amebad" / "recovery_update_writer.h"
        copy_if_changed(writer_header, include / writer_header.name)
    if context.features.enabled("USE_SDCARD"):
        for header in ("ff.h", "ffconf.h", "diskio.h"):
            # The SDK ships older FatFs headers; keep Newmota's beside its sources.
            origin = ROOT / "src" / "third_party" / "fatfs" / header
            copy_if_changed(origin, source / header)
            copy_if_changed(origin, include / header)
    for header in ("lfs.h", "lfs_util.h"):
        origin = ROOT / "src" / "third_party" / "littlefs" / header
        # The SDK also ships an older LittleFS under an earlier include path.
        # Keep the selected headers beside lfs.c so quoted includes resolve to
        # the Newmota copy before the vendor search paths are considered.
        copy_if_changed(origin, source / header)
        copy_if_changed(origin, include / header)
    if context.features.enabled("USE_CMSIS_DSP"):
        config_header = ROOT / "src/platform/amebad/arm_dsp_config.h"
        copy_if_changed(config_header, include / config_header.name)
    makefile = ROOT / "src/platform/amebad/native/Makefile"
    copy_if_changed(makefile, source / "Makefile")
    sources.update(stage_berry_runtime(context, source, include))
    sources.update(stage_lvgl_runtime(context, source))
    sources.update(stage_audio_runtime(context, source))
    # Derive the vendor make inputs from the very same staged selection.
    make_sources = [
        "# Generated from resolved requirements; do not edit.",
        (
            "MODULE_IFLAGS += "
            + " ".join(f"-I$(DIR)/lvgl/{directory}" for directory in LVGL_INCLUDE_DIRS)
            if context.features.enabled("USE_LVGL")
            else ""
        ),
        "GLOBAL_CFLAGS += -DBR_LOMUL=1" if context.features.enabled("USE_CRYPTO") else "",
        "CPPFLAGS += -DBR_LOMUL=1" if context.features.enabled("USE_CRYPTO") else "",
        # The SDK's cmsis folder holds older headers of the same names; -iquote wins over its -I.
        (
            f"MODULE_IFLAGS += -DARM_DSP_CUSTOM_CONFIG"
            f" -iquote{ROOT}/src/third_party/cmsis_dsp/Include"
            f" -iquote{ROOT}/src/third_party/cmsis_dsp/PrivateInclude"
            if context.features.enabled("USE_CMSIS_DSP")
            else ""
        ),
        "CSRC = $(DIR)/main.c "
        + " ".join(
            "$(DIR)/" + str(path.relative_to(source))
            for path in sources.values()
            if path.suffix == ".c"
        ),
        "CPPSRC = "
        + " ".join(
            "$(DIR)/" + str(path.relative_to(source))
            for path in sources.values()
            if path.suffix == ".cpp"
        ),
        "",
        # Imported BearSSL and Berry intentionally test undefined feature, target, and
        # compiler macros. Keep the SDK's -Wundef diagnostics for Newmota and all other sources.
        *(
            f"{path.stem}.o: GLOBAL_CFLAGS += -Wno-undef"
            for original, path in sources.items()
            if path.suffix == ".c"
            and (
                original.is_relative_to(BEARSSL_ROOT / "src")
                or original.is_relative_to(BERRY_SOURCE)
            )
        ),
        "",
    ]
    make_sources.extend(ameba_codec_settings(sources, source))
    if context.features.enabled("USE_BLE"):
        make_sources.append("MODULE_IFLAGS += -I$(DIR)/lvgl/berry_mapping/src")
        bluetooth = "$(BASEDIR)/component/common/bluetooth/realtek/sdk"
        directories = (
            "inc",
            "inc/app",
            "inc/bluetooth/gap",
            "inc/bluetooth/gap/gap_lib",
            "inc/bluetooth/profile",
            "inc/os",
            "inc/stack",
            "board/amebad/lib",
            "board/amebad/src",
            "board/common/inc",
        )
        make_sources.append(
            "MODULE_IFLAGS += "
            + " ".join(f"-I{bluetooth}/{directory}" for directory in directories)
        )
    write_if_changed(source / "newmota_sources.mk", "\n".join(make_sources))
    main = source / "main.c"
    main_text = (project_source / "src" / "src_hp" / "main.c").read_text(encoding="utf-8")
    main_text = _claim_loguart_from_vendor_shell(main_text)
    if context.features.enabled("USE_BLE"):
        main_text = main_text.replace(
            "\tapp_ftl_init();", "\t/* Newmota BLE owns RAM key storage. */"
        )
    if "extern void newmota_entry(void);" not in main_text:
        main_text = main_text.replace(
            '#include "main.h"', '#include "main.h"\n\nextern void newmota_entry(void);'
        )
    main_text = main_text.replace("\texample_entry();", "\tnewmota_entry();")
    write_if_changed(main, main_text, encoding="utf-8")
    return project, sources


def _disable_vendor_fast_reconnect(main_text: str) -> str:
    """Keep the SDK reconnect example from writing into the XIP image."""
    marker = (
        "/* Newmota owns Wi-Fi credentials in LittleFS; vendor fast reconnect is "
        "disabled unconditionally. */"
    )
    if marker in main_text:
        return main_text

    vendor_override = (
        "#undef CONFIG_EXAMPLE_WLAN_FAST_CONNECT\n#define CONFIG_EXAMPLE_WLAN_FAST_CONNECT\t1"
    )
    if vendor_override in main_text:
        main_text = main_text.replace(
            vendor_override,
            "#undef CONFIG_EXAMPLE_WLAN_FAST_CONNECT\n#define CONFIG_EXAMPLE_WLAN_FAST_CONNECT\t0",
            1,
        )
    elif "#define CONFIG_EXAMPLE_WLAN_FAST_CONNECT\t0" not in main_text:
        raise BuildError(
            "AmebaD SDK main.h fast-reconnect override changed; refusing to leave "
            "the vendor flash profile writer active"
        )

    newmota_override = (
        f"\n\n{marker}\n"
        "#undef CONFIG_EXAMPLE_WLAN_FAST_CONNECT\n"
        "#define CONFIG_EXAMPLE_WLAN_FAST_CONNECT\t0\n"
    )
    guard_end = main_text.rfind("\n#endif")
    if guard_end < 0:
        raise BuildError("AmebaD SDK main.h has no include guard terminator")
    return main_text[:guard_end] + newmota_override + main_text[guard_end:]


def _disable_vendor_fast_reconnect_platform_options(platform_text: str) -> str:
    """Keep SDK fast-DHCP support from retaining the reconnect flash dependency."""
    marker = (
        "/* Newmota owns Wi-Fi credentials in LittleFS; vendor fast-DHCP persistence "
        "is disabled unconditionally. */"
    )
    if marker in platform_text:
        return platform_text

    guard_end = platform_text.rfind("\n#endif")
    if guard_end < 0:
        raise BuildError("AmebaD SDK platform_opts.h has no include guard terminator")

    newmota_override = (
        f"\n\n{marker}\n"
        "#undef CONFIG_EXAMPLE_WLAN_FAST_CONNECT\n"
        "#define CONFIG_EXAMPLE_WLAN_FAST_CONNECT\t0\n"
        "#undef CONFIG_FAST_DHCP\n"
        "#define CONFIG_FAST_DHCP\t0\n"
    )
    return platform_text[:guard_end] + newmota_override + platform_text[guard_end:]


def _claim_loguart_from_vendor_shell(main_text: str) -> str:
    """Remove the SDK shell's LOGUART consumer from the generated HP startup."""
    marker = "\t/* Newmota owns LOGUART input; the SDK shell is disabled. */\n"
    if marker in main_text:
        return main_text

    vendor_console_block = (
        "\t/* init console */\n"
        "\tshell_recv_all_data_onetime = 1;\n"
        "\tshell_init_rom(0, 0);\t\n"
        "\tshell_init_ram();\n"
        "\tipc_table_init();\n"
        "\n"
        "\t/* Register Log Uart Callback function */\n"
        "\tInterruptRegister((IRQ_FUN) shell_uart_irq_rom, UART_LOG_IRQ, (u32)NULL, 5);\n"
        "\tInterruptEn(UART_LOG_IRQ,5);\n"
    )
    if vendor_console_block not in main_text:
        raise BuildError(
            "AmebaD SDK main.c console startup block changed; refusing to leave "
            "the vendor LOGUART shell active"
        )

    replacement = marker + "\tipc_table_init();\n\n"
    return main_text.replace(vendor_console_block, replacement, 1)


def _link_toolchain(project: Path, toolchain: Path, os_dir: str, expected_version: str) -> None:
    release = project / "GCC-RELEASE"
    for core in ("project_lp", "project_hp"):
        destination = release / core / "toolchain" / os_dir / expected_version
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.is_symlink():
            if destination.resolve() == toolchain.resolve():
                continue
            destination.unlink()
        elif destination.exists():
            raise BuildError(f"unexpected AmebaD toolchain path exists: {destination}")
        os.symlink(toolchain, destination, target_is_directory=True)


class AmebadAdapter(PlatformAdapter):
    native_config = NativeConfigSpec("amebad", "defconfig")

    @classmethod
    def resolve_layout(
        cls, environment: Environment, sdk: SdkVersion, work_dir: Path
    ) -> LayoutFacts | None:
        """Resolve the vendor layout into shared OTA compatibility facts."""

        try:
            return resolve_layout(environment, sdk, work_dir)
        except AmebadLayoutError as error:
            raise BuildError(str(error)) from error

    @classmethod
    def sdk_log_settings(cls, level: str) -> Mapping[str, str]:
        """Map AmebaD's coarse debug-log switch to the global threshold."""

        return {"CONFIG_DEBUG_LOG": "n" if level in {"none", "error", "warn"} else "y"}

    def __init__(self, context: BuildContext) -> None:
        super().__init__(context)
        self.project = context.work_dir / "overlay" / "project"
        self.release = self.project / "GCC-RELEASE"
        self.command: list[str] = []
        self.environment: dict[str, str] = {}
        self.compiler = ""
        self.source_map: dict[Path, Path] = {}
        self.layout: AmebadLayout | None = None
        self.bootcfg_directory: Path | None = None

    def prepare(self) -> None:
        sdk_board = self.context.environment.sdk_board
        if sdk_board is None:
            raise BuildError(f"board '{self.context.environment.board}' has no AmebaD sdk_board")
        self.layout = _load_amebad_layout(self.context)
        self.bootcfg_directory = _prepare_bootcfg(self.context, self.layout)
        self.project, staged_files = _prepare_project(self.context, self.context.sdk, sdk_board)
        self.source_map = {
            destination: source
            for source, destination in staged_files.items()
            if source.is_relative_to(ROOT / "src") and source.suffix == ".cpp"
        }
        self.release = self.project / "GCC-RELEASE"
        if self.context.native_config.changed:
            print("Discarding stale AmebaD objects after native config change.")
            for core in ("project_lp", "project_hp"):
                shutil.rmtree(self.release / core / "asdk" / "build", ignore_errors=True)
                shutil.rmtree(self.release / core / "asdk" / "image", ignore_errors=True)

    def configure(self) -> None:
        if self.layout is None or self.bootcfg_directory is None:
            raise BuildError("AmebaD configure called before prepare")
        _apply_config(self.project, self.context)
        toolchain, compiler_subdir, expected_version = _toolchain()
        _link_toolchain(self.project, toolchain, compiler_subdir.parts[0], expected_version)
        item = self.context.environment
        if item.partition is None:
            raise BuildError("AmebaD OTA writer requires a resolved partition source")
        layout_facts = self.context.layout
        if layout_facts is None:
            raise BuildError("AmebaD OTA writer requires resolved layout facts")
        layout_digest = layout_facts.identity
        definitions = " ".join(
            [
                f'-DNEWMOTA_TARGET_NAME=\\"{item.name}\\"',
                f'-DNEWMOTA_CHIP_NAME=\\"{item.chip}\\"',
                f'-DNEWMOTA_SDK_VERSION=\\"{self.context.sdk.version}\\"',
                f'-DNEWMOTA_FRAMEWORK_VERSION=\\"{self.context.version}\\"',
                f'-DNEWMOTA_BUILD_DATE=\\"{self.context.build_date}\\"',
                f'-DNEWMOTA_BUILD_TIME=\\"{self.context.build_time}\\"',
                "-DNEWMOTA_FILESYSTEM_ENABLED=1",
                f"-DNEWMOTA_FILESYSTEM_OFFSET={self.layout.filesystem.offset:#x}",
                f"-DNEWMOTA_FILESYSTEM_SIZE={self.layout.filesystem.size:#x}",
                f"-DNEWMOTA_FILESYSTEM_BLOCK_SIZE={self.layout.filesystem_block_size:#x}",
                f"-DNEWMOTA_OTA1_OFFSET={self.layout.ota1.offset:#x}",
                f"-DNEWMOTA_OTA1_SIZE={self.layout.ota1.size:#x}",
                f"-DNEWMOTA_OTA2_OFFSET={self.layout.ota2.offset:#x}",
                f"-DNEWMOTA_OTA2_SIZE={self.layout.ota2.size:#x}",
                f"-DNEWMOTA_UPDATE_CHIP_ID={chip_id(item.chip):#x}",
                f"-DNEWMOTA_UPDATE_FLASH_BYTES={self.layout.flash_bytes:#x}",
                f'-DNEWMOTA_UPDATE_LAYOUT_SHA256=\\"{layout_digest}\\"',
                "-DLFS_MULTIVERSION",
                "-DNEWMOTA_IMAGE_APPLICATION=1"
                if self.context.environment.role == "application"
                else "-DNEWMOTA_IMAGE_SAFEBOOT=1",
            ]
        )
        if self.context.features.enabled("USE_MQTT"):
            definitions += " -DMQTT_DO_NOT_USE_CUSTOM_CONFIG=1"
        if self.context.features.enabled("USE_BLE"):
            definitions += f' -DNEWMOTA_BLE_SDK_REVISION=\\"{self.context.sdk.revision}\\"'
        if self.context.features.enabled("USE_WEB_HTTPS"):
            definitions += (
                " -DMBEDTLS_ASN1_WRITE_C"
                " -DMBEDTLS_ECDSA_C"
                " -DMBEDTLS_ECDH_C"
                " -DMBEDTLS_ECP_C"
                " -DMBEDTLS_PEM_WRITE_C"
                " -DMBEDTLS_PK_WRITE_C"
                " -DMBEDTLS_X509_CREATE_C"
                " -DMBEDTLS_X509_CRT_WRITE_C"
            )
        if self.context.jobs > 1:
            print(
                f"AmebaD object compilation uses {self.context.jobs} jobs; "
                "image phases remain serial."
            )
        self.command = ["make"]
        if not verbose_build_enabled():
            self.command.extend(["-s", "--no-print-directory"])
        self.command.append(f"JOBS={self.context.jobs}")
        self.compiler = str(toolchain / compiler_subdir / "bin" / "arm-none-eabi-gcc")
        self.environment = cross_build_environment()
        gate = str(_compiler_gate())
        self.environment["NEWMOTA_COMPILER_GATE_LOCK"] = f"{gate}.lock"
        cache = find_ccache()
        if cache:
            # ccache starts the driver itself and skips it entirely on a direct hit.
            self.environment["CCACHE_PREFIX"] = gate
            self.environment["CCACHE_PREFIX_CPP"] = gate
            self.compile_command = f"{cache} {self.compiler}"
        else:
            self.compile_command = f"{gate} {self.compiler}"
        self.command.append(f"CC={self.compile_command}")
        self.command.append(f"CPPFLAGS_EXTRA={definitions}")
        self.command.append(
            "NEWMOTA_USE_BERRY=" + ("1" if self.context.features.enabled("USE_BERRY") else "0")
        )
        self.command.append(f"CFGDIR={self.bootcfg_directory}")

    def pre_build(self) -> None:
        generate_berry(self.context)
        include = self.project / "src" / "src_hp" / "include"
        asset = include / "web_assets.h"
        if self.context.features.enabled("USE_WEB"):
            copy_if_changed(
                generate_web_assets(
                    self.context.work_dir / "config",
                    self.context.environment.language,
                    dict(self.context.features.assignments),
                    encoding="br" if self.context.features.enabled("USE_WEB_HTTPS") else "gzip",
                ),
                asset,
            )
        else:
            asset.unlink(missing_ok=True)
        if self.context.features.enabled("USE_RECOVERY_WEB"):
            copy_if_changed(
                generate_recovery_web_assets(self.context.work_dir / "config"),
                include / "recovery_web_assets.h",
            )
        else:
            (include / "recovery_web_assets.h").unlink(missing_ok=True)
        stage_berry_generated_headers(
            self.context,
            include / "berry" / "generated",
        )

    def build(self) -> None:
        for project, label in (("project_lp", "AmebaD KM0"), ("project_hp", "AmebaD KM4")):
            cwd = self.release / project
            if verbose_build_enabled():
                run(self.command, cwd=cwd, environment=self.environment)
                continue
            # Report each compiled source on one status line, as the ESP-IDF build does.
            command = [argument for argument in self.command if not argument.startswith("CC=")]
            command.append(f"CC={compile_status_compiler(self.compile_command)}")
            run_build(
                command,
                label=label,
                log=self.context.work_dir / f"native-{project}.log",
                cwd=cwd,
                environment=self.environment,
            )

    def build_application(self) -> None:
        """Build the coupled KM0/KM4 image used for a wired OTA2 update."""

        self.build()

    def application_update(self) -> ApplicationUpdate:
        """Validate the coupled application image against the OTA2 slot."""

        if self.layout is None:
            raise BuildError("AmebaD application output requested before prepare")
        image = self.release / "project_hp" / "asdk" / "image" / "km0_km4_image2.bin"
        if not image.is_file():
            raise BuildError(f"AmebaD application image is missing: {image}")
        image_size = image.stat().st_size
        if image_size > self.layout.ota2.size:
            raise BuildError(
                f"AmebaD application image is {image_size} bytes but OTA2 allows "
                f"{self.layout.ota2.size} bytes"
            )
        print(
            f"Application update: {image.relative_to(ROOT)} -> OTA2 at "
            f"{self.layout.ota2.offset:#x} ({image_size}/{self.layout.ota2.size} bytes)"
        )
        return ApplicationUpdate(image, self.layout.ota2.offset, self.layout.ota2.size)

    def compilation_database(self) -> Path:
        log = self.context.work_dir / "clangd-compile-records.jsonl"
        log.unlink(missing_ok=True)
        for staged in self.source_map:
            for suffix in (".o", ".d"):
                staged.with_suffix(suffix).unlink(missing_ok=True)
        recorder = recorder_command(log, self.compiler)
        command = [argument for argument in self.command if not argument.startswith("CC=")]
        command.append(f"CC={recorder}")
        run(command, cwd=self.release / "project_hp", environment=self.environment)
        return write_recorded_database(
            log,
            self.context.work_dir / "clangd-native-compile_commands.json",
            self.source_map,
        )

    def post_build(self) -> tuple[Artifact, ...]:
        lp_image = self.release / "project_lp" / "asdk" / "image"
        hp_image = self.release / "project_hp" / "asdk" / "image"
        artifacts = [
            Artifact(lp_image / "km0_boot_all.bin", "native"),
            Artifact(hp_image / "km4_boot_all.bin", "native"),
            Artifact(hp_image / "km0_km4_image2.bin", "native"),
            Artifact(hp_image / "target_img2.axf", "debug"),
            Artifact(hp_image / "target_img2.map", "debug"),
        ]
        if self.context.environment.role == "application":
            artifacts.append(Artifact(hp_image / "OTA_All.bin", "native"))
        return tuple(artifacts)

    def package(self) -> PackageResult:
        loader = _flashloader(self.context.sdk.source)
        if self.context.environment.role == "safeboot":
            from newmota.platforms.amebad.images import validate_amebad_safeboot

            return validate_amebad_safeboot(self.context)
        return package_amebad(self.context, loader)


def native_requirements(
    environment: Environment, values: dict[str, bool], layout: LayoutFacts | None
) -> tuple[tuple[str, ...], tuple[str, ...], dict[str, str]]:
    """Translate common feature selection into this SDK's sources and settings."""
    sources = [
        "platform/amebad/digital_gpio.cpp",
        "platform/amebad/platform.cpp",
        "platform/amebad/native/entry.cpp",
        "platform/amebad/native/sdk_errno.c",
    ]
    if environment.role == "safeboot":
        sources.append("platform/amebad/recovery_update_writer.cpp")
    if values.get("USE_DEEPSLEEP", False):
        sources.append("platform/amebad/deep_sleep.cpp")
    sdk_requirements: list[str] = []
    native_settings: dict[str, str] = {}
    if values.get("USE_BLE", False):
        sources.extend(("platform/amebad/ble.cpp", "platform/amebad/ble_support.cpp"))
        sdk_requirements.append("Realtek GAP/GATT native BLE")
        # The adapter owns both roles; do not select the SDK example applications.
        native_settings.update({"CONFIG_BT": "y", "CONFIG_BT_EN": "y"})
        native_settings.update(
            {
                f"CONFIG_BT_{role}": "n"
                for role in (
                    "PERIPHERAL",
                    "CENTRAL",
                    "SCATTERNET",
                    "BEACON",
                    "CONFIG",
                    "AIRSYNC_CONFIG",
                    "ANCS",
                    "BREEZE",
                    "DISTANCE_DETECTOR",
                    "TAG_SCANNER",
                    "OTA_CLIENT",
                    "OTA_SERVER",
                    "MESH_PROVISIONER",
                    "MESH_PROVISIONER_RTK_DEMO",
                    "MESH_DEVICE",
                    "MESH_DEVICE_RTK_DEMO",
                    "MESH_PROVISIONER_MULTIPLE_PROFILE",
                    "MESH_DEVICE_MULTIPLE_PROFILE",
                    "MESH_PERIPHERAL",
                    "MESH_CENTRAL",
                    "MESH_SCATTERNET",
                )
            }
        )
    if values.get("USE_PWM", False) or values.get("USE_WS2812", False):
        sources.append("platform/amebad/pwm.cpp")
    sources.extend(
        (
            "app/support/crc32_portable.cpp",
            "platform/amebad/filesystem.cpp",
            "third_party/littlefs/lfs.c",
        )
    )
    if (
        values.get("USE_MQTT", False)
        or values.get("USE_OTA_PULL", False)
        or (values.get("USE_I2S", False) and values.get("USE_WIFI", False))
    ):
        sources.append("platform/lwip/tcp_stream.cpp")
    if values.get("USE_WEB", False) or values.get("USE_RECOVERY_WEB", False):
        sources.append("platform/amebad/web.cpp")
    return tuple(sources), tuple(sdk_requirements), native_settings


def native_target_issue(board: BoardDefinition) -> str | None:
    """Read-only check that the pinned SDK contains the selected target."""
    sdk = load_sdk_version("amebad").source
    target = board.chip.sdk_target
    project = sdk / "project" / str(board.sdk_board)
    if not project.is_dir():
        return f"missing AmebaD project {display_path(project)}"
    header = sdk / "component" / "soc" / "realtek" / "amebad" / "fwlib" / "include" / f"{target}.h"
    return None if header.is_file() else f"missing AmebaD target {display_path(header)}"


def board_issues(board: BoardDefinition) -> list[str]:
    """Additional native board constraints, beyond catalog and target validation."""
    return []
