"""Bouffalo wired-device adapter using the pinned blflashcommand package."""

from __future__ import annotations

import binascii
import json
import struct
from importlib import metadata
from itertools import pairwise
from pathlib import Path

from newmota.device import DeviceAdapter, DeviceError, ProbeResult, common_monitor
from newmota.factory_package import BOUFFALO_PARTITION_TABLE, FactoryPackage
from newmota.host import load_sdk_version
from newmota.model import ROOT, WORK_DIR, ApplicationUpdate, Environment
from newmota.package import FirmwarePackage, PackageError
from newmota.platforms.bouffalo.images import BouffaloLayoutError, expected_partition_entries
from newmota.progress import WAIT_SECONDS, Progress, notice

PT_MAGIC = 0x54504642
PT_TABLE_SIZE = 0x1000
PT_TABLE0_ADDRESS = 0xE000
PT_TABLE1_ADDRESS = 0xF000
PT_ENTRY_SIZE = 36
PT_FILESYSTEM_NAME = "lfs"
# Offsets of address0, address1, size0, and size1 inside one 36-byte entry.
PT_ENTRY_ADDRESS0 = 12
PT_ENTRY_SIZE0 = 20
# Bouffalo flash commands use 3-byte addresses; LittleFS never grows past them.
MAX_FILESYSTEM_FLASH_BYTES = 16 * 1024 * 1024
DOWNLOAD_MODE_HINT = "hold BOOT, press and release RESET/EN, then release BOOT"


class BouffaloProgrammerError(DeviceError):
    """Raised when the Bouffalo programmer cannot run or reports a failure."""


def _package_chip(environment: Environment) -> str:
    """Translate SDK target names to the standalone flasher's public profiles."""

    chip = environment.sdk_target.lower()
    # The BouffaloSDK names this silicon bl618dg. The flasher intentionally
    # exposes the requested chip as BL618 while sharing BL616 loader assets.
    if chip == "bl618dg":
        chip = "bl618"
    if chip not in {"bl602", "bl616", "bl618", "bl702"}:
        raise BouffaloProgrammerError(
            f"blflashcommand has no profile for Bouffalo SDK target {environment.sdk_target}"
        )
    return chip


def _default_baud(chip: str) -> int:
    """Return Newmota's UART transfer default for the selected chip profile."""

    if chip in {"bl616", "bl618"}:
        # Keep transfers at the ROM handshake rate; faster links remain an
        # explicit BAUD override until larger transfers are qualified.
        return 500_000
    return 500_000 if chip == "bl602" else 2_000_000


def _jedec_capacity(jedec_id: str) -> int:
    """Decode the standard JEDEC capacity exponent from a three-byte ID."""

    try:
        raw = bytes.fromhex(jedec_id)
    except ValueError as error:
        raise BouffaloProgrammerError(
            f"invalid JEDEC ID returned by flasher: {jedec_id!r}"
        ) from error
    if len(raw) != 3:
        raise BouffaloProgrammerError(
            f"JEDEC ID has {len(raw)} bytes; expected 3 bytes, got {jedec_id!r}"
        )
    capacity_exponent = raw[2]
    if not 16 <= capacity_exponent <= 31:
        raise BouffaloProgrammerError(
            f"unsupported JEDEC capacity code 0x{capacity_exponent:02x} in {jedec_id}"
        )
    return 1 << capacity_exponent


