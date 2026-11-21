"""Device contract, serial helpers, and manifest-driven device workflows."""

from __future__ import annotations

import os
import sys
from abc import ABC, abstractmethod
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any

from newmota.catalog import project_version, resolve_environment
from newmota.factory_package import FactoryPackage, load_factory_package
from newmota.host import run
from newmota.model import FIRMWARE_DIR, ApplicationUpdate, BuildError, Environment
from newmota.package import FirmwarePackage, load_package, package_manifest_path
from newmota.pipeline import build_application_update, build_selected
from newmota.platforms import flasher_for


class DeviceError(BuildError):
    """Raised before or during a wired device operation."""


@dataclass(frozen=True)
class ProbeResult:
    platform: str
    chip: str
    port: str
    flash_bytes: int
    identifier: str | None
    security: dict[str, bool | str | None]
    work_dir: Path


@dataclass(frozen=True)
class SerialPort:
    device: str
    description: str | None
    manufacturer: str | None
    vid: int | None
    pid: int | None
    serial_number: str | None

    def label(self) -> str:
        details: list[str] = []
        for value in (self.description, self.manufacturer):
            clean = _terminal_text(value)
            if clean and clean.lower() not in {"n/a", "unknown"} and clean not in details:
                details.append(clean)
        if self.vid is not None and self.pid is not None:
            details.append(f"{self.vid:04x}:{self.pid:04x}")
        serial_number = _terminal_text(self.serial_number)
        if serial_number:
            details.append(f"S/N {serial_number}")
        return self.device + (" — " + " — ".join(details) if details else "")


def _terminal_text(value: str | None) -> str:
    if not value:
        return ""
    return " ".join("".join(character for character in value if character.isprintable()).split())


def _looks_like_board_port(item: Any) -> bool:
    if item.vid is not None and item.pid is not None:
        return True
    identity = " ".join(
        str(value or "") for value in (item.device, item.description, item.manufacturer, item.hwid)
    ).lower()
    markers = (
        "usb",
        "uart",
        "jtag",
        "cp210",
        "ch340",
        "ch910",
        "ftdi",
        "wch",
        "espressif",
    )
    return any(marker in identity for marker in markers)


def discover_ports() -> list[SerialPort]:
    try:
        from serial.tools.list_ports import comports
    except ImportError as error:
        raise DeviceError(
            "automatic port detection requires the project pyserial dependency"
        ) from error

    discovered = [item for item in comports() if item.device and _looks_like_board_port(item)]
    unique = {item.device: item for item in discovered}
    return [
        SerialPort(
            device=item.device,
            description=item.description,
            manufacturer=item.manufacturer,
            vid=item.vid,
            pid=item.pid,
            serial_number=item.serial_number,
        )
        for item in sorted(unique.values(), key=lambda port: port.device.lower())
    ]


def _select_port_curses() -> str | None:
    try:
        import curses
    except ImportError as error:
        raise DeviceError(
            "interactive port selection is unavailable; pass PORT=<device>"
        ) from error

    def menu(stdscr: Any) -> str | None:
        try:
            curses.curs_set(0)
        except curses.error:
            pass
        stdscr.keypad(True)
        selected = 0
        candidates = discover_ports()

        while True:
            height, width = stdscr.getmaxyx()
            stdscr.erase()
            title = "Select serial port  [up/down: move, Enter: select, r: refresh, q: cancel]"
            try:
                stdscr.addnstr(0, 0, title, max(0, width - 1), curses.A_BOLD)
            except curses.error:
                pass

            if candidates:
                selected = min(selected, len(candidates) - 1)
                visible = max(1, height - 3)
                offset = min(max(0, selected - visible + 1), max(0, len(candidates) - visible))
                for row, candidate in enumerate(candidates[offset : offset + visible], start=2):
                    index = offset + row - 2
                    marker = "> " if index == selected else "  "
                    attribute = curses.A_REVERSE if index == selected else curses.A_NORMAL
                    try:
                        stdscr.addnstr(
                            row, 0, marker + candidate.label(), max(0, width - 1), attribute
                        )
                    except curses.error:
                        pass
            else:
                message = "No typical USB/UART serial device found. Connect one and press r."
                try:
                    stdscr.addnstr(2, 0, message, max(0, width - 1))
                except curses.error:
                    pass

            stdscr.refresh()
            key = stdscr.getch()
            if key in (ord("q"), ord("Q"), 27):
                return None
            if key in (ord("r"), ord("R")):
                candidates = discover_ports()
                selected = 0
            elif candidates and key in (curses.KEY_UP, ord("k"), ord("K")):
                selected = (selected - 1) % len(candidates)
            elif candidates and key in (curses.KEY_DOWN, ord("j"), ord("J")):
                selected = (selected + 1) % len(candidates)
            elif candidates and key in (curses.KEY_ENTER, 10, 13):
                return candidates[selected].device

    try:
        return curses.wrapper(menu)
    except curses.error as error:
        raise DeviceError(
            "interactive port selection is unavailable; pass PORT=<device>"
        ) from error


