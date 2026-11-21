"""Pico wired-device adapter using the pinned picotool CLI."""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from itertools import pairwise
from pathlib import Path

from newmota.device import DeviceAdapter, DeviceError, ProbeResult, discover_ports
from newmota.factory_package import FactoryPackage
from newmota.host import SdkVersionError, check_sdk_version, load_sdk_version, run, tools_root
from newmota.model import ROOT, WORK_DIR, ApplicationUpdate, Environment
from newmota.package import FirmwarePackage, LayoutFacts, PackageError
from newmota.platforms.pico.images import (
    RP2040_LAYOUT_IDENTITY_PREFIX,
    PicoLayoutError,
    resolve_layout,
)
from newmota.progress import WAIT_SECONDS, notice

# Pinned picotool release driving every Pico wired operation.
PROGRAMMER_VERSION = "2.3.1"
# USB handoff, read-back, and component writes should all complete promptly.
# A bounded subprocess prevents a missing/locked CDC endpoint from making
# `make flash` appear hung forever; a full 4 MiB backup remains within this
# allowance on the USB boot-ROM transport.
PICOTOOL_TIMEOUT_SECONDS = 30.0
# picotool's exit status when no RP-series device in BOOTSEL mode is accessible.
PICOTOOL_NO_DEVICE = 249
BOOTSEL_RETRY_SECONDS = 2.0
BOOTSEL_HINT = "hold BOOTSEL while reconnecting USB (or while pressing RESET)"
# All picotool memory addresses are XIP bus addresses, never flash offsets.
XIP_BASE = 0x10000000
BOOTSEL_PORT = "usb-bootsel"
MONITOR_ENUMERATION_TIMEOUT = 10.0

# Expected on-device partition ranges, printed by `picotool partition info`
# as lowercase hex `<start>-><end>` flash offsets.
EXPECTED_RANGES = (
    "00000000->000d0000",
    "000d0000->003a2000",
    "003a2000->003fd000",
)
EXPECTED_PARTITIONS = ("safeboot", "application", "data")

_PARTITION_LINE_PATTERN = re.compile(
    r'^\s*\d+\([^)]*\)\s+([0-9a-f]{8})->([0-9a-f]{8})\s+(.*?)"([^"]+)"(.*)$'
)

# picotool prints the numeric index and link field first, so a logical entry
# always starts with `<index>(<link>)`.
_PARTITION_ENTRY_START_PATTERN = re.compile(r"^\s*\d+\([^)]*\)")
# picotool quotes family names in `uf2 { 'rp2350-arm-s' }`.
_UF2_FAMILY_PATTERN = re.compile(r"uf2\s*\{([^}]*)\}")

_FLASH_SIZE_PATTERN = re.compile(r"flash size:\s*([0-9]+)\s*[kK]")
_RP2040_PATTERN = re.compile(r"\brp2040\b", re.IGNORECASE)
_RP2350_PATTERN = re.compile(r"\brp2350(?:a|b)?\b", re.IGNORECASE)


class PicoProgrammerError(DeviceError):
    """Raised when picotool cannot run or reports a failure."""

    returncode: int | None = None


class PicoMonitorOpenError(PicoProgrammerError):
    """Raised when the CDC endpoint is temporarily absent during reset."""