def parse_partition_table(data: bytes) -> dict[str, object]:
    """Validate one 4 KiB partition-table copy and return its entries.

    Both the header CRC and the entries CRC use standard IEEE CRC-32, as
    verified against the SDK-generated partition.bin.
    """

    if len(data) < PT_TABLE_SIZE:
        raise BouffaloProgrammerError(
            f"partition table copy is {len(data)} bytes, expected {PT_TABLE_SIZE}"
        )
    magic, version, entry_count, age, header_crc = struct.unpack("<IHHII", data[:16])
    if magic != PT_MAGIC:
        raise BouffaloProgrammerError(
            f"partition table has invalid magic {magic:#x}, expected {PT_MAGIC:#x}"
        )
    if binascii.crc32(data[:12]) & 0xFFFFFFFF != header_crc:
        raise BouffaloProgrammerError("partition table header CRC mismatch")
    if entry_count == 0 or entry_count > 16:
        raise BouffaloProgrammerError(f"partition table has invalid entry count {entry_count}")
    entries_data = data[16 : 16 + PT_ENTRY_SIZE * entry_count]
    (entries_crc,) = struct.unpack(
        "<I", data[16 + PT_ENTRY_SIZE * entry_count : 20 + PT_ENTRY_SIZE * entry_count]
    )
    if binascii.crc32(entries_data) & 0xFFFFFFFF != entries_crc:
        raise BouffaloProgrammerError("partition table entries CRC mismatch")
    entries = []
    for index in range(entry_count):
        raw = entries_data[index * PT_ENTRY_SIZE : (index + 1) * PT_ENTRY_SIZE]
        kind, device, active, name, address0, address1, size0, size1, length, entry_age = (
            struct.unpack("<BBB9sIIIIII", raw)
        )
        entries.append(
            {
                "type": kind,
                "device": device,
                "active_index": active,
                "name": name.split(b"\x00")[0].decode("ascii", errors="replace"),
                "address0": address0,
                "address1": address1,
                "size0": size0,
                "size1": size1,
                "len": length,
                "age": entry_age,
            }
        )
    return {"version": version, "age": age, "entries": entries}


def grow_filesystem_table(
    data: bytes, filesystem_offset: int, declared_flash_bytes: int, flash_bytes: int
) -> bytes:
    """Return a table copy whose final LittleFS entry reaches the physical flash end.

    Only the `lfs` size changes, and only when it is the last region of the declared layout.
    The header (and its CRC) is untouched; the entries CRC is recomputed. Packaged copies
    hold only the header, entries, and CRC; the result keeps the input length.
    """

    def padded(copy: bytes) -> bytes:
        return copy + b"\xff" * max(0, PT_TABLE_SIZE - len(copy))

    table = parse_partition_table(padded(data))
    entries = table["entries"]
    assert isinstance(entries, list)
    matches = [index for index, entry in enumerate(entries) if entry["name"] == PT_FILESYSTEM_NAME]
    if len(matches) != 1:
        raise BouffaloProgrammerError("partition table has no single LittleFS entry")
    filesystem = entries[matches[0]]
    if (
        filesystem["address0"] != filesystem_offset
        or filesystem["address0"] + filesystem["size0"] != declared_flash_bytes
        or filesystem["address1"] != 0
        or filesystem["size1"] != 0
    ):
        raise BouffaloProgrammerError("LittleFS is not the final region of the declared layout")
    for entry in entries:
        if entry is filesystem:
            continue
        for slot in ("0", "1"):
            if entry[f"size{slot}"] and entry[f"address{slot}"] + entry[f"size{slot}"] > (
                filesystem_offset
            ):
                raise BouffaloProgrammerError(f"partition entry {entry['name']} follows LittleFS")
    end = min(flash_bytes, MAX_FILESYSTEM_FLASH_BYTES)
    if end <= declared_flash_bytes:
        return bytes(data)
    grown = bytearray(data)
    entry_offset = 16 + matches[0] * PT_ENTRY_SIZE
    struct.pack_into("<I", grown, entry_offset + PT_ENTRY_SIZE0, end - filesystem_offset)
    entries_end = 16 + PT_ENTRY_SIZE * len(entries)
    struct.pack_into("<I", grown, entries_end, binascii.crc32(grown[16:entries_end]) & 0xFFFFFFFF)
    parse_partition_table(padded(bytes(grown)))
    return bytes(grown)


