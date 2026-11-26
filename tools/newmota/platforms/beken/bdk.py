"""Newmota-owned GNU Make wrapper for the common Beken BDK.

The BDK deliberately ships sources, chip libraries, and linker scripts but no
application build system.  This module keeps the source selection in Newmota
and emits a small, inspectable Makefile for one target.  The source groups
mirror the reviewed BDK/LibreTiny selection; the BDK checkout remains
read-only.
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

from newmota.host import write_if_changed
from newmota.model import ROOT, BuildContext, BuildError
from newmota.platforms.beken.images import explicit_native_regions, load_layout_document
from newmota.platforms.beken.profile import BUILD_TARGETS, BekenTargetProfile


class BekenBdkBuildError(BuildError):
    """Raised when the pinned BDK cannot provide one selected build input."""


LIBC_ALLOCATIONS = ("malloc", "calloc", "realloc", "free")
LIBC_HEAP_WRAPS = LIBC_ALLOCATIONS + tuple(f"_{name}_r" for name in LIBC_ALLOCATIONS)


def validate_runtime_symbols(symbol_table: str) -> None:
    """Reject a second heap or discarded C++ constructors before packaging."""

    symbols = {}
    for line in symbol_table.splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    forbidden = set(LIBC_HEAP_WRAPS) | {"_sbrk", "_sbrk_r"}
    present = sorted(forbidden.intersection(symbols))
    if present:
        raise BekenBdkBuildError(
            "Beken image links an independent libc heap: " + ", ".join(present)
        )
    for name in ("__wrap_malloc", "__wrap_free", "__init_array_start", "__init_array_end"):
        if name not in symbols:
            raise BekenBdkBuildError(f"Beken image is missing runtime integration: {name}")
    if symbols["__init_array_end"] <= symbols["__init_array_start"]:
        raise BekenBdkBuildError("Beken image discarded the application C++ constructors")


def _append_file(destination: list[str], root: Path, relative: str, prefix: str) -> None:
    path = root / relative
    if not path.is_file():
        raise BekenBdkBuildError(f"BDK target input is missing: {path.relative_to(root.parent)}")
    item = f"{prefix}/{relative}"
    if item not in destination:
        destination.append(item)


def _append_glob(
    destination: list[str],
    root: Path,
    pattern: str,
    prefix: str,
    excluded: set[str] | None = None,
) -> None:
    excluded = excluded or set()
    matches = sorted(path for path in root.glob(pattern) if path.is_file())
    for path in matches:
        relative = path.relative_to(root)
        # Callers use the short filename for a single-directory glob and the
        # full relative path for recursive exclusions.  Accept both forms so
        # target-specific exclusions cannot silently leak excluded vendor sources into
        # the selected BDK build.
        if str(relative) in excluded or path.name in excluded:
            continue
        _append_file(destination, root, str(relative), prefix)


# BK7252N builds its own variants of these shared BDK drivers.
BK7252N_DRIVER_VARIANTS = {
    "driver/general_dma/general_dma.c": "driver/general_dma/general_dma_bk7252n.c",
    "driver/i2c/i2c1.c": "driver/i2c/i2c1_bk7252n.c",
    "driver/i2s/i2s.c": "driver/i2s/i2s_bk7252n.c",
    "driver/irda/irda.c": "driver/irda/irda_bk7252n.c",
    "driver/jpeg/jpeg_encoder.c": "driver/jpeg/jpeg.c",
    "driver/qspi/qspi.c": "driver/qspi/qspi_bk7252n.c",
}


def _source_groups(
    profile: BekenTargetProfile, use_ble: bool
) -> tuple[list[str], list[str], list[str]]:
    """Return selected BDK C/C++/assembler paths for this feature set."""

    if profile.spec.sdk_target not in BUILD_TARGETS:
        raise BekenBdkBuildError(
            f"the BDK wrapper currently supports {', '.join(sorted(BUILD_TARGETS))}, "
            f"not {profile.spec.sdk_target}"
        )

    root = profile.source_root
    c_sources: list[str] = []
    cpp_sources: list[str] = []
    asm_sources: list[str] = []

    def c_file(relative: str) -> None:
        _append_file(c_sources, root, relative, "beken378")

    def c_glob(pattern: str, excluded: set[str] | None = None) -> None:
        _append_glob(c_sources, root, pattern, "beken378", excluded)

    # BDK core groups.  The stock app/app.c and app/ate_app.c are deliberately
    # not selected: they combine the vendor demo/CLI/application startup with
    # the Wi-Fi transport bridge.  Newmota supplies the small, owned startup
    # and transport bridge in platform/beken/native/beken_app.c instead.
    for relative in (
        "app/config/param_config.c",
        "driver/entry/arch_main.c",
        "driver/intc/intc.c",
        "func/wlan_ui/wlan_ui.c",
    ):
        c_file(relative)
    for relative in ("driver/entry/boot_vectors.S", "driver/entry/boot_handlers.S"):
        _append_file(asm_sources, root, relative, "beken378")

    for pattern in (
        "app/http/*.c",
        "app/net_work/*.c",
        "app/standalone-ap/*.c",
        "app/standalone-station/*.c",
        "app/video_work/*.c",
    ):
        c_glob(pattern)

    # BDK driver sources.  Chip-specific IP and radio code is supplied by the
    # selected static libraries below, as in the reviewed BDK builder.
    for pattern, excluded in (
        ("driver/*.c", set()),
        ("driver/common/*.c", set()),
        ("driver/dma/*.c", set()),
        ("driver/fft/*.c", set()),
        ("driver/flash/*.c", set()),
        ("driver/gpio/*.c", set()),
        ("driver/icu/*.c", set()),
        ("driver/macphy_bypass/*.c", set()),
        ("driver/phy/*.c", set()),
        ("driver/pwm/*.c", {"pwm_bk7271.c", "pwm_new.c"}),
        ("driver/rw_pub/*.c", set()),
        ("driver/saradc/*.c", set()),
        ("driver/security/*.c", set()),
        ("driver/spi/*.c", {"spi_psram.c"}),
        ("driver/spidma/*.c", set()),
        ("driver/sys_ctrl/*.c", set()),
        ("driver/uart/*.c", {"printf.c"}),
        ("driver/wdt/*.c", set()),
        ("driver/calendar/calendar.c", set()),
        ("driver/general_dma/general_dma.c", set()),
        ("driver/i2c/i2c1.c", set()),
        ("driver/i2c/i2c2.c", set()),
        ("driver/i2s/i2s.c", set()),
        ("driver/irda/irda.c", set()),
        ("driver/jpeg/jpeg_encoder.c", set()),
        ("driver/qspi/qspi.c", set()),
    ):
        c_glob(pattern, excluded)
    if profile.spec.sdk_target == "bk7252n":
        for index, item in enumerate(c_sources):
            variant = BK7252N_DRIVER_VARIANTS.get(item.removeprefix("beken378/"))
            if variant is not None:
                c_sources[index] = "beken378/" + variant
        # Its Wi-Fi library and power save use the RTC and the IP checksum unit,
        # and its device table registers the HPM, LA and YUV buffer drivers.
        for relative in (
            "driver/hpm/hpm.c",
            "driver/ipchksum/ipchksum.c",
            "driver/la/la.c",
            "driver/rtc/rtc_reg.c",
            "driver/yuv_buf/yuv_buf.c",
        ):
            c_file(relative)

    # Functional groups used by the stock station/network startup path.
    for relative in ("func/func.c",):
        c_file(relative)
    for pattern, excluded in (
        ("func/airkiss/*.c", set()),
        ("func/base64/*.c", set()),
        ("func/bk7011_cal/*.c", set()),
        ("func/ble_wifi_exchange/*.c", set()),
        ("func/camera_intf/*.c", set()),
        ("func/force_sleep/*.c", set()),
        ("func/hostapd_intf/*.c", set()),
        ("func/joint_up/*.c", set()),
        ("func/lwip_intf/dhcpd/*.c", set()),
        # The BDK fake clock supplies fclk_* used by the Wi-Fi/BLE libraries.
        # LibreTiny replaces it with an equivalent fixup; the common BDK
        # wrapper keeps the native implementation in this build instead.
        ("func/misc/*.c", set()),
        ("func/net_param_intf/*.c", set()),
        ("func/ntp/*.c", set()),
        (
            "func/power_save/*.c",
            # The targets whose BDK configuration enables CFG_LOW_VOLTAGE_PS.
            set()
            if profile.spec.sdk_target in {"bk7238", "bk7252n"}
            else {"low_voltage_compensation.c", "low_voltage_ps.c"},
        ),
        ("func/rtc/*.c", {"alarm.c"}),
        ("func/rwnx_intf/*.c", set()),
        ("func/saradc_intf/*.c", set()),
        ("func/security/*.c", set()),
        ("func/sim_uart/*.c", set()),
        ("func/spidma_intf/*.c", set()),
        ("func/temp_detect/*.c", set()),
        ("func/usb_plug/*.c", set()),
        ("func/user_driver/*.c", {"BkDriverQspi.c"}),
        ("func/utf8/*.c", set()),
        ("func/video_transfer/*.c", set()),
    ):
        c_glob(pattern, excluded)
    c_file("func/wpa_supplicant_2_9/src/crypto/crypto_mbedtls.c")

    # The BDK carries its lwIP and mbedTLS source trees, rather than exposing a
    # second SDK package to the application build.
    # Match LibreTiny's lwip-bdk external-library selection.  The checkout
    # also contains optional applications and the host-side makefsdata tool;
    # compiling the whole tree would pull those unrelated sources into the
    # firmware.
    for pattern in (
        "func/lwip_intf/lwip-2.0.2/src/api/*.c",
        "func/lwip_intf/lwip-2.0.2/src/core/*.c",
        "func/lwip_intf/lwip-2.0.2/src/core/ipv4/*.c",
        "func/lwip_intf/lwip-2.0.2/src/core/ipv6/*.c",
        "func/lwip_intf/lwip-2.0.2/src/netif/ethernet.c",
        "func/lwip_intf/lwip-2.0.2/src/apps/mdns/*.c",
        "func/lwip_intf/lwip-2.0.2/src/apps/sntp/sntp.c",
        "func/lwip_intf/lwip-2.0.2/port/*.c",
    ):
        c_glob(pattern)
    c_glob("func/mbedtls/mbedtls/library/*.c")
    c_glob("func/mbedtls/mbedtls-port/src/*.c")

    if use_ble:
        ble_root = (
            "driver/ble/ble_5_2" if profile.spec.ble_version == "5.2" else "driver/ble/ble_5_1"
        )
        if profile.spec.ble_version == "5.2":
            ble_patterns = (
                ("ble_pub/app/src/*.c", {"app_ble_task.c"}),
                ("ble_pub/ui/ble_ui.c", set()),
                ("ble_pub/profiles/bas/bass/src/bass.c", set()),
                ("ble_pub/profiles/bk_comm/src/*.c", set()),
                ("ble_pub/profiles/bk_sdp/src/sdp_common.c", set()),
                ("ble_pub/profiles/dis/diss/src/diss.c", set()),
                ("ble_pub/profiles/find/findt/src/findt.c", set()),
                ("ble_pub/profiles/hogp/hogpd/src/hogpd.c", set()),
                ("hci/controller_hci.c", set()),
                (
                    f"platform/{profile.spec.ble_platform}/**/*.c",
                    {f"platform/{profile.spec.ble_platform}/nvds/src/nvds.c"},
                ),
            )
        else:
            ble_patterns = (
                ("ble_pub/app/src/*.c", {"app_ble_task.c"}),
                ("ble_pub/ui/ble_ui.c", set()),
                ("ble_pub/prf/*.c", set()),
                ("ble_pub/profiles/comm/src/*.c", set()),
                ("ble_pub/profiles/sdp/src/*.c", set()),
                ("platform/7231n/**/*.c", {"platform/7231n/nvds/src/nvds.c"}),
            )
        for pattern, excluded in ble_patterns:
            _append_glob(
                c_sources,
                root / ble_root,
                pattern,
                f"beken378/{ble_root}",
                excluded,
            )

    # FreeRTOS itself is kept at the BDK package root.  The Beken adaptation
    # layer lives below beken378/os and must be linked alongside the kernel.
    freertos_root = profile.sdk_root / "os"
    freertos_source_root = "FreeRTOSv9.0.0/FreeRTOS/Source"
    for relative in (
        f"{freertos_source_root}/croutine.c",
        f"{freertos_source_root}/event_groups.c",
        f"{freertos_source_root}/list.c",
        f"{freertos_source_root}/queue.c",
        f"{freertos_source_root}/tasks.c",
        f"{freertos_source_root}/timers.c",
        f"{freertos_source_root}/portable/Keil/ARM968es/port.c",
        f"{freertos_source_root}/portable/MemMang/heap_4.c",
    ):
        _append_file(c_sources, freertos_root, relative, "os")
    _append_glob(c_sources, root, "os/**/*.c", "beken378", {"os/platform_stub.c"})

    return c_sources, cpp_sources, asm_sources


def _stage_https_server_source(profile: BekenTargetProfile, overlay_root: Path) -> str:
    """Stage the pinned BDK server source with its dynamic-buffer pointer fix."""
    source = profile.source_root / "func" / "mbedtls" / "mbedtls" / "library" / "ssl_srv.c"
    try:
        content = source.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise BekenBdkBuildError(f"cannot read Beken Mbed TLS server source: {source}") from error

    broken = "const unsigned char * const end = ssl->out_msg + MBEDTLS_SSL_MAX_CONTENT_LEN;"
    fixed = "const unsigned char *end = ssl->out_msg + MBEDTLS_SSL_MAX_CONTENT_LEN;"
    if broken in content:
        content = content.replace(broken, fixed, 1)
    elif fixed not in content:
        raise BekenBdkBuildError(
            "Beken Mbed TLS ssl_srv.c no longer matches the reviewed dynamic-buffer fix"
        )

    relative = "apps/newmota/src/newmota_beken_mbedtls_ssl_srv.c"
    destination = overlay_root / "src" / "newmota_beken_mbedtls_ssl_srv.c"
    write_if_changed(destination, content, encoding="utf-8")
    return relative


PARTITION_ENTRIES = (
    ("BK_PARTITION_BOOTLOADER", "bootloader", "Bootloader"),
    ("BK_PARTITION_APPLICATION", "application", "Application"),
    ("BK_PARTITION_OTA", "ota", "ota"),
    ("BK_PARTITION_RF_FIRMWARE", "rf", "RF Firmware"),
    ("BK_PARTITION_NET_PARAM", "net", "NET info"),
    ("BK_PARTITION_BLE_BONDING_FLASH", "ble-bonding", "ble bonding info"),
)


def _stage_partition_table(
    profile: BekenTargetProfile, context: BuildContext, overlay_root: Path
) -> str | None:
    """Compile the layout's own regions as the runtime BDK table, for vendor layouts.

    BkDriverFlash.c includes "BkFlashPartition.h" with quotes, so a copy staged next to
    a generated header uses that header instead of the SDK one. Returns the replaced
    SDK source, or None when the layout uses the BDK table itself.
    """
    if context.environment.partition is None or context.layout is None:
        raise BekenBdkBuildError("Beken partition table staging requires a resolved layout")
    regions = explicit_native_regions(load_layout_document(context.environment.partition))
    if regions is None:
        return None
    if context.layout.flash_bytes != 0x200000:
        raise BekenBdkBuildError("Beken vendor layouts are defined for the 2 MiB table only")
    source = profile.source_root / "func" / "user_driver" / "BkDriverFlash.c"
    try:
        driver = source.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise BekenBdkBuildError(f"cannot read Beken flash driver source: {source}") from error
    if driver.count('#include "BkFlashPartition.h"') != 1:
        raise BekenBdkBuildError(
            "Beken BkDriverFlash.c no longer includes its partition table exactly once"
        )
    entries = "\n".join(
        f"""    [{native}] =
    {{
        .partition_owner           = BK_FLASH_EMBEDDED,
        .partition_description     = "{description}",
        .partition_start_addr      = {regions[name].offset:#08x},
        .partition_length          = {regions[name].size:#07x},
        .partition_options         = PAR_OPT_READ_EN | PAR_OPT_WRITE_DIS,
    }},"""
        for native, name, description in PARTITION_ENTRIES
    )
    descriptor = context.environment.partition.relative_to(ROOT).as_posix()
    header = f"""/* Generated from {descriptor}; replaces the BDK 2 MiB table. Do not edit. */
#ifndef CFG_FLASH_SELECTION_TYPE
#include "sys_config.h"
#endif

#ifndef __BKFLASHPARTITION_H__
#define __BKFLASHPARTITION_H__

#if (CFG_FLASH_SELECTION_TYPE != FLASH_SELECTION_TYPE_2M) || CFG_SUPPORT_MATTER
#error "Newmota vendor layouts need the fixed 2 MiB table without the BDK Matter partitions"
#endif

const bk_logic_partition_t bk7231_partitions_2M[BK_PARTITION_MAX] =
{{
{entries}
}};

#endif
"""
    directory = overlay_root / "src" / "newmota_beken_flash"
    write_if_changed(directory / "BkFlashPartition.h", header, encoding="utf-8")
    write_if_changed(directory / "BkDriverFlash.c", driver, encoding="utf-8")
    return "beken378/func/user_driver/BkDriverFlash.c"


def _include_directories(profile: BekenTargetProfile) -> list[str]:
    """Return all BDK include directories in stable order.

    The BDK has no central include manifest.  Scoping this discovery to its
    checked-out source roots is less fragile than maintaining a second list of
    hundreds of nested BLE/IP headers.
    """

    directories = {
        ".",
        "config",
        "release",
        "apps/newmota/src",
        "apps/newmota/include",
        "apps/newmota/include/berry/src",
    }
    ble_root = f"ble_{profile.spec.ble_version.replace('.', '_')}"
    for root, prefix in (
        (profile.source_root, "beken378"),
        (profile.sdk_root / "os", "os"),
    ):
        for path in sorted(root.rglob("*")):
            if path.is_dir():
                relative = path.relative_to(root)
                if (
                    prefix == "beken378"
                    and len(relative.parts) >= 3
                    and relative.parts[:2] == ("driver", "ble")
                    and relative.parts[2] != ble_root
                ):
                    # The BDK contains several BLE generations with headers
                    # such as ble.h and rwip_config.h.  Only expose the
                    # generation selected by the SoC profile; otherwise an
                    # unrelated generation can win by include-path order.
                    continue
                directories.add(f"{prefix}/{relative}")
    directories.add("apps/newmota/include/berry/generated")

    # Several BDK subsystems intentionally use short, generic header names.
    # In particular, scanu_task.h includes the LMAC scan.h, while the WPA
    # supplicant also ships a scan.h.  A purely alphabetical recursive list
    # therefore selects the wrong header and breaks the BK7238 build.  Keep
    # the BDK's LMAC scan directory ahead of the functional/WPA directories;
    # the remaining discovery stays deterministic and future-proof.
    preferred = [
        "config",
        "beken378/ip/lmac/src/scan",
        f"beken378/driver/ble/{ble_root}/platform/{profile.spec.ble_platform}/config",
        f"beken378/driver/ble/{ble_root}/platform/{profile.spec.ble_platform}/entry",
    ]
    return preferred + sorted(
        directories.difference(preferred), key=lambda value: (value != ".", value)
    )


def _silence_sdk_diagnostics(native: Path, sources: list[str]) -> list[str]:
    """Compile SDK diagnostic output as no-ops without modifying the BDK checkout."""

    patches = {
        "beken378/func/misc/mem_check.c": (
            (
                """static void mem_printf(const char *fmt, ...)
{
    va_list ap;
    char string[32];

    va_start(ap, fmt);
    vsnprintf(string, sizeof(string) - 1, fmt, ap);
    string[31] = 0;
    bk_send_string(UART1_PORT, string);
    va_end(ap);
}
""",
                "#define mem_printf(...) ((void)0)\n",
            ),
        ),
        "beken378/driver/uart/uart.c": (
            (
                "void bk_printf(const char *fmt, ...)",
                "void (bk_printf)(const char *fmt, ...)",
            ),
        ),
        "os/FreeRTOSv9.0.0/FreeRTOS/Source/portable/MemMang/heap_4.c": (
            (
                "extern void bk_printf(const char *fmt, ...);",
                "extern void (bk_printf)(const char *fmt, ...);",
            ),
        ),
        "os/FreeRTOSv9.0.0/FreeRTOS/Source/tasks.c": (),
    }
    quiet_sources = []
    for source in sources:
        replacements = patches.get(source)
        if replacements is None:
            quiet_sources.append(source)
            continue

        source_path = native / source
        quiet_path = native / "quiet" / source
        content = source_path.read_text(encoding="utf-8")
        for before, after in replacements:
            if content.count(before) != 1:
                raise BekenBdkBuildError(
                    f"cannot safely silence Beken diagnostics in {source}: "
                    "expected one source pattern"
                )
            content = content.replace(before, after, 1)
        write_if_changed(quiet_path, content, encoding="utf-8")

        if source.endswith("/tasks.c"):
            header = source_path.parent / "task_debug.h"
            quiet_header = quiet_path.parent / "task_debug.h"
            header_content = header.read_text(encoding="utf-8")
            declaration = "extern void bk_printf(const char *fmt, ...);"
            if header_content.count(declaration) != 1:
                raise BekenBdkBuildError(
                    "cannot safely silence Beken printf in task_debug.h: expected one declaration"
                )
            write_if_changed(
                quiet_header,
                header_content.replace(
                    declaration,
                    "extern void (bk_printf)(const char *fmt, ...);",
                    1,
                ),
                encoding="utf-8",
            )

        quiet_sources.append(f"quiet/{source}")
    return quiet_sources


def _disable_bk7238_ble_power_save(
    native: Path, profile: BekenTargetProfile, sources: list[str]
) -> list[str]:
    """Guard the BK7238 power-save BLE probe in a generated SDK source copy."""

    if profile.spec.sdk_target != "bk7238":
        return sources
    source = "beken378/func/power_save/power_save.c"
    if source not in sources:
        raise BekenBdkBuildError("BK7238 power-save source is missing from the selected BDK groups")
    source_path = native / source
    before = "#if (CFG_BLE_VERSION > BLE_VERSION_5_1)"
    after = "#if (CFG_SUPPORT_BLE) && (CFG_BLE_VERSION > BLE_VERSION_5_1)"
    content = source_path.read_text(encoding="utf-8")
    if content.count(before) != 1:
        raise BekenBdkBuildError(
            "cannot safely disable the BK7238 power-save BLE probe: expected one SDK source pattern"
        )
    destination = native / "ble-disabled" / source
    write_if_changed(destination, content.replace(before, after, 1), encoding="utf-8")
    return [f"ble-disabled/{source}" if item == source else item for item in sources]


def _define(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def _makefile_list(name: str, values: list[str]) -> str:
    if not values:
        return f"{name} :="
    lines = [f"{name} := " + "\\"]
    lines.extend(f"  {value} " + "\\" for value in values[:-1])
    if values:
        lines.append(f"  {values[-1]}")
    return "\n".join(lines)


def _bdk_version_define(version: str) -> str:
    """Encode 3.0.78 as the BDK's CFG_BDK_VERSION integer 30078."""

    parts = version.split(".")
    if len(parts) != 3 or any(not part.isdigit() for part in parts):
        raise BekenBdkBuildError(f"invalid BDK version: {version!r}")
    major, minor, patch = (int(part) for part in parts)
    if major > 9 or minor > 99 or patch > 99:
        raise BekenBdkBuildError(f"BDK version cannot be encoded: {version!r}")
    return f"{major}{minor:02d}{patch:02d}"


def generate_makefile(
    profile: BekenTargetProfile,
    context: BuildContext,
    native: Path,
    overlay: Path,
) -> Path:
    """Emit one deterministic BDK build file and return its path."""

    if context.layout is None:
        raise BekenBdkBuildError("BDK Makefile generation requires resolved layout facts")
    use_ble = context.features.enabled("USE_BLE")
    c_sources, _cpp_sources, asm_sources = _source_groups(profile, use_ble)
    if not use_ble:
        c_sources = _disable_bk7238_ble_power_save(native, profile, c_sources)
    overlay_root = overlay / "apps" / "newmota"
    if not overlay_root.is_dir():
        raise BekenBdkBuildError(f"Newmota Beken overlay is missing: {overlay_root}")
    if context.features.enabled("USE_WIFI_WPA3_BEARSSL"):
        # The BearSSL backend (src/platform/crypto/hostap_*.c) replaces the supplicant's
        # mbedTLS adapter; without HTTPS nothing else in the image uses mbedTLS.
        backend = "beken378/func/wpa_supplicant_2_9/src/crypto/crypto_mbedtls.c"
        if backend not in c_sources:
            raise BekenBdkBuildError(f"Beken SDK source list lacks {backend}")
        c_sources.remove(backend)
        if not context.features.enabled("USE_WEB_HTTPS"):
            c_sources = [
                path for path in c_sources if not path.startswith("beken378/func/mbedtls/")
            ]
    if context.features.enabled("USE_WEB_HTTPS"):
        server_source = "beken378/func/mbedtls/mbedtls/library/ssl_srv.c"
        if server_source not in c_sources:
            raise BekenBdkBuildError("Beken HTTPS requires its source-built Mbed TLS server")
        c_sources.remove(server_source)
        _stage_https_server_source(profile, overlay_root)
    replaced = _stage_partition_table(profile, context, overlay_root)
    if replaced is not None:
        if replaced not in c_sources:
            raise BekenBdkBuildError(f"Beken SDK source list lacks {replaced}")
        c_sources.remove(replaced)
    else:
        shutil.rmtree(overlay_root / "src" / "newmota_beken_flash", ignore_errors=True)
    for suffix in ("*.c", "*.cpp"):
        for path in sorted((overlay_root / "src").rglob(suffix)):
            relative = f"apps/newmota/src/{path.relative_to(overlay_root / 'src')}"
            if suffix == "*.cpp":
                _cpp_sources.append(relative)
            else:
                c_sources.append(relative)

    output = overlay_root / "output" / context.version
    output_relative = Path(os.path.relpath(output, native)).as_posix()
    target = context.environment
    try:
        sdk_log_level = int(context.native_config.values["CFG_LOG_LEVEL"])
    except (KeyError, TypeError, ValueError) as error:
        raise BekenBdkBuildError("Beken SDK log level is missing or invalid") from error
    if sdk_log_level < 0 or sdk_log_level > 5:
        raise BekenBdkBuildError(f"Beken SDK log level is out of range: {sdk_log_level}")
    if sdk_log_level == 0:
        c_sources = _silence_sdk_diagnostics(native, c_sources)
    write_if_changed(
        native / "config" / "sdk_log.h",
        f"""/* Generated by tools/newmota/platforms/beken/bdk.py; do not edit. */
#ifndef NEWMOTA_BEKEN_SDK_LOG_H
#define NEWMOTA_BEKEN_SDK_LOG_H
/* CFG_LOG_LEVEL controls BK_LOG* compilation in the BDK.  Load the BDK
 * declarations before replacing its vendor printf entry points so headers
 * retain valid prototypes.  At level none, source-built SDK diagnostics and
 * assertion messages are compiled out; prebuilt-library calls are silenced
 * by the link wrapper. */
#ifdef CFG_LOG_LEVEL
#undef CFG_LOG_LEVEL
#endif
#define CFG_LOG_LEVEL {sdk_log_level}

#if CFG_LOG_LEVEL == 0
#ifdef __cplusplus
extern "C" {{
#endif
#include "beken378/common/generic.h"
#include "beken378/driver/include/uart_pub.h"
#ifdef __cplusplus
}}
#endif

#undef os_printf
static inline void (os_printf)(const char *format, ...)
{{
    (void)format;
}}
#define os_printf(...) ((void)0)
#undef bk_printf
#define bk_printf(...) ((void)0)
#undef warning_prf
#define warning_prf(...) ((void)0)
#undef fatal_prf
#define fatal_prf(...) ((void)0)
#undef NEWMOTA_BEKEN_ASSERT
#define NEWMOTA_BEKEN_ASSERT(condition) \\
    do {{ if (!(condition)) {{ for (;;) {{}} }} }} while (0)
#undef ASSERT_EQ
#define ASSERT_EQ(a, b) NEWMOTA_BEKEN_ASSERT((a) == (b))
#undef ASSERT_NE
#define ASSERT_NE(a, b) NEWMOTA_BEKEN_ASSERT((a) != (b))
#undef ASSERT_GT
#define ASSERT_GT(a, b) NEWMOTA_BEKEN_ASSERT((a) > (b))
#undef ASSERT_GE
#define ASSERT_GE(a, b) NEWMOTA_BEKEN_ASSERT((a) >= (b))
#undef ASSERT_LT
#define ASSERT_LT(a, b) NEWMOTA_BEKEN_ASSERT((a) < (b))
#undef ASSERT_LE
#define ASSERT_LE(a, b) NEWMOTA_BEKEN_ASSERT((a) <= (b))
#undef ASSERT
#define ASSERT(condition) NEWMOTA_BEKEN_ASSERT(condition)
#undef BK_ASSERT
#define BK_ASSERT(condition) NEWMOTA_BEKEN_ASSERT(condition)
#endif

#endif
""",
    )
    filesystem = context.layout.region("filesystem")
    net = context.layout.region("net")
    definitions = [
        "CFG_OS_FREERTOS=1",
        'MBEDTLS_CONFIG_FILE=\\"tls_config.h\\"',
        "WIFI_BLE_COEXIST=1",
        "WOLFSSL_BEKEN=1",
        "MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED",
        "INCLUDE_xTaskGetHandle=1",
        # func.c prints this value next to BEKEN_SDK_REV.  The public BDK
        # does not ship a generated VCS header, matching LibreTiny's NULL
        # definition for released framework packages.
        "SDK_COMMIT_ID=NULL",
        # The BDK's FreeRTOS port prints the analogous OSK revision and its
        # released package likewise has no VCS commit string.
        "OSK_COMMIT_ID=NULL",
        f"CFG_BDK_VERSION={_bdk_version_define(context.sdk.version)}",
        f"PLATFORM_{target.sdk_target.upper()}=1",
        "PLATFORM_BEKEN=1",
        f'NEWMOTA_CHIP_NAME=\\"{_define(target.chip)}\\"',
        f'NEWMOTA_TARGET_NAME=\\"{_define(target.name)}\\"',
        f'NEWMOTA_SDK_VERSION=\\"{_define(context.sdk.version)}\\"',
        f'NEWMOTA_FRAMEWORK_VERSION=\\"{_define(context.version)}\\"',
        f'NEWMOTA_BUILD_DATE=\\"{_define(context.build_date)}\\"',
        f'NEWMOTA_BUILD_TIME=\\"{_define(context.build_time)}\\"',
        "NEWMOTA_FILESYSTEM_ENABLED=1",
        "LFS_MULTIVERSION",
        f"NEWMOTA_FILESYSTEM_OFFSET={filesystem.offset:#x}",
        f"NEWMOTA_FILESYSTEM_SIZE={filesystem.size:#x}",
        f"NEWMOTA_FILESYSTEM_BLOCK_SIZE={context.layout.erase_size:#x}",
        f"NEWMOTA_NET_PARAM_OFFSET={net.offset:#x}",
        f"NEWMOTA_NET_PARAM_SIZE={net.size:#x}",
        "NEWMOTA_IMAGE_APPLICATION=1",
    ]
    if context.features.enabled("USE_WEB_HTTPS"):
        # The BDK's TLS profile is client-only and disables certificate writing.
        # Mbed TLS supports an additive user config, so enable only the server,
        # P-256 ECDSA, PEM, and X.509 writer pieces needed by the web identity.
        write_if_changed(
            overlay_root / "include" / "newmota_beken_mbedtls_web_config.h",
            """/* Generated by Newmota for the HTTPS application profile. */
#ifndef NEWMOTA_BEKEN_MBEDTLS_WEB_CONFIG_H
#define NEWMOTA_BEKEN_MBEDTLS_WEB_CONFIG_H
#define MBEDTLS_ECDSA_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PEM_WRITE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
#endif
""",
        )
        definitions.append('MBEDTLS_USER_CONFIG_FILE=\\"newmota_beken_mbedtls_web_config.h\\"')
    if context.features.enabled("USE_WIFI_WPA3_BEARSSL"):
        definitions.extend(
            (
                # The prebuilt (upstream hostap) sae.o does not free crypto_ec_get_a().
                "NEWMOTA_HOSTAP_EC_A_BORROWED=1",
                # BearSSL's P-256 code keeps its work on the stack; mbedTLS used the heap.
                "NEWMOTA_SUPPLICANT_STACK_SIZE=8192",
            )
        )
    if context.features.enabled("USE_MQTT"):
        definitions.append("MQTT_DO_NOT_USE_CUSTOM_CONFIG=1")
    if context.features.enabled("USE_MATTER"):
        # The BDK enables IPv6 only with its own Matter option; Newmota builds
        # lwIP itself. Each address joins a solicited-node group and DNS-SD adds
        # ff02::fb, so the default four MLD groups leave no margin. Matter
        # messages fit the IPv6 minimum MTU, so reassembly stays off.
        definitions.extend(("LWIP_IPV6=1", "MEMP_NUM_MLD6_GROUP=6", "LWIP_IPV6_REASS=0"))
    include_flags = " ".join(f"-I{directory}" for directory in _include_directories(profile))
    define_flags = " ".join(f"-D{item}" for item in definitions)
    common_flags = " ".join(  # noqa: FLY002 - keep audited compiler flags one per line
        (
            "-g",
            "-mcpu=arm968e-s",
            "-march=armv5te",
            "-mthumb",
            "-mthumb-interwork",
            "-mlittle-endian",
            "-Os",
            "-ffunction-sections",
            "-fdata-sections",
            "-fsigned-char",
            "-fcommon",
            "-nostdlib",
            include_flags,
            define_flags,
        )
    )
    # The BDK needs these suppressions; Newmota objects keep -Wall instead.
    sdk_warning_flags = " ".join(  # noqa: FLY002 - keep audited compiler flags one per line
        (
            "-Wno-write-strings",
            "-Wno-attributes",
            "-Wno-comment",
            "-Wno-char-subscripts",
            "-Wno-missing-braces",
            "-Wno-return-type",
            "-Wno-format",
            "-Wno-unknown-pragmas",
            "-Wno-unused-function",
            "-Wno-unused-but-set-variable",
        )
    )
    # These diagnostics exist only for C; passing them to C++ makes GCC warn per file.
    sdk_c_warning_flags = " ".join(  # noqa: FLY002 - keep audited compiler flags one per line
        (
            "-Wno-implicit-function-declaration",
            "-Wno-implicit-int",
            "-Wno-int-conversion",
            "-Wno-incompatible-pointer-types",
        )
    )
    linker = profile.spec.linker_script
    suffix = profile.spec.library_suffix
    library_flags = [
        "-L./beken378/lib",
        "-L./beken378/func/airkiss",
        "-Wl,--start-group",
        "-lairkiss",
        "-lsensor",
    ]
    if use_ble:
        library_flags.append(f"-lble_{suffix}")
    library_flags.extend(
        (
            f"-lcal_{suffix}",
            f"-lrf_test_{suffix}",
            f"-lrf_use_{suffix}",
            f"-lrwnx_{suffix}",
            f"-lsupplicant_{suffix}",
            f"-luart_debug_{suffix}",
            "-lstdc++",
            "-lgcc",
            "-lg",
            "-lc",
            "-lm",
            "-lnosys",
            "-Wl,--end-group",
        )
    )
    libraries = " ".join(library_flags)
    wrapped_symbols = (
        LIBC_HEAP_WRAPS
        + (("bk_printf",) if sdk_log_level == 0 else ())
        + (("os_get_random",) if context.features.enabled("USE_WIFI") else ())
        + (
            ("rtos_create_thread",)
            if context.features.enabled("USE_WIFI_WPA3_BEARSSL")
            else ()
        )
    )
    runtime_wraps = " ".join(f"-Wl,--wrap={name}" for name in wrapped_symbols)
    objects_c = [f"$(OBJ_DIR)/{path[:-2]}.o" for path in c_sources]
    objects_cpp = [f"$(OBJ_DIR)/{path[:-4]}.o" for path in _cpp_sources]
    objects_s = [
        f"$(OBJ_DIR)/{path[:-1]}O"
        if path.endswith("boot_handlers.S")
        else f"$(OBJ_DIR)/{path[:-2]}.o"
        for path in asm_sources
    ]
    all_objects = objects_c + objects_cpp + objects_s
    # The staged Mbed TLS server copy is BDK code and keeps the SDK suppressions.
    newmota_objects = [
        path
        for path in objects_c + objects_cpp
        if path.startswith("$(OBJ_DIR)/apps/newmota/src/")
        and not path.endswith("/newmota_beken_mbedtls_ssl_srv.o")
    ]
    makefile = native / "bdk.mk"
    content = (
        "\n".join(
            (
                "# Generated by tools/newmota/platforms/beken/bdk.py; do not edit.",
                "SHELL := /bin/sh",
                "CROSS_COMPILE ?= arm-none-eabi-",
                "CC = $(CROSS_COMPILE)gcc",
                "OBJCOPY = $(CROSS_COMPILE)objcopy",
                "OBJDUMP = $(CROSS_COMPILE)objdump",
                "OBJ_DIR := Debug/obj",
                f"OUTPUT_DIR := {output_relative}",
                f"RAW_ELF := $(OUTPUT_DIR)/newmota_{context.version}.axf",
                f"RAW_BIN := $(OUTPUT_DIR)/newmota_{context.version}.bin",
                f"RAW_MAP := $(OUTPUT_DIR)/newmota_{context.version}.map",
                f"RAW_ASM := $(OUTPUT_DIR)/newmota_{context.version}.asm",
                f"INCLUDE_FLAGS := {include_flags}",
                f"DEFINE_FLAGS := {define_flags}",
                f"COMMON_FLAGS := {common_flags} -include config/sdk_log.h",
                f"WARNING_FLAGS := {sdk_warning_flags}",
                f"C_WARNING_FLAGS := {sdk_c_warning_flags}",
                # Recursive so the Newmota object overrides below take effect.
                "CFLAGS = $(COMMON_FLAGS) $(WARNING_FLAGS) $(C_WARNING_FLAGS) -std=gnu99 -MMD -MP",
                # BDK release.h pastes version macros onto string literals.
                (
                    "CXXFLAGS = $(COMMON_FLAGS) $(WARNING_FLAGS) -std=gnu++17 -MMD -MP "
                    "-fno-exceptions -fno-rtti -Wno-literal-suffix"
                ),
                "ASFLAGS := -g -mcpu=arm968e-s -march=armv5te -marm -mthumb-interwork -x assembler-with-cpp $(INCLUDE_FLAGS) $(DEFINE_FLAGS)",
                _makefile_list("SRC_C", c_sources),
                _makefile_list("SRC_CPP", _cpp_sources),
                _makefile_list("SRC_S", asm_sources),
                _makefile_list("OBJECTS", all_objects),
                _makefile_list("NEWMOTA_OBJECTS", newmota_objects),
                "$(NEWMOTA_OBJECTS): WARNING_FLAGS := -Wall",
                "$(NEWMOTA_OBJECTS): C_WARNING_FLAGS :=",
                "",
                ".PHONY: application clean",
                "application: $(RAW_BIN) $(RAW_ELF) $(RAW_MAP) $(RAW_ASM)",
                "",
                # The BDK linker scripts select memory regions with #if on the
                # sys_config.h values; ld itself would skip only the directives.
                f"LINKER_SCRIPT := $(OBJ_DIR)/{linker}",
                f"$(LINKER_SCRIPT): build/{linker} bdk.mk",
                "\t@mkdir -p $(dir $@)",
                "\t$(CC) -E -P -x c -MMD -MP -MF $@.d -MT $@ $(INCLUDE_FLAGS) $(DEFINE_FLAGS) $< -o $@",
                "",
                # ARMv5TE is single-core and has no barrier instruction, and the
                # BDK keeps code in RWX ITCM/RAM by design.
                "$(RAW_ELF): $(OBJECTS) bdk.mk $(LINKER_SCRIPT)",
                "\t@mkdir -p $(dir $@)",
                f"\t$(CC) -mcpu=arm968e-s -marm -mthumb-interwork --specs=nano.specs --specs=sync-none.specs -nostdlib -Wl,--gc-sections -Wl,--no-warn-rwx-segments -Wl,-Map,$(RAW_MAP) -Wl,-wrap,user_main_entry {runtime_wraps} -T$(LINKER_SCRIPT) $(OBJECTS) {libraries} -o $@",
                "",
                # The linker emits RAW_MAP as a side effect of producing RAW_ELF.
                # Declare that ordering so `make -j` never checks the map first.
                "$(RAW_MAP): $(RAW_ELF)",
                '\t@test -f $@ || { echo "linker map missing: $@" >&2; exit 1; }',
                "",
                "$(RAW_BIN): $(RAW_ELF)",
                "\t@mkdir -p $(dir $@)",
                "\t$(OBJCOPY) -O binary $< $@",
                "",
                "$(RAW_ASM): $(RAW_ELF)",
                # Source-interleaved disassembly of this BDK ELF expands without
                # bound because the release contains large debug/source tables.
                # Keep the diagnostic artifact useful and deterministic.
                "\t$(OBJDUMP) -d $< > $@",
                "",
                "$(OBJ_DIR)/%.o: %.c",
                "\t@mkdir -p $(dir $@)",
                "\t$(CC) $(CFLAGS) -c $< -o $@",
                "",
                "$(OBJ_DIR)/%.o: %.cpp",
                "\t@mkdir -p $(dir $@)",
                "\t$(CC) $(CXXFLAGS) -c $< -o $@",
                "",
                "$(OBJ_DIR)/%.o: %.S",
                "\t@mkdir -p $(dir $@)",
                "\t$(CC) $(ASFLAGS) -c $< -o $@",
                "",
                # The BDK FreeRTOS ARM968 port contains ARM-mode CPSR/SPSR
                # instructions.  LibreTiny builds this port outside the common
                # Thumb compilation flags; keep the kernel and heap in Thumb but
                # compile this one translation unit in ARM mode.
                "$(OBJ_DIR)/os/FreeRTOSv9.0.0/FreeRTOS/Source/portable/Keil/ARM968es/port.o: CFLAGS += -marm",
                # Only ARM-mode newlib calls these hooks; ARM avoids interworking veneers.
                "$(OBJ_DIR)/apps/newmota/src/libc_syscalls.o: CFLAGS += -marm",
                "",
                # The BDK linker script intentionally refers to this object with
                # an upper-case suffix for its startup stacks.
                "$(OBJ_DIR)/beken378/driver/entry/boot_handlers.O: beken378/driver/entry/boot_handlers.S",
                "\t@mkdir -p $(dir $@)",
                "\t$(CC) $(ASFLAGS) -c $< -o $@",
                "",
                # boot_handlers.O is a deliberate BDK linker-script spelling and
                # is a binary object, not a generated make dependency file.
                "-include $(filter %.d,$(OBJECTS:.o=.d)) $(LINKER_SCRIPT).d",
                "",
                "clean:",
                "\trm -rf Debug/obj $(OUTPUT_DIR)",
                "",
            )
        )
        + "\n"
    )
    write_if_changed(makefile, content)
    return makefile