class _PicoMiniterm:
    """Small pyserial-miniterm adapter that turns USB disconnects into status.

    A Pico reset intentionally removes and re-enumerates its USB CDC device.
    pyserial's stock miniterm marks that as an unhandled reader-thread
    exception, which leaves the user without a monitor after every restart.
    Keeping the exception inside the reader lets the outer Pico monitor reopen
    the same CDC endpoint after enumeration.
    """

    def __init__(self, serial_instance: object, miniterm: object, serial_module: object):
        self._serial_module = serial_module
        self.disconnected = False

        # pyserial's Miniterm is intentionally duck-typed here.  Keeping the
        # subclass local avoids importing pyserial during ordinary build and
        # flash commands on hosts that only use picotool.
        parent = miniterm.Miniterm

        class ReconnectingMiniterm(parent):
            def reader(inner_self):  # type: ignore[no-untyped-def]
                try:
                    return super(ReconnectingMiniterm, inner_self).reader()
                except (serial_module.SerialException, OSError):
                    self.disconnected = True
                    # SerialException is already handled by miniterm's base
                    # reader; OSError is raised by some USB backends before
                    # it reaches that handler.  Set both flags explicitly so
                    # the writer cannot remain blocked on a vanished CDC
                    # endpoint, then suppress the traceback.
                    inner_self.alive = False
                    inner_self._reader_alive = False
                    try:
                        inner_self.console.cancel()
                    except OSError:
                        pass
                    return None

        # Rebuild with the reconnecting subclass while retaining all of the
        # standard miniterm settings and hotkeys.
        self._term = ReconnectingMiniterm(
            serial_instance,
            eol="crlf",
            # As in common_monitor: escape sequences reach the terminal.
            filters=("direct",),
        )
        self._term.set_rx_encoding("UTF-8")
        self._term.set_tx_encoding("UTF-8")

    def run(self) -> bool:
        """Run one terminal session; return true when USB disconnected."""

        term = self._term
        serial_instance = term.serial
        sys.stderr.write(
            f"--- Miniterm on {serial_instance.name}  "
            f"{serial_instance.baudrate},{serial_instance.bytesize},"
            f"{serial_instance.parity},{serial_instance.stopbits} ---\n"
            "--- Quit: Ctrl-] | Menu: Ctrl-T | Help: Ctrl-T followed by Ctrl-H ---\n"
        )
        term.start()
        try:
            # The writer exits for both Ctrl-] and a reader disconnect.  The
            # flag above distinguishes those paths without changing miniterm's
            # familiar interactive controls.
            term.join(True)
        except KeyboardInterrupt:
            term.stop()
        finally:
            term.join()
            term.close()
            term.console.cleanup()
        return self.disconnected


def _programmer() -> list[str]:
    """Return the pinned picotool invocation for this host."""

    sdk = check_sdk_version("pico-sdk")
    if sdk.programmer != "picotool" or sdk.programmer_status == "unselected":
        raise PicoProgrammerError(f"Pico programmer is not picotool: {sdk.programmer}")
    if sdk.programmer_version != PROGRAMMER_VERSION:
        raise PicoProgrammerError(
            f"Pico programmer is {sdk.programmer_version}, expected {PROGRAMMER_VERSION}"
        )
    root = tools_root() / "pico-sdk" / f"picotool-{sdk.programmer_version}" / "picotool"
    executable = root.with_name("picotool.exe") if os.name == "nt" else root / "picotool"
    if not executable.is_file():
        raise PicoProgrammerError(f"Pico programmer is missing: {executable}")
    return [str(executable)]


def _logical_partition_entries(text: str) -> list[str]:
    """Return `picotool partition info` output as one string per logical entry.

    picotool mixes plain `printf` for the index, range, and attribute fields
    with a column-tracking formatter for the permission field.  The formatter
    only learns about the `printf` columns indirectly, so when stdout is not a
    terminal (the probed/piped case) it falls back to an 80-column budget and
    soft-wraps a long entry across two physical lines.  Unfolding continuation
    lines keeps every identity check below independent of line structure while
    still rejecting foreign or virgin tables.
    """

    entries: list[str] = []
    for raw in text.lower().splitlines():
        line = " ".join(raw.split())
        if not line:
            continue
        if _PARTITION_ENTRY_START_PATTERN.match(line):
            entries.append(line)
        elif entries and '"' not in entries[-1]:
            # A continuation line only belongs to an entry that has not yet
            # reached its quoted name field.
            entries[-1] = f"{entries[-1]} {line}"
    return entries


def _uf2_families(attributes: str) -> set[str]:
    """Return the UF2 family names picotool reports for one partition."""

    match = _UF2_FAMILY_PATTERN.search(attributes)
    if match is None:
        return set()
    return {token.strip(" '\t") for token in match.group(1).split(",") if token.strip()}


def parse_partition_info(text: str) -> dict[str, object]:
    """Validate `picotool partition info` output against the Newmota layout.

    The parser stays format-tolerant (no line-structure assumptions) but
    exact on identity: every expected range and every expected partition
    name must appear, so a foreign or virgin table fails closed.
    """

    entries = [
        match.groups()
        for match in (
            _PARTITION_LINE_PATTERN.match(line) for line in _logical_partition_entries(text)
        )
        if match is not None
    ]
    identity = [(f"{start}->{end}", name) for start, end, _, name, _ in entries]
    expected = list(zip(EXPECTED_RANGES, EXPECTED_PARTITIONS, strict=True))
    if identity != expected:
        raise PicoProgrammerError(
            "device partition table does not exactly match the Newmota Pico layout"
        )
    for _, _, permissions, name, attributes in entries:
        if not all(token in permissions for token in ("s(rw)", "nsboot(rw)", "ns(-)")):
            raise PicoProgrammerError(f"device partition permissions differ for {name}")
        family = "data" if name == "data" else "rp2350-arm-s"
        if family not in _uf2_families(attributes):
            raise PicoProgrammerError(f"device UF2 family differs for {name}")
    return {"ranges": list(EXPECTED_RANGES), "partitions": list(EXPECTED_PARTITIONS)}