def validate_installed_layout(
    table: dict[str, object], environment: Environment, flash_bytes: int | None = None
) -> dict[str, object]:
    """Require exact immutable partition identity for application-only writes.

    A LittleFS entry that the factory flasher grew to the end of the probed flash is accepted.
    """

    try:
        expected = expected_partition_entries(environment)
    except BouffaloLayoutError as error:
        raise BouffaloProgrammerError(str(error)) from error
    raw_entries = table.get("entries")
    if not isinstance(raw_entries, list):
        raise BouffaloProgrammerError("active partition table has no entries")
    actual = {
        entry.get("name"): entry
        for entry in raw_entries
        if isinstance(entry, dict) and isinstance(entry.get("name"), str)
    }
    if set(actual) != set(expected):
        raise BouffaloProgrammerError(
            "device partition table does not exactly match the Newmota Bouffalo layout"
        )
    declared_flash_bytes = environment.flash_kb * 1024
    for name, fields in expected.items():
        differing = {field for field, value in fields.items() if actual[name].get(field) != value}
        if name == PT_FILESYSTEM_NAME and differing == {"size0"}:
            size = actual[name].get("size0")
            grown_end = fields["address0"] + size if isinstance(size, int) else 0
            if (
                fields["address0"] + fields["size0"] == declared_flash_bytes
                and flash_bytes is not None
                and declared_flash_bytes < grown_end <= flash_bytes
            ):
                differing = set()
        if differing:
            raise BouffaloProgrammerError(f"device partition geometry differs for {name}")
    return {"entries": sorted(expected)}


def active_partition_table(
    table_data: bytes,
) -> tuple[int, dict[str, object], list[int | None], list[str | None]]:
    """Return the newest valid copy of the two 4 KiB tables, plus per-copy ages and errors."""

    tables: list[dict[str, object] | None] = []
    table_errors: list[str | None] = []
    for index in range(2):
        try:
            table = parse_partition_table(
                table_data[index * PT_TABLE_SIZE : (index + 1) * PT_TABLE_SIZE]
            )
        except BouffaloProgrammerError as error:
            tables.append(None)
            table_errors.append(str(error))
        else:
            tables.append(table)
            table_errors.append(None)
    valid = [(index, table) for index, table in enumerate(tables) if table is not None]
    if not valid:
        raise BouffaloProgrammerError("neither Bouffalo partition-table copy is valid")
    active, active_table = max(valid, key=lambda item: int(item[1]["age"]))
    ages = [int(table["age"]) if table is not None else None for table in tables]
    return active, active_table, ages, table_errors


class _SessionFeedback:
    """Turn blflashcommand events into wait notices and one progress bar per session."""

    def __init__(self, action: str, images: list[tuple[str, int, int]]) -> None:
        self.action = action
        self.images = sorted(images, key=lambda image: image[1])
        total = sum(size for _, _, size in self.images)
        self.write = Progress(f"Writing ({action})", total) if total else None
        self.read: Progress | None = None

    def _image_progress(self, event: dict[str, object]) -> None:
        if self.write is None:
            return
        before = 0
        for name, offset, size in self.images:
            if offset == event.get("address"):
                done, total = event.get("done", 0), event.get("total", 0)
                if event["event"] == "written":
                    fraction = 1.0
                elif isinstance(done, int) and isinstance(total, int) and total > 0:
                    fraction = done / total
                else:
                    fraction = 0.0
                self.write.update(before + int(size * fraction), f"Writing {name}")
                return
            before += size

    def __call__(self, event: dict[str, object]) -> None:
        kind = event.get("event")
        if kind == "connecting":
            notice(f"Bouffalo {self.action}: connecting to the boot ROM ...")
        elif kind == "waiting":
            notice(
                f"Bouffalo {self.action}: no answer. Enter download mode now: "
                f"{DOWNLOAD_MODE_HINT} ({event.get('remaining')} s left)"
            )
        elif kind == "connected":
            notice(f"Bouffalo {self.action}: boot ROM connected")
        elif kind in {"write", "written"}:
            self._image_progress(event)
        elif kind in {"read", "verify"}:
            done, total = event.get("done", 0), event.get("total", 0)
            if isinstance(done, int) and isinstance(total, int) and total > 0:
                label = "Reading flash" if kind == "read" else "Verifying"
                if self.read is None or self.read.total != total or self.read.label != label:
                    if self.read is not None:
                        self.read.end_line()
                    self.read = Progress(label, total)
                self.read.update(done)

    def close(self) -> None:
        for progress in (self.write, self.read):
            if progress is not None:
                progress.end_line()


def _save_log(path: Path, log: str) -> None:
    """Keep the full loader log out of the terminal; print where it went."""

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(log, encoding="utf-8")
    print(f"Bouffalo loader log: {path.relative_to(ROOT)}", flush=True)