def resolve_port(requested: str | None) -> str:
    if requested:
        return requested
    candidates = discover_ports()
    if len(candidates) == 1:
        print(f"Auto-selected serial port: {candidates[0].label()}")
        return candidates[0].device
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        if not candidates:
            raise DeviceError("no typical USB/UART serial device found; pass PORT=<device>")
        devices = ", ".join(candidate.device for candidate in candidates)
        raise DeviceError(f"multiple serial devices found; pass PORT=<device>: {devices}")
    selected = _select_port_curses()
    if selected is None:
        raise DeviceError("serial port selection cancelled")
    print(f"Selected serial port: {selected}")
    return selected


def common_monitor(port: str, baud: int) -> None:
    environment = os.environ.copy()
    run(
        # "direct" hands escape sequences to the terminal; the default filter prints them as ␛.
        [sys.executable, "-m", "serial.tools.miniterm", "--filter", "direct", port, str(baud)],
        environment=environment,
    )


class DeviceAdapter(ABC):
    default_monitor_baud = 115200

    def resolve_programming_port(self, requested: str | None) -> str:
        """Resolve the transport used by probe, flash, backup, and update."""

        return resolve_port(requested)

    def resolve_monitor_port(self, requested: str | None, programming_port: str) -> str:
        """Resolve the log transport, defaulting to a serial programming port."""

        return requested or programming_port or resolve_port(None)

    @abstractmethod
    def probe(self, environment: Environment, port: str, baud: int | None) -> ProbeResult:
        """Identify a connected target and its physical flash."""

    @abstractmethod
    def backup(
        self,
        environment: Environment,
        port: str,
        output: Path,
        baud: int | None,
    ) -> None:
        """Read the complete physical flash without overwriting an existing backup."""

    @abstractmethod
    def flash(
        self,
        environment: Environment,
        package: FirmwarePackage,
        port: str,
        baud: int | None,
    ) -> None:
        """Probe, plan, write, verify, and reset a target."""

    def flash_factory(
        self,
        environment: Environment,
        factory: FactoryPackage,
        port: str,
        baud: int | None,
    ) -> None:
        """Write a factory package from its description alone."""

        del factory, port, baud
        raise DeviceError(
            f"{environment.platform} cannot write a factory package yet; "
            "pass the package manifest.json instead"
        )

    def update(
        self,
        environment: Environment,
        update: ApplicationUpdate,
        port: str,
        baud: int | None,
    ) -> None:
        """Write only a freshly built normal-application partition."""

        del update, port, baud
        raise DeviceError(f"{environment.platform} application-only wired update is unavailable")

    def monitor(
        self,
        environment: Environment,
        package: FirmwarePackage | None,
        port: str,
        baud: int | None,
        after_flash: bool,
    ) -> None:
        del environment, package, after_flash
        common_monitor(port, baud or self.default_monitor_baud)


class UnqualifiedFlasher(DeviceAdapter):
    platform_name = "unknown"
    reason = "wired operations have not passed hardware qualification"

    def _unavailable(self, operation: str) -> DeviceError:
        return DeviceError(f"{self.platform_name} {operation} is unavailable: {self.reason}")

    def probe(self, environment: Environment, port: str, baud: int | None) -> ProbeResult:
        del environment, port, baud
        raise self._unavailable("probe")

    def backup(
        self,
        environment: Environment,
        port: str,
        output: Path,
        baud: int | None,
    ) -> None:
        del environment, port, output, baud
        raise self._unavailable("backup")

    def flash(
        self,
        environment: Environment,
        package: FirmwarePackage,
        port: str,
        baud: int | None,
    ) -> None:
        del environment, package, port, baud
        raise self._unavailable("flash")


def _package_environment(package: FirmwarePackage) -> Environment:
    environment = resolve_environment(package.environment)
    target = package.data["target"]
    expected = {
        "platform": environment.platform,
        "board": environment.board,
        "chip": environment.chip,
        "sdk_target": environment.sdk_target,
        "role": environment.role,
        "flash_bytes": environment.flash_kb * 1024,
    }
    mismatches = [field for field, value in expected.items() if target.get(field) != value]
    if mismatches:
        raise DeviceError("package no longer matches the catalog fields: " + ", ".join(mismatches))
    if environment.platform == "esp":
        from newmota.host import load_sdk_version

        package_sdk = package.data.get("sdk")
        if not isinstance(package_sdk, dict):
            raise DeviceError("package has no ESP-IDF SDK identity")
        selected = next(
            (
                key
                for key in ("esp-idf", "esp-idf-v6.1")
                if load_sdk_version(key).revision == package_sdk.get("revision")
            ),
            None,
        )
        if selected is None:
            raise DeviceError("package ESP-IDF revision is not a supported pinned SDK")
        pin = load_sdk_version(selected)
        if package_sdk.get("version") != pin.version or package_sdk.get("name") != pin.name:
            raise DeviceError("package ESP-IDF identity does not match its pinned SDK")
        environment = replace(
            environment, idf_version="6.1" if selected == "esp-idf-v6.1" else "5.5"
        )
    return environment