def parse_device_flash_size(text: str) -> int | None:
    """Return the `picotool info` flash capacity in bytes, if reported."""

    match = _FLASH_SIZE_PATTERN.search(text)
    if match is None:
        return None
    return int(match.group(1)) * 1024


def validate_device_chip(text: str, expected: str) -> None:
    """Reject a BOOTSEL target whose reported silicon differs from the catalog."""

    patterns = {"rp2040": _RP2040_PATTERN, "rp2350": _RP2350_PATTERN}
    pattern = patterns.get(expected)
    if pattern is None or pattern.search(text) is None:
        raise PicoProgrammerError(f"connected Pico does not report expected chip {expected}")


def rp2040_layout_identity_matches(
    flash_image: bytes, safeboot_size: int, layout_identity: str
) -> bool:
    """Confirm the expected layout marker is embedded in the resident Safeboot."""

    if (
        safeboot_size <= 0
        or safeboot_size > len(flash_image)
        or re.fullmatch(r"[0-9a-f]{64}", layout_identity) is None
    ):
        return False
    marker = RP2040_LAYOUT_IDENTITY_PREFIX + layout_identity.encode("ascii")
    return marker in flash_image[:safeboot_size]


class PicoFlasher(DeviceAdapter):
    """Flash a paired Pico image set through USB BOOTSEL picotool operations.

    Every write uses an explicit XIP bus address from the package manifest
    and is verified by read-back compare, so an addressing mistake fails
    closed instead of misplacing an image.  The device must be in BOOTSEL
    mode (physical access).
    """

    platform_name = "Raspberry Pi Pico"

    @staticmethod
    def _layout(environment: Environment, work_dir: Path) -> LayoutFacts:
        try:
            return resolve_layout(
                environment,
                load_sdk_version("pico-sdk"),
                work_dir,
            )
        except (OSError, SdkVersionError, PicoLayoutError, ValueError) as error:
            raise PicoProgrammerError(
                f"cannot resolve the selected Pico layout: {error}"
            ) from error

    def resolve_programming_port(self, requested: str | None) -> str:
        """Select the Pico USB BOOTSEL transport, not a serial device."""

        if requested and requested != BOOTSEL_PORT:
            raise PicoProgrammerError(
                "Pico programming uses USB BOOTSEL/picotool, not a serial port; "
                "leave PORT unset (use MONITOR_PORT for USB CDC logs)"
            )
        return BOOTSEL_PORT

    @staticmethod
    def _wait_for_monitor_port() -> str:
        """Wait for the application USB CDC port after a BOOTSEL reboot."""

        deadline = time.monotonic() + MONITOR_ENUMERATION_TIMEOUT
        while time.monotonic() < deadline:
            candidates = discover_ports()
            if len(candidates) == 1:
                print(f"Auto-selected Pico monitor port: {candidates[0].label()}")
                return candidates[0].device
            if len(candidates) > 1:
                devices = ", ".join(candidate.device for candidate in candidates)
                raise PicoProgrammerError(
                    "multiple serial devices found after Pico reboot; pass "
                    f"MONITOR_PORT explicitly: {devices}"
                )
            time.sleep(0.1)
        raise PicoProgrammerError(
            "Pico USB CDC monitor did not enumerate; pass MONITOR_PORT or "
            "verify that the application was flashed and USB stdio is enabled"
        )

    def resolve_monitor_port(self, requested: str | None, programming_port: str) -> str:
        """Use an explicit CDC port or wait for it after USB BOOTSEL flashing."""

        if requested and requested != BOOTSEL_PORT:
            return requested
        if programming_port and programming_port != BOOTSEL_PORT:
            return programming_port
        return self._wait_for_monitor_port()

    @staticmethod
    def _wait_for_monitor_reconnect(previous: str) -> str:
        """Wait for the CDC endpoint to return after a Pico reset."""

        deadline = time.monotonic() + MONITOR_ENUMERATION_TIMEOUT
        while time.monotonic() < deadline:
            candidates = discover_ports()
            if any(candidate.device == previous for candidate in candidates):
                return previous
            # macOS may allocate a new usbmodem suffix after a reset.  If the
            # Pico is the only board present, following that new path is safe;
            # with multiple boards we keep waiting instead of attaching to the
            # wrong console.
            if len(candidates) == 1:
                return candidates[0].device
            time.sleep(0.1)
        raise PicoProgrammerError(
            "Pico USB CDC monitor did not re-enumerate after reset; "
            "verify USB connectivity or pass MONITOR_PORT explicitly"
        )

    @staticmethod
    def _run_monitor_session(port: str, baud: int) -> bool:
        """Run one miniterm session and report whether USB disappeared."""

        try:
            import serial
            from serial.tools import miniterm
        except ImportError as error:
            raise PicoProgrammerError(
                "Pico monitor requires the project pyserial dependency"
            ) from error

        try:
            serial_instance = serial.serial_for_url(
                port,
                baudrate=baud,
                parity=serial.PARITY_NONE,
                rtscts=False,
                xonxoff=False,
                do_not_open=True,
            )
            if not hasattr(serial_instance, "cancel_read"):
                serial_instance.timeout = 1
            if isinstance(serial_instance, serial.Serial):
                serial_instance.exclusive = True
            serial_instance.open()
        except serial.SerialException as error:
            raise PicoMonitorOpenError(
                f"could not open Pico monitor port {port!r}: {error}"
            ) from error

        return _PicoMiniterm(serial_instance, miniterm, serial).run()

    def monitor(
        self,
        environment: Environment,
        package: FirmwarePackage | None,
        port: str,
        baud: int | None,
        after_flash: bool,
    ) -> None:
        """Keep the USB CDC monitor alive across Pico resets."""

        del environment, package, after_flash
        monitor_baud = baud or self.default_monitor_baud
        current_port = port
        while True:
            try:
                disconnected = self._run_monitor_session(current_port, monitor_baud)
            except PicoMonitorOpenError:
                # A reset can happen in the short gap between port discovery
                # and opening it.  Treat that exactly like a reader disconnect
                # and wait for the endpoint instead of failing the monitor.
                current_port = self._wait_for_monitor_reconnect(current_port)
                continue
            if not disconnected:
                return
            print("Pico USB CDC disconnected; waiting for reset/re-enumeration.", flush=True)
            current_port = self._wait_for_monitor_reconnect(current_port)

    @staticmethod
    def _run_picotool(arguments: list[str], *, show_progress: bool = False) -> str:
        """Run picotool; show_progress lets its own progress bar reach the terminal.

        Output is returned only when captured, so parsed commands keep show_progress off.
        """

        try:
            completed = run(
                [*_programmer(), *arguments],
                capture=not show_progress,
                timeout=PICOTOOL_TIMEOUT_SECONDS,
            )
        except subprocess.CalledProcessError as error:
            detail = (error.stderr or error.stdout or "").strip()
            if not detail and error.returncode == PICOTOOL_NO_DEVICE:
                detail = (
                    "no accessible RP-series device in BOOTSEL mode; "
                    "automatic USB-CDC-to-BOOTSEL transition failed, so hold BOOTSEL "
                    "while reconnecting the board"
                )
            failure = PicoProgrammerError(
                f"picotool failed (exit {error.returncode})" + (f": {detail}" if detail else "")
            )
            failure.returncode = error.returncode
            raise failure from error
        except subprocess.TimeoutExpired as error:
            raise PicoProgrammerError(
                "picotool timed out after "
                f"{PICOTOOL_TIMEOUT_SECONDS:g}s while running "
                f"{' '.join(arguments)}; close any monitor, then hold BOOTSEL "
                "while reconnecting the Pico and retry"
            ) from error
        except OSError as error:
            raise PicoProgrammerError(f"picotool could not start: {error}") from error
        return completed.stdout or ""

    def _enter_bootsel(self) -> str:
        """Bring the board into BOOTSEL, repeating the instruction until WAIT_SECONDS.

        A running Newmota image is switched automatically (-F); otherwise the user acts.
        Returns the device-only `picotool info` text.
        """

        notice("Pico: looking for the board (a running Newmota image switches to BOOTSEL) ...")
        deadline = time.monotonic() + WAIT_SECONDS
        while True:
            try:
                info = self._run_picotool(["info", "-d", "-F"])
            except PicoProgrammerError as error:
                if error.returncode != PICOTOOL_NO_DEVICE:
                    raise
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise PicoProgrammerError(
                        f"no Pico in BOOTSEL mode within {WAIT_SECONDS} s; "
                        f"{BOOTSEL_HINT}, then retry"
                    ) from error
                notice(
                    f"Pico: no BOOTSEL device. Enter BOOTSEL now: {BOOTSEL_HINT} "
                    f"({round(remaining)} s left)"
                )
                time.sleep(min(BOOTSEL_RETRY_SECONDS, remaining))
            else:
                notice("Pico: BOOTSEL device connected")
                return info

    @staticmethod
    def _save_all_flash_arguments(environment: Environment, destination: Path) -> list[str]:
        """Build a complete-flash read using the target's known capacity."""

        if environment.sdk_target == "rp2040":
            # RP2040 info identifies the chip but picotool cannot infer flash
            # capacity. Read the catalog-declared range in XIP address space.
            flash_bytes = environment.flash_kb * 1024
            selection = [
                "-r",
                f"0x{XIP_BASE:x}",
                f"0x{XIP_BASE + flash_bytes:x}",
            ]
        else:
            selection = ["-a"]
        return ["save", *selection, str(destination), "-t", "bin", "-F"]

    def _probe_target(
        self,
        environment: Environment,
        port: str,
        baud: int | None,
        *,
        inspect_layout: bool,
        require_layout: bool,
        keep_bootsel: bool,
    ) -> ProbeResult:
        """Identify one BOOTSEL target and optionally validate its resident table."""

        del port, baud
        # A running Pico SDK application exposes the picotool protocol over
        # USB CDC, but is not a BOOTSEL target yet.  -F performs that reset
        # and deliberately keeps the ROM transport available for the rest of
        # a multi-command operation (factory flash/update/backup).  Request
        # device-only information here: the default `info` command also walks
        # every embedded partition and can get stuck on a stale/incomplete
        # diagnostic image before the flasher has a chance to repair it.
        info = self._enter_bootsel()
        validate_device_chip(info, environment.chip)
        flash_bytes = environment.flash_kb * 1024
        reported = parse_device_flash_size(info)
        capacity_source = "declared-unverified"
        if reported is not None:
            if reported != flash_bytes:
                raise PicoProgrammerError(
                    f"device reports {reported} flash bytes, expected {flash_bytes}"
                )
            capacity_source = "picotool-info"
        work_dir = WORK_DIR / environment.name / "flash" / f"{flash_bytes}"
        work_dir.mkdir(parents=True, exist_ok=True)
        table: dict[str, object] | None = None
        table_status = "not-inspected"
        if inspect_layout:
            if environment.sdk_target == "rp2040":
                layout = self._layout(environment, work_dir / "layout")
                safeboot = layout.region("safeboot")
                try:
                    with tempfile.TemporaryDirectory(
                        prefix="rp2040-layout-", dir=work_dir
                    ) as temporary:
                        dump = Path(temporary) / "flash.bin"
                        self._run_picotool(self._save_all_flash_arguments(environment, dump))
                        flash_image = dump.read_bytes()
                    if len(flash_image) != flash_bytes:
                        table_status = "mismatch"
                        table = {
                            "identity": layout.identity,
                            "flash_image_bytes": len(flash_image),
                        }
                    else:
                        matches = rp2040_layout_identity_matches(
                            flash_image, safeboot.size, layout.identity
                        )
                        table_status = "newmota-match" if matches else "mismatch"
                        table = {
                            "identity": layout.identity,
                            "safeboot_identity": matches,
                        }
                except (OSError, PicoProgrammerError):
                    if require_layout:
                        raise PicoProgrammerError(
                            "RP2040 application update requires a readable flash dump and "
                            "matching resident Safeboot layout identity"
                        ) from None
                    table_status = "unavailable"
                if require_layout and table_status != "newmota-match":
                    raise PicoProgrammerError(
                        "RP2040 application update requires the installed Newmota Safeboot "
                        "with the selected flash layout"
                    )
            else:
                try:
                    table_text = self._run_picotool(["partition", "info"])
                except PicoProgrammerError:
                    if require_layout:
                        raise PicoProgrammerError(
                            "Pico application update requires the installed Newmota partition table"
                        ) from None
                    table_status = "unavailable"
                else:
                    table = parse_partition_info(table_text)
                    table_status = "newmota-match"
        probe_path = work_dir / "probe.json"
        probe_path.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "platform": environment.platform,
                    "chip": environment.chip,
                    "flash_bytes": flash_bytes,
                    "capacity_source": capacity_source,
                    "partition_table_status": table_status,
                    "partition_table": table,
                    "device_info": info,
                    "warning": "capacity is the catalog declaration unless picotool "
                    "info reports it; keep BOOTSEL access for recovery",
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"Pico probe: {probe_path.relative_to(ROOT)}")
        if not keep_bootsel:
            # `make probe` should be observational and return the board to its
            # application rather than leaving it in the ROM transport.
            self._run_picotool(["reboot", "-a"])
        return ProbeResult(
            platform=environment.platform,
            chip=environment.chip,
            port="usb-bootsel",
            flash_bytes=flash_bytes,
            identifier=None,
            security={"secure_boot": None, "flash_encryption": None},
            work_dir=work_dir,
        )

    def probe(self, environment: Environment, port: str, baud: int | None) -> ProbeResult:
        """Identify the target and report whether a Newmota table is installed."""

        return self._probe_target(
            environment,
            port,
            baud,
            inspect_layout=True,
            require_layout=False,
            keep_bootsel=False,
        )

    def backup(
        self,
        environment: Environment,
        port: str,
        output: Path,
        baud: int | None,
    ) -> None:
        """Read the complete physical flash without overwriting an existing file."""

        del port, baud
        if output.exists():
            raise PicoProgrammerError(f"backup output already exists: {output}")
        flash_bytes = environment.flash_kb * 1024
        work = WORK_DIR / environment.name / "flash"
        work.mkdir(parents=True, exist_ok=True)
        try:
            validate_device_chip(self._enter_bootsel(), environment.chip)
            self._run_picotool(
                self._save_all_flash_arguments(environment, output), show_progress=True
            )
        except PicoProgrammerError as error:
            raise PicoProgrammerError(f"Pico backup failed: {error}") from error
        if not output.is_file():
            raise PicoProgrammerError("picotool produced no backup output")
        data = output.read_bytes()
        if len(data) != flash_bytes:
            raise PicoProgrammerError(f"backup is {len(data)} bytes, expected {flash_bytes}")
        sidecar = output.with_suffix(output.suffix + ".json")
        sidecar.write_text(
            json.dumps(
                {
                    "environment": environment.name,
                    "flash_bytes": flash_bytes,
                    "sha256": hashlib.sha256(data).hexdigest(),
                    "warning": "a complete backup contains credentials and user data",
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        self._run_picotool(["reboot", "-a"])
        print(f"Pico backup: {output} ({len(data)} bytes)")

    @staticmethod
    def _images(package: FirmwarePackage) -> list[tuple[str, int, Path]]:
        flash = package.data.get("flash")
        if not isinstance(flash, dict) or flash.get("protocol") != "picotool-usb":
            raise PackageError("package has no Pico USB flash plan")
        raw_components = flash.get("components")
        if not isinstance(raw_components, list) or not raw_components:
            raise PackageError("Pico flash plan has no components")
        layout = package.data.get("layout")
        if not isinstance(layout, dict) or layout.get("flash_bytes") != package.flash_bytes:
            raise PackageError("Pico package has no valid flash layout metadata")

        def region(name: str) -> tuple[int, int]:
            regions = layout.get("regions")
            if not isinstance(regions, list):
                raise PackageError("Pico package layout has no regions")
            matches = [
                item for item in regions if isinstance(item, dict) and item.get("name") == name
            ]
            if len(matches) != 1:
                raise PackageError(f"Pico package layout is missing region {name}")
            offset = matches[0].get("offset")
            size = matches[0].get("size")
            if (
                not isinstance(offset, int)
                or isinstance(offset, bool)
                or not isinstance(size, int)
                or isinstance(size, bool)
                or offset < 0
                or size <= 0
                or offset + size > package.flash_bytes
            ):
                raise PackageError(f"Pico package layout has an invalid {name} region")
            return offset, size

        application_offset, application_size = region("application")
        safeboot_offset, safeboot_size = region("safeboot")
        filesystem_offset, filesystem_size = region("littlefs")
        expected = {
            "safeboot.bin": (safeboot_offset, safeboot_size),
            "application.bin": (application_offset, application_size),
            "littlefs.bin": (filesystem_offset, filesystem_size),
        }
        parsed: dict[str, tuple[int, Path]] = {}
        for index, raw in enumerate(raw_components):
            if not isinstance(raw, dict):
                raise PackageError(f"Pico flash component {index} is invalid")
            name = raw.get("name")
            role = raw.get("role")
            path_name = raw.get("path")
            offset = raw.get("offset")
            if (
                not isinstance(name, str)
                or not isinstance(role, str)
                or not isinstance(path_name, str)
                or not isinstance(offset, int)
                or isinstance(offset, bool)
            ):
                raise PackageError(f"Pico flash component {index} is incomplete")
            if name not in expected:
                raise PackageError(f"Pico flash plan contains an unexpected image: {name}")
            if role != "factory-component":
                raise PackageError(f"Pico image is not a factory artifact: {name}")
            if offset != expected[name][0] or name in parsed:
                raise PackageError(f"Pico image has an invalid or duplicate offset: {name}")
            path = package.file(path_name)
            size = path.stat().st_size
            if not size or offset + size > package.flash_bytes:
                raise PicoProgrammerError(f"Pico image exceeds the package flash capacity: {name}")
            if offset + size > expected[name][0] + expected[name][1]:
                raise PicoProgrammerError(f"Pico image exceeds its region: {name}")
            parsed[name] = (offset, path)

        if set(parsed) != set(expected):
            missing = sorted(set(expected) - set(parsed))
            raise PackageError("Pico flash plan is missing: " + ", ".join(missing))
        ranges = sorted(
            (offset, offset + path.stat().st_size, name) for name, (offset, path) in parsed.items()
        )
        if any(left[1] > right[0] for left, right in pairwise(ranges)):
            raise PackageError("Pico factory flash components overlap")
        return [(name, parsed[name][0], parsed[name][1]) for name in expected]

    @staticmethod
    def _validate_target(environment: Environment, package: FirmwarePackage) -> None:
        target = package.data.get("target")
        if not isinstance(target, dict):
            raise PackageError("Pico package has no target metadata")
        if (
            target.get("environment") != environment.name
            or target.get("chip") != environment.chip
            or target.get("sdk_target") != environment.sdk_target
        ):
            raise PicoProgrammerError(
                "Pico package does not match the selected catalog environment"
            )

    def _write_component(
        self, name: str, offset: int, path: Path, *, ignore_partitions: bool
    ) -> None:
        bus = XIP_BASE + offset
        size = path.stat().st_size
        arguments = ["load", "-v", "-u"]
        if ignore_partitions:
            arguments.append("--ignore-partitions")
        arguments.extend([str(path), "-t", "bin", "-o", f"{bus:#x}"])
        notice(f"Pico: writing {name} ({size} bytes)")
        try:
            self._run_picotool(arguments, show_progress=True)
        except PicoProgrammerError as error:
            raise PicoProgrammerError(f"Pico write failed: {name}: {error}") from error
        try:
            self._run_picotool(
                [
                    "verify",
                    str(path),
                    "-t",
                    "bin",
                    "-o",
                    f"{bus:#x}",
                    "-r",
                    f"{bus:#x}",
                    f"{bus + size:#x}",
                ],
                show_progress=True,
            )
        except PicoProgrammerError as error:
            raise PicoProgrammerError(f"Pico verification failed: {name}: {error}") from error

    def flash(
        self,
        environment: Environment,
        package: FirmwarePackage,
        port: str,
        baud: int | None,
    ) -> None:
        """Probe, plan, write, verify, and reset a Pico target."""

        self._validate_target(environment, package)
        images = self._images(package)
        probe = self._probe_target(
            environment,
            port,
            baud,
            inspect_layout=False,
            require_layout=False,
            keep_bootsel=True,
        )
        flash_bytes = package.flash_bytes
        if probe.flash_bytes != flash_bytes:
            raise PicoProgrammerError(
                f"connected Pico has {probe.flash_bytes} bytes, package requires {flash_bytes}"
            )
        work_dir = WORK_DIR / environment.name / "flash" / f"{flash_bytes}"
        work_dir.mkdir(parents=True, exist_ok=True)
        plan_path = work_dir / "flash-plan.json"
        plan_path.write_text(
            json.dumps(
                {
                    "environment": environment.name,
                    "protocol": "picotool-usb",
                    "components": [
                        {
                            "name": name,
                            "offset": offset,
                            "bus_address": XIP_BASE + offset,
                            "size": path.stat().st_size,
                        }
                        for name, offset, path in images
                    ],
                    "preserves": [
                        region["name"]
                        for region in package.data["layout"]["regions"]
                        if isinstance(region, dict) and region.get("role") == "reserved"
                    ],
                    "note": (
                        "the sticky-boot marker sector is outside every "
                        "written range and survives factory flashing"
                    ),
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"Pico flash plan: {plan_path.relative_to(ROOT)}")
        for name, offset, path in images:
            self._write_component(name, offset, path, ignore_partitions=True)
        self._run_picotool(["reboot", "-a"])
        print("Pico flash verified by read-back compare.")

    def flash_factory(
        self,
        environment: Environment,
        factory: FactoryPackage,
        port: str,
        baud: int | None,
    ) -> None:
        """Write a factory package from its description alone, as the web flasher does."""

        factory.steps((), "Pico")
        flash_bytes = factory.description["target"]["flash_bytes"]
        probe = self._probe_target(
            environment,
            port,
            baud,
            inspect_layout=False,
            require_layout=False,
            keep_bootsel=True,
        )
        if probe.flash_bytes != flash_bytes:
            raise PicoProgrammerError(
                f"connected Pico has {probe.flash_bytes} bytes, package requires {flash_bytes}"
            )
        work_dir = WORK_DIR / environment.name / "flash" / f"{flash_bytes}" / "factory-package"
        images = factory.write_images(work_dir)
        plan_path = work_dir / "flash-plan.json"
        plan_path.write_text(
            json.dumps(
                {
                    "environment": environment.name,
                    "package": str(factory.path),
                    "protocol": "picotool-usb",
                    "components": [
                        {
                            "name": name,
                            "offset": offset,
                            "bus_address": XIP_BASE + offset,
                            "size": path.stat().st_size,
                        }
                        for name, offset, path in images
                    ],
                    "preserves": factory.description["preserve"],
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"Pico flash plan: {plan_path.relative_to(ROOT)}")
        for name, offset, path in images:
            self._write_component(name, offset, path, ignore_partitions=True)
        self._run_picotool(["reboot", "-a"])
        print("Pico flash verified by read-back compare.")

    def update(
        self,
        environment: Environment,
        update: ApplicationUpdate,
        port: str,
        baud: int | None,
    ) -> None:
        """Write and verify only the normal application slot."""

        if environment.role != "application":
            raise PicoProgrammerError("Pico application update requires an application environment")
        layout = self._layout(environment, WORK_DIR / environment.name / "layout-validation")
        application = layout.region("application")
        if update.offset != application.offset or update.size_limit != application.size:
            raise PicoProgrammerError("Pico application update does not match the application slot")
        if update.offset + update.size_limit > environment.flash_kb * 1024:
            raise PicoProgrammerError("Pico application update partition exceeds declared flash")
        if not update.image.is_file():
            raise PicoProgrammerError(f"Pico application update image is missing: {update.image}")
        image_size = update.image.stat().st_size
        if image_size == 0 or image_size > update.size_limit:
            raise PicoProgrammerError("Pico application update image does not fit its slot")
        self._probe_target(
            environment,
            port,
            baud,
            inspect_layout=True,
            require_layout=True,
            keep_bootsel=True,
        )
        plan_path = WORK_DIR / environment.name / "update-plan.json"
        plan_path.parent.mkdir(parents=True, exist_ok=True)
        preserved_regions = [
            "safeboot",
            "littlefs",
            *[region.name for region in layout.regions if region.role == "reserved"],
        ]
        if environment.sdk_target != "rp2040":
            preserved_regions.append("partition-table")
        plan_path.write_text(
            json.dumps(
                {
                    "environment": environment.name,
                    "protocol": "picotool-usb",
                    "partition": "application",
                    "offset": update.offset,
                    "bus_address": XIP_BASE + update.offset,
                    "size": image_size,
                    "size_limit": update.size_limit,
                    "image": str(update.image.relative_to(ROOT)),
                    "preserves": preserved_regions,
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"Pico update plan: {plan_path.relative_to(ROOT)}")
        self._write_component(
            "application.bin",
            update.offset,
            update.image,
            ignore_partitions=environment.sdk_target == "rp2040",
        )
        self._run_picotool(["reboot", "-a"])
        print("Pico update verified by read-back compare.")