class BouffaloFlasher(DeviceAdapter):
    """Flash Bouffalo image sets through the importable UART API."""

    platform_name = "Bouffalo"
    default_monitor_baud = 2_000_000

    @staticmethod
    def _api():
        """Load the pinned standalone package only when a device action runs."""

        sdk = load_sdk_version("bouffalo")
        if sdk.programmer != "blflashcommand" or sdk.programmer_status == "unselected":
            raise BouffaloProgrammerError(
                f"Bouffalo flasher is not configured: {sdk.programmer} {sdk.programmer_version}"
            )
        try:
            installed = metadata.version("blflashcommand")
        except metadata.PackageNotFoundError as error:
            raise BouffaloProgrammerError(
                "blflashcommand is not installed; run make uv-setup"
            ) from error
        if installed != sdk.programmer_version:
            raise BouffaloProgrammerError(
                f"blflashcommand version mismatch: expected {sdk.programmer_version}, "
                f"got {installed}; run make uv-setup"
            )
        try:
            import blflashcommand
        except ImportError as error:
            raise BouffaloProgrammerError(
                f"cannot import blflashcommand {installed}; run make uv-setup"
            ) from error
        return blflashcommand

    @staticmethod
    def _invoke(
        action: str, operation, *args, images: list[tuple[str, int, int]] | None = None, **kwargs
    ):
        """Apply UART defaults, show progress and waits, and translate package errors.

        images lists (name, offset, size) of what the session writes, for its progress bar.
        """

        chip = kwargs.get("chip")
        if chip in {"bl616", "bl618"} and kwargs.get("baudrate") is None:
            kwargs["baudrate"] = _default_baud(chip)
            print(f"Bouffalo {action}: {chip} profile, {kwargs['baudrate']} baud", flush=True)
        feedback = _SessionFeedback(action, images or [])
        kwargs.update(on_event=feedback, wait_seconds=WAIT_SECONDS)
        try:
            return operation(*args, **kwargs)
        except Exception as error:
            if isinstance(error, BouffaloProgrammerError):
                raise
            detail = f"Bouffalo {action} failed: {error}"
            log = getattr(error, "log", "")
            if log:
                detail += "\n" + str(log).rstrip()
            raise BouffaloProgrammerError(detail) from error
        finally:
            feedback.close()

    @staticmethod
    def _read_flash(
        environment: Environment, port: str, baud: int | None, start: int, length: int
    ) -> bytes:
        api = BouffaloFlasher._api()
        chip = _package_chip(environment)
        result = BouffaloFlasher._invoke(
            "flash read",
            api.read_flash_uart,
            start,
            length,
            chip=chip,
            port=port,
            baudrate=baud,
        )
        if len(result.data) != length:
            raise BouffaloProgrammerError(
                f"Bouffalo flash read returned {len(result.data)} bytes, expected {length}"
            )
        return result.data

    def _probe_target(
        self,
        environment: Environment,
        port: str,
        baud: int | None,
    ) -> ProbeResult:
        """Read JEDEC capacity and validate both native tables' CRC envelopes."""

        api = self._api()
        chip = _package_chip(environment)
        inspected = self._invoke(
            "probe",
            api.read_flash_uart,
            PT_TABLE0_ADDRESS,
            PT_TABLE_SIZE * 2,
            chip=chip,
            port=port,
            baudrate=baud,
            include_probe=True,
        )
        hardware = inspected.probe
        if hardware is None:
            raise BouffaloProgrammerError("Bouffalo partition read returned no probe evidence")
        table_data = inspected.data
        if len(table_data) != PT_TABLE_SIZE * 2:
            raise BouffaloProgrammerError(
                f"Bouffalo partition read returned {len(table_data)} bytes, "
                f"expected {PT_TABLE_SIZE * 2}"
            )
        flash_bytes = _jedec_capacity(hardware.jedec_id)
        declared_flash_bytes = environment.flash_kb * 1024
        if flash_bytes < declared_flash_bytes:
            raise BouffaloProgrammerError(
                f"JEDEC reports {flash_bytes} bytes of flash, but "
                f"{environment.name} requires at least {declared_flash_bytes} bytes"
            )

        active, active_table, ages, table_errors = active_partition_table(table_data)
        firmware = next(
            (
                entry
                for entry in active_table["entries"]  # type: ignore[union-attr]
                if entry["name"] == "FW"
            ),
            None,
        )
        if firmware is None:
            raise BouffaloProgrammerError("active partition table has no FW entry")
        work_dir = WORK_DIR / environment.name / "flash" / f"{flash_bytes}"
        work_dir.mkdir(parents=True, exist_ok=True)
        probe_path = work_dir / "probe.json"
        probe_path.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "platform": environment.platform,
                    "chip": environment.chip,
                    "sdk_target": environment.sdk_target,
                    "flasher_chip_profile": chip,
                    "port": port,
                    "flash_bytes": flash_bytes,
                    "capacity_source": "jedec-capacity-byte",
                    "backend": hardware.backend,
                    "boot_info": hardware.boot_info,
                    "mac_address": hardware.mac_address,
                    "jedec_id": hardware.jedec_id,
                    "jedec_response": hardware.jedec_response,
                    "active_table": active,
                    "table_ages": ages,
                    "table_errors": table_errors,
                    "firmware_active_index": firmware["active_index"],
                    "warning": "Newmota Bouffalo firmware behavior remains hardware-unqualified",
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"Bouffalo probe: {probe_path.relative_to(ROOT)}")
        print(f"Bouffalo JEDEC flash: {hardware.jedec_id.upper()} ({flash_bytes // 1024} KiB)")
        return ProbeResult(
            platform=environment.platform,
            chip=environment.chip,
            port=port,
            flash_bytes=flash_bytes,
            identifier=f"JEDEC {hardware.jedec_id.upper()}",
            security={"secure_boot": None, "flash_encryption": None},
            work_dir=work_dir,
        )

    def probe(self, environment: Environment, port: str, baud: int | None) -> ProbeResult:
        """Read redundant tables and validate their native CRC envelope."""

        return self._probe_target(environment, port, baud)

    def backup(
        self,
        environment: Environment,
        port: str,
        output: Path,
        baud: int | None,
    ) -> None:
        """Read the complete physical flash without overwriting an existing file."""

        if output.exists():
            raise BouffaloProgrammerError(f"backup output already exists: {output}")
        chip = _package_chip(environment)
        api = self._api()
        declared_flash_bytes = environment.flash_kb * 1024
        backup = self._invoke(
            "backup",
            api.read_flash_uart,
            0,
            declared_flash_bytes,
            chip=chip,
            port=port,
            baudrate=baud,
            include_probe=True,
        )
        hardware = backup.probe
        if hardware is None:
            raise BouffaloProgrammerError("Bouffalo backup returned no probe evidence")
        flash_bytes = _jedec_capacity(hardware.jedec_id)
        if flash_bytes != declared_flash_bytes:
            raise BouffaloProgrammerError(
                f"JEDEC reports {flash_bytes} bytes of flash, but "
                f"{environment.name} requires {declared_flash_bytes} bytes"
            )
        data = backup.data
        if len(data) != flash_bytes:
            raise BouffaloProgrammerError(f"backup is {len(data)} bytes, expected {flash_bytes}")
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
        sidecar = output.with_suffix(output.suffix + ".json")
        import hashlib

        sidecar.write_text(
            json.dumps(
                {
                    "environment": environment.name,
                    "port": port,
                    "flash_bytes": flash_bytes,
                    "jedec_id": hardware.jedec_id,
                    "jedec_response": hardware.jedec_response,
                    "sha256": hashlib.sha256(data).hexdigest(),
                    "warning": "a complete backup contains credentials and user data",
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"Bouffalo backup: {output} ({len(data)} bytes)")

    @staticmethod
    def _images(package: FirmwarePackage) -> list[tuple[str, int, Path]]:
        flash = package.data.get("flash")
        if not isinstance(flash, dict) or flash.get("protocol") != "bouffalo-uart":
            raise PackageError("package has no Bouffalo UART flash plan")
        raw_components = flash.get("components")
        if not isinstance(raw_components, list) or not raw_components:
            raise PackageError("Bouffalo flash plan has no components")
        layout = package.data.get("layout")
        if not isinstance(layout, dict) or layout.get("flash_bytes") != package.flash_bytes:
            raise PackageError("Bouffalo package has no valid flash layout metadata")

        def region(name: str) -> tuple[int, int]:
            regions = layout.get("regions")
            if not isinstance(regions, list):
                raise PackageError("Bouffalo package layout has no regions")
            matches = [
                item for item in regions if isinstance(item, dict) and item.get("name") == name
            ]
            if len(matches) != 1:
                raise PackageError(f"Bouffalo package layout is missing region {name}")
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
                raise PackageError(f"Bouffalo package layout has an invalid {name} region")
            return offset, size

        application_offset, application_size = region("application")
        safeboot_offset, safeboot_size = region("safeboot")
        filesystem_offset, filesystem_size = region("littlefs")
        expected = {
            "boot2.bin": (0x0000, package.flash_bytes),
            "partition-table0.bin": (PT_TABLE0_ADDRESS, PT_TABLE_SIZE),
            "partition-table1.bin": (PT_TABLE1_ADDRESS, PT_TABLE_SIZE),
            "safeboot.bin": (safeboot_offset, safeboot_size),
            "application.bin": (application_offset, application_size),
            "littlefs.bin": (filesystem_offset, filesystem_size),
        }
        parsed: dict[str, tuple[int, Path]] = {}
        for index, raw in enumerate(raw_components):
            if not isinstance(raw, dict):
                raise PackageError(f"Bouffalo flash component {index} is invalid")
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
                raise PackageError(f"Bouffalo flash component {index} is incomplete")
            if name not in expected:
                raise PackageError(f"Bouffalo flash plan contains an unexpected image: {name}")
            if role not in {"factory", "factory-component"}:
                raise PackageError(f"Bouffalo image is not a factory artifact: {name}")
            if offset != expected[name][0] or name in parsed:
                raise PackageError(f"Bouffalo image has an invalid or duplicate offset: {name}")
            path = package.file(path_name)
            size = path.stat().st_size
            if not size or offset + size > package.flash_bytes:
                raise BouffaloProgrammerError(
                    f"Bouffalo image exceeds the package flash capacity: {name}"
                )
            if offset + size > expected[name][0] + expected[name][1]:
                raise BouffaloProgrammerError(f"Bouffalo image exceeds its region: {name}")
            parsed[name] = (offset, path)

        if set(parsed) != set(expected):
            missing = sorted(set(expected) - set(parsed))
            raise PackageError("Bouffalo flash plan is missing: " + ", ".join(missing))
        ranges = sorted(
            (offset, offset + path.stat().st_size, name) for name, (offset, path) in parsed.items()
        )
        if any(left[1] > right[0] for left, right in pairwise(ranges)):
            raise PackageError("Bouffalo factory flash components overlap")
        return [(name, parsed[name][0], parsed[name][1]) for name in expected]

    @staticmethod
    def _validate_target(environment: Environment, package: FirmwarePackage) -> None:
        target = package.data.get("target")
        if not isinstance(target, dict):
            raise PackageError("Bouffalo package has no target metadata")
        if (
            target.get("environment") != environment.name
            or target.get("chip") != environment.chip
            or target.get("sdk_target") != environment.sdk_target
        ):
            raise BouffaloProgrammerError(
                "Bouffalo package does not match the selected catalog environment"
            )

    def flash(
        self,
        environment: Environment,
        package: FirmwarePackage,
        port: str,
        baud: int | None,
    ) -> None:
        """Plan, write with loader verification, and reset a Bouffalo target."""

        self._validate_target(environment, package)
        images = self._images(package)
        flash_bytes = package.flash_bytes
        declared_flash_bytes = environment.flash_kb * 1024
        if flash_bytes != declared_flash_bytes:
            raise BouffaloProgrammerError(
                f"package declares {flash_bytes} bytes, but "
                f"{environment.name} requires {declared_flash_bytes} bytes"
            )
        chip = _package_chip(environment)
        api = self._api()
        work_dir = WORK_DIR / environment.name / "flash" / f"{flash_bytes}"
        work_dir.mkdir(parents=True, exist_ok=True)
        # LittleFS ends the layout. Both table copies get one variant per larger flash size;
        # the write session selects by the JEDEC capacity it reads, so no extra session runs.
        filesystem_offset = next(offset for name, offset, _ in images if name == "littlefs.bin")
        capacities: list[int] = []
        capacity = declared_flash_bytes * 2
        while capacity <= MAX_FILESYSTEM_FLASH_BYTES:
            capacities.append(capacity)
            capacity *= 2
        variants: dict[str, dict[int, Path]] = {}
        for name, _, path in images:
            if name not in {"partition-table0.bin", "partition-table1.bin"}:
                continue
            variants[name] = {}
            for capacity in capacities:
                grown = work_dir / f"{Path(name).stem}-{capacity // (1024 * 1024)}m.bin"
                grown.write_bytes(
                    grow_filesystem_table(
                        path.read_bytes(), filesystem_offset, declared_flash_bytes, capacity
                    )
                )
                variants[name][capacity] = grown
        plan_path = work_dir / "flash-plan.json"
        effective_baud = baud if baud is not None else _default_baud(chip)
        plan_path.write_text(
            json.dumps(
                {
                    "environment": environment.name,
                    "protocol": "bouffalo-uart",
                    "chip_profile": chip,
                    "port": port,
                    "baud": effective_baud,
                    "minimum_flash_bytes": declared_flash_bytes,
                    "filesystem_growth_capacities": capacities,
                    "components": [
                        {"name": name, "offset": offset, "size": path.stat().st_size}
                        for name, offset, path in images
                    ],
                    "preserves": ["unwritten-layout-regions"],
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"Bouffalo flash plan: {plan_path.relative_to(ROOT)}")
        # Check JEDEC before any erase/write in the worker's active loader
        # session, keeping the same UART open through loader verification and reset.
        result = self._invoke(
            "flash",
            api.flash_uart,
            [api.FlashImage(path, offset, variants.get(name, {})) for name, offset, path in images],
            images=[(name, offset, path.stat().st_size) for name, offset, path in images],
            chip=chip,
            port=port,
            baudrate=baud,
            erase=True,
            verify=False,
            reset_after=True,
            minimum_flash_bytes=declared_flash_bytes,
        )
        _save_log(work_dir / "flash.log", result.log)
        print("Bouffalo flash completed with loader verification; reset acknowledged.")

    def flash_factory(
        self,
        environment: Environment,
        factory: FactoryPackage,
        port: str,
        baud: int | None,
    ) -> None:
        """Write a factory package from its description alone, as the web flasher does."""

        steps = factory.steps((BOUFFALO_PARTITION_TABLE,), "Bouffalo")
        if len(steps) != 1:
            raise PackageError("Bouffalo factory package has no partition-table step")
        table = steps[0]
        flash_bytes = factory.description["target"]["flash_bytes"]
        chip = _package_chip(environment)
        api = self._api()
        work_dir = WORK_DIR / environment.name / "flash" / f"{flash_bytes}" / "factory-package"
        images = factory.write_images(work_dir)
        # As in flash(): one table variant per larger flash size, chosen in the write session
        # by the JEDEC capacity, so no extra session runs.
        capacities: list[int] = []
        capacity = flash_bytes * 2
        while capacity <= MAX_FILESYSTEM_FLASH_BYTES:
            capacities.append(capacity)
            capacity *= 2
        variants: dict[int, dict[int, Path]] = {}
        for name, offset, path in images:
            if offset not in table["tables"]:
                continue
            variants[offset] = {}
            for capacity in capacities:
                grown = work_dir / f"{Path(name).stem}-{capacity // (1024 * 1024)}m.bin"
                grown.write_bytes(
                    grow_filesystem_table(
                        path.read_bytes(), table["filesystem_offset"], flash_bytes, capacity
                    )
                )
                variants[offset][capacity] = grown
        plan_path = work_dir / "flash-plan.json"
        plan_path.write_text(
            json.dumps(
                {
                    "environment": environment.name,
                    "package": str(factory.path),
                    "protocol": "bouffalo-uart",
                    "chip_profile": chip,
                    "port": port,
                    "baud": baud if baud is not None else _default_baud(chip),
                    "minimum_flash_bytes": flash_bytes,
                    "filesystem_growth_capacities": capacities,
                    "components": [
                        {"name": name, "offset": offset, "size": path.stat().st_size}
                        for name, offset, path in images
                    ],
                    "preserves": ["unwritten-layout-regions"],
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"Bouffalo flash plan: {plan_path.relative_to(ROOT)}")
        result = self._invoke(
            "flash",
            api.flash_uart,
            [api.FlashImage(path, offset, variants.get(offset, {})) for _, offset, path in images],
            images=[(name, offset, path.stat().st_size) for name, offset, path in images],
            chip=chip,
            port=port,
            baudrate=baud,
            erase=True,
            verify=False,
            reset_after=True,
            minimum_flash_bytes=flash_bytes,
        )
        _save_log(work_dir / "flash.log", result.log)
        print("Bouffalo flash completed with loader verification; reset acknowledged.")

    def update(
        self,
        environment: Environment,
        update: ApplicationUpdate,
        port: str,
        baud: int | None,
    ) -> None:
        """Write only the normal application FW slot with loader verification."""

        if environment.role != "application":
            raise BouffaloProgrammerError(
                "Bouffalo application update requires an application environment"
            )
        try:
            firmware = expected_partition_entries(environment)["FW"]
        except (BouffaloLayoutError, KeyError) as error:
            raise BouffaloProgrammerError(
                f"invalid Bouffalo application layout: {error}"
            ) from error
        if update.offset != firmware["address0"] or update.size_limit != firmware["size0"]:
            raise BouffaloProgrammerError("Bouffalo application update does not match FW slot 0")
        if update.offset + update.size_limit > environment.flash_kb * 1024:
            raise BouffaloProgrammerError(
                "Bouffalo application update partition exceeds declared flash"
            )
        if not update.image.is_file():
            raise BouffaloProgrammerError(
                f"Bouffalo application update image is missing: {update.image}"
            )
        image_size = update.image.stat().st_size
        if image_size == 0 or image_size > update.size_limit:
            raise BouffaloProgrammerError("Bouffalo application update image does not fit its slot")
        plan_path = WORK_DIR / environment.name / "update-plan.json"
        plan_path.parent.mkdir(parents=True, exist_ok=True)
        plan_path.write_text(
            json.dumps(
                {
                    "environment": environment.name,
                    "protocol": "bouffalo-uart",
                    "chip_profile": _package_chip(environment),
                    "port": port,
                    "baud": (
                        baud if baud is not None else _default_baud(_package_chip(environment))
                    ),
                    "minimum_flash_bytes": environment.flash_kb * 1024,
                    "partition": "application",
                    "offset": update.offset,
                    "size": image_size,
                    "size_limit": update.size_limit,
                    "image": str(update.image.relative_to(ROOT)),
                    "preserves": [
                        "boot2",
                        "partition-tables",
                        "safeboot",
                        "littlefs",
                        "unwritten-layout-regions",
                    ],
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"Bouffalo update plan: {plan_path.relative_to(ROOT)}")
        chip = _package_chip(environment)
        api = self._api()

        def approve(hardware: object, ranges: dict[int, bytes]) -> None:
            """Check JEDEC and the installed layout in the write session, before any erase."""

            flash_bytes = _jedec_capacity(str(getattr(hardware, "jedec_id", "")))
            if update.offset + update.size_limit > flash_bytes:
                raise BouffaloProgrammerError(
                    "Bouffalo application update partition exceeds probed flash"
                )
            _, table, _, _ = active_partition_table(ranges[PT_TABLE0_ADDRESS])
            validate_installed_layout(table, environment, flash_bytes)
            print(
                f"Bouffalo layout check: JEDEC {flash_bytes // 1024} KiB, "
                "installed partition table matches",
                flush=True,
            )

        # One loader session: layout inspection, then the write, with no reconnect between.
        result = self._invoke(
            "application update",
            api.flash_uart,
            [api.FlashImage(update.image, update.offset)],
            images=[("application", update.offset, image_size)],
            chip=chip,
            port=port,
            baudrate=baud,
            erase=True,
            verify=False,
            reset_after=True,
            minimum_flash_bytes=environment.flash_kb * 1024,
            inspect=[(PT_TABLE0_ADDRESS, PT_TABLE_SIZE * 2)],
            approve=approve,
        )
        _save_log(plan_path.parent / "update.log", result.log)
        print("Bouffalo update completed with loader verification; reset acknowledged.")

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