def _factory_environment(factory: FactoryPackage) -> Environment:
    """Return the catalog environment a factory package names, for its chip family."""

    description = factory.description
    environment = resolve_environment(str(description["environment"]))
    target = description["target"]
    mismatches = [
        field
        for field, value in (("platform", environment.platform), ("chip", environment.chip))
        if target.get(field) != value
    ]
    if mismatches:
        raise DeviceError(
            "factory package does not match its catalog environment: " + ", ".join(mismatches)
        )
    return environment


def build_package(environment: Environment, jobs: int | None) -> FirmwarePackage:
    build_selected([environment], jobs)
    return load_package(package_manifest_path(environment.name, project_version()))


def existing_package(manifest: Path) -> tuple[Environment, FirmwarePackage]:
    package = load_package(manifest)
    return _package_environment(package), package


def probe(environment_name: str, requested_port: str | None, baud: int | None) -> None:
    environment = resolve_environment(environment_name)
    adapter = flasher_for(environment.platform)
    adapter.probe(environment, adapter.resolve_programming_port(requested_port), baud)


def backup(
    environment_name: str,
    requested_port: str | None,
    output: Path,
    baud: int | None,
) -> None:
    environment = resolve_environment(environment_name)
    resolved_output = output.expanduser().resolve()
    try:
        resolved_output.relative_to(FIRMWARE_DIR.resolve())
    except ValueError:
        pass
    else:
        raise DeviceError("full-chip backups must not be stored under build/firmware")
    adapter = flasher_for(environment.platform)
    adapter.backup(
        environment, adapter.resolve_programming_port(requested_port), resolved_output, baud
    )


def flash_built(
    environment_name: str,
    requested_port: str | None,
    baud: int | None,
    monitor_port: str | None,
    monitor_baud: int | None,
    jobs: int | None,
    start_monitor: bool,
) -> None:
    environment = resolve_environment(environment_name)
    if environment.role != "application":
        raise DeviceError("make flash requires an application environment, not safeboot")
    adapter = flasher_for(environment.platform)
    port = adapter.resolve_programming_port(requested_port)
    package = build_package(environment, jobs)
    _flash_and_maybe_monitor(
        environment,
        package,
        port,
        baud,
        monitor_port,
        monitor_baud,
        start_monitor,
    )


def update_built(
    environment_name: str,
    requested_port: str | None,
    baud: int | None,
    jobs: int | None,
) -> None:
    """Build and write only the normal application partition."""

    environment = resolve_environment(environment_name)
    if environment.role != "application":
        raise DeviceError("make update requires an application environment, not safeboot")
    adapter = flasher_for(environment.platform)
    port = adapter.resolve_programming_port(requested_port)
    update = build_application_update(environment, jobs)
    adapter.update(environment, update, port, baud)


def flash_existing(
    manifest: Path,
    requested_port: str | None,
    baud: int | None,
    monitor_port: str | None,
    monitor_baud: int | None,
    start_monitor: bool,
) -> None:
    if manifest.suffix == ".zip":
        factory = load_factory_package(manifest.expanduser().resolve())
        environment = _factory_environment(factory)
        adapter = flasher_for(environment.platform)
        port = adapter.resolve_programming_port(requested_port)
        adapter.flash_factory(environment, factory, port, baud)
        if start_monitor:
            adapter.monitor(
                environment,
                None,
                adapter.resolve_monitor_port(monitor_port, port),
                monitor_baud,
                after_flash=True,
            )
        return
    environment, package = existing_package(manifest)
    adapter = flasher_for(environment.platform)
    port = adapter.resolve_programming_port(requested_port)
    _flash_and_maybe_monitor(
        environment,
        package,
        port,
        baud,
        monitor_port,
        monitor_baud,
        start_monitor,
    )


def _flash_and_maybe_monitor(
    environment: Environment,
    package: FirmwarePackage,
    port: str,
    baud: int | None,
    requested_monitor_port: str | None,
    monitor_baud: int | None,
    start_monitor: bool,
) -> None:
    adapter = flasher_for(environment.platform)
    adapter.flash(environment, package, port, baud)
    if start_monitor:
        selected_monitor_port = adapter.resolve_monitor_port(requested_monitor_port, port)
        adapter.monitor(
            environment,
            package,
            selected_monitor_port,
            monitor_baud,
            after_flash=True,
        )


def monitor(
    environment_name: str,
    requested_port: str | None,
    baud: int | None,
) -> None:
    environment = resolve_environment(environment_name)
    adapter = flasher_for(environment.platform)
    port = adapter.resolve_monitor_port(requested_port, "")
    adapter.monitor(
        environment,
        None,
        port,
        baud,
        after_flash=False,
    )
