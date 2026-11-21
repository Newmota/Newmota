"""Beken BK7231 wired flashing through the pinned bk7231tools protocol."""

from __future__ import annotations

import io
import json
import os
import time
from collections.abc import Iterator
from contextlib import contextmanager
from importlib.metadata import PackageNotFoundError
from importlib.metadata import version as package_version
from pathlib import Path
from typing import Any

from newmota.device import DeviceAdapter, DeviceError, ProbeResult, common_monitor
from newmota.factory_package import BEKEN_NET_RESET, BEKEN_RF_CHECK, FactoryPackage
from newmota.host import check_sdk_version, load_sdk_version, sha256
from newmota.model import ROOT, WORK_DIR, ApplicationUpdate, Environment
from newmota.package import FirmwarePackage, PackageError
from newmota.platforms.beken.codec import (
    BekenImageError,
    load_image_config,
    validate_application_image,
    validate_image_layout,
    validate_ota_image,
)
from newmota.platforms.beken.images import resolve_layout
from newmota.platforms.beken.net_params import (
    NET_RESET_POLICY,
    NET_SECTOR_SIZE,
    factory_net_sector,
)
from newmota.progress import WAIT_SECONDS, Progress, notice

DEFAULT_BAUD = 1_500_000
# Each link attempt listens this long before the reset instruction repeats.
LINK_SLICE_SECONDS = 3.0
RESET_HINT = "pull CEN to GND briefly or power-cycle the board"
DEFAULT_MONITOR_BAUD = 115200
PROGRAMMER_PACKAGE = "bk7231tools"


def _display(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def _normalized_chip(value: object) -> str:
    return "".join(character for character in str(value).lower() if character.isalnum())


def _serial_class() -> type[Any]:
    sdk = load_sdk_version("beken")
    if sdk.programmer != PROGRAMMER_PACKAGE:
        raise DeviceError(
            f"Beken SDK pin selects {sdk.programmer!r}, expected {PROGRAMMER_PACKAGE!r}"
        )
    try:
        installed = package_version(PROGRAMMER_PACKAGE)
    except PackageNotFoundError as error:
        raise DeviceError(
            f"{PROGRAMMER_PACKAGE} {sdk.programmer_version} is not installed; run make uv-setup"
        ) from error
    if installed != sdk.programmer_version:
        raise DeviceError(
            f"{PROGRAMMER_PACKAGE} version mismatch: expected {sdk.programmer_version}, "
            f"got {installed}"
        )
    try:
        from bk7231tools.serial import BK7231Serial
    except ImportError as error:
        raise DeviceError(
            f"cannot import {PROGRAMMER_PACKAGE} {sdk.programmer_version}; run make uv-setup"
        ) from error
    return BK7231Serial


def _log_info(*values: object) -> None:
    print("bk7231tools:", *values, flush=True)


def _log_warning(*values: object) -> None:
    print("bk7231tools warning:", *values, flush=True)


def _program(
    device: Any,
    path: Path | None,
    data: bytes | None,
    start: int,
    progress: Progress,
    label: str,
    before: int,
) -> int:
    """Program one region from a file or bytes, advancing the shared bar; return bytes done."""

    size = path.stat().st_size if path is not None else len(data or b"")
    done = 0
    progress.update(before, label)
    with path.open("rb") if path is not None else io.BytesIO(data or b"") as stream:
        for written in device.program_flash(io=stream, io_size=size, start=start, crc_check=True):
            done += written
            progress.update(before + min(done, size), label)
    return before + size


@contextmanager
def _connected(port: str, baud: int | None) -> Iterator[Any]:
    serial_class = _serial_class()
    device = serial_class(port, baudrate=baud or DEFAULT_BAUD)
    device.info = _log_info
    device.warn = _log_warning
    try:
        notice("Beken: connecting to the bootloader ...")
        deadline = time.monotonic() + WAIT_SECONDS
        device.hw_reset()
        # The bootloader answers only shortly after a reset; keep asking while the user acts.
        while not device.wait_for_link(LINK_SLICE_SECONDS):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise DeviceError(
                    f"Beken bootloader did not answer within {WAIT_SECONDS} s; {RESET_HINT}, "
                    "then retry"
                )
            notice(
                f"Beken: no answer. Reset the chip now: {RESET_HINT} ({round(remaining)} s left)"
            )
        device.connect()
        notice("Beken: bootloader connected")
    except Exception as error:
        device.close()
        if isinstance(error, DeviceError):
            raise
        raise DeviceError(f"Beken connection failed: {error}") from error
    try:
        yield device
    finally:
        device.close()


def _validate_chip(device: Any, environment: Environment) -> None:
    actual = getattr(getattr(device, "chip_type", None), "name", None)
    expected = _normalized_chip(environment.sdk_target)
    if not actual:
        raise DeviceError("bk7231tools did not identify the Beken chip")
    if _normalized_chip(actual) != expected:
        raise DeviceError(
            f"connected chip {actual} does not match environment {environment.sdk_target}"
        )


def _identifier(device: Any) -> str | None:
    flash_id = getattr(device, "flash_id", None)
    if isinstance(flash_id, bytes):
        return flash_id.hex(" ", -1)
    return None


def _protocol_name(device: Any) -> str | None:
    protocol = getattr(device, "protocol_type", None)
    return getattr(protocol, "name", None)


def _probe_directory(environment: Environment, flash_bytes: int) -> Path:
    path = WORK_DIR / environment.name / "flash" / f"{flash_bytes:x}"
    path.mkdir(parents=True, exist_ok=True)
    return path


def _layout_for(environment: Environment):
    sdk = check_sdk_version("beken")
    return resolve_layout(environment, sdk, WORK_DIR / environment.name / "flash-layout")


# The BDK's RF calibration record starts with its TLV header, 0x00564C54 ("TLV\0").
RF_CALIBRATION_HEADER = (0x00564C54).to_bytes(4, "little")
# Factory RF calibration on known 2 MiB layouts: Tuya BK7231N, Beken SDK, Tuya T1 (BK7238).
KNOWN_CALIBRATION_OFFSETS = (0x1D0000, 0x1E0000, 0x1E3000)


def _check_flash_layout(device: Any, layout: Any, written: tuple[str, ...]) -> None:
    """Refuse a write unless the device keeps its RF calibration where the layout says.

    The calibration is factory data and Beken layouts differ by bootloader, so this is the
    one fact that tells a Tuya module from a Beken SDK one before anything is written.
    """

    regions = [layout.region(name) for name in written]
    _check_rf_calibration(
        device, layout.region("rf").offset, [(region.offset, region.end) for region in regions]
    )


def _check_rf_calibration(device: Any, rf_offset: int, written: list[tuple[int, int]]) -> None:
    """Require RF calibration at @p rf_offset and none at a known offset inside @p written."""

    def header(offset: int) -> bytes:
        return device.flash_read_bytes(offset, 0x1000, crc_check=True)[:4]

    if header(rf_offset) != RF_CALIBRATION_HEADER:
        raise DeviceError(
            f"no RF calibration at {rf_offset:#x}, where this layout keeps it; the module uses "
            "another flash layout (choose its board) or has lost its calibration. Nothing was "
            "written"
        )
    for offset in KNOWN_CALIBRATION_OFFSETS:
        if offset == rf_offset or not any(start <= offset < end for start, end in written):
            continue
        if header(offset) == RF_CALIBRATION_HEADER:
            raise DeviceError(
                f"the module has RF calibration at {offset:#x}, which this layout would "
                "overwrite; it uses another flash layout. Nothing was written"
            )


def _layout_identity(package: FirmwarePackage) -> str:
    layout = package.data.get("layout")
    if not isinstance(layout, dict) or not isinstance(layout.get("identity"), str):
        raise PackageError("Beken package has no layout identity")
    return str(layout["identity"])


def _package_components(
    environment: Environment, package: FirmwarePackage
) -> tuple[tuple[str, Path], tuple[str, Path], Any]:
    target = package.data.get("target")
    if not isinstance(target, dict):
        raise PackageError("Beken package has no target metadata")
    expected = {
        "environment": environment.name,
        "board": environment.board,
        "chip": environment.chip,
        "sdk_target": environment.sdk_target,
        "role": "application",
        "platform": "beken",
        "flash_bytes": environment.flash_kb * 1024,
    }
    mismatches = [field for field, value in expected.items() if target.get(field) != value]
    if mismatches:
        raise DeviceError(
            "Beken package does not match the selected catalog: " + ", ".join(mismatches)
        )

    sdk = package.data.get("sdk")
    pinned = load_sdk_version("beken")
    if not isinstance(sdk, dict) or sdk.get("programmer_version") != pinned.programmer_version:
        raise DeviceError("Beken package programmer version does not match the project pin")
    flash = package.data.get("flash")
    if not isinstance(flash, dict) or flash.get("protocol") != "bk7231tools-python":
        raise PackageError("Beken package has no bk7231tools flash plan")
    preserves = flash.get("preserves")
    if not isinstance(preserves, list) or not {"bootloader", "rf", "ble-bonding"}.issubset(
        preserves
    ):
        raise PackageError("Beken flash plan does not preserve native boot and metadata regions")
    if "net" in preserves:
        raise PackageError("Beken factory package preserves old NET settings; rebuild the package")
    raw_components = flash.get("components")
    if not isinstance(raw_components, list):
        raise PackageError("Beken package has no flash components")

    layout = _layout_for(environment)
    net = layout.region("net")
    if flash.get("net_initialization") != {
        "policy": NET_RESET_POLICY,
        "partition": "net",
        "offset": net.offset,
        "size": net.size,
    }:
        raise PackageError("Beken factory package requires the current NET initialization policy")
    if (
        net.size != NET_SECTOR_SIZE
        or net.offset % NET_SECTOR_SIZE
        or net.end > layout.flash_bytes
        or any(
            net.offset < region.end and region.offset < net.end
            for region in layout.regions
            if region.name != "net"
        )
    ):
        raise PackageError("Beken NET initialization must be one isolated, aligned sector")
    try:
        image_config = load_image_config(environment)
        validate_image_layout(image_config, layout)
    except BekenImageError as error:
        raise PackageError(str(error)) from error
    configuration = package.data.get("configuration")
    image_metadata = configuration.get("beken_image") if isinstance(configuration, dict) else None
    if (
        not isinstance(image_metadata, dict)
        or image_metadata.get("identity") != image_config.identity
    ):
        raise DeviceError(
            "Beken package image-format policy does not match the current layout descriptor"
        )
    if _layout_identity(package) != layout.identity:
        raise DeviceError(
            "Beken package layout does not match the current SDK partition source or descriptor"
        )
    components: dict[str, tuple[str, Path]] = {}
    for index, raw in enumerate(raw_components):
        if not isinstance(raw, dict):
            raise PackageError(f"Beken flash component {index} is invalid")
        partition = raw.get("partition")
        path_name = raw.get("path")
        offset = raw.get("offset")
        if (
            not isinstance(partition, str)
            or not isinstance(path_name, str)
            or not isinstance(offset, int)
            or isinstance(offset, bool)
        ):
            raise PackageError(f"Beken flash component {index} is incomplete")
        if partition not in {"application", "filesystem"} or partition in components:
            raise PackageError(f"Beken flash component has an unexpected partition: {partition}")
        region = layout.region(partition)
        if offset != region.offset:
            raise PackageError(f"Beken {partition} component does not match the resolved offset")
        path = package.file(path_name)
        size = path.stat().st_size
        limit = region.image_budget or region.size
        if size <= 0 or size > limit:
            raise PackageError(f"Beken {partition} image does not fit its native region")
        if partition == "filesystem" and size != region.size:
            raise PackageError("Beken filesystem image must cover the entire filesystem region")
        components[partition] = (path_name, path)
    if set(components) != {"application", "filesystem"}:
        missing = sorted({"application", "filesystem"} - set(components))
        raise PackageError("Beken flash plan is missing: " + ", ".join(missing))
    try:
        validate_application_image(
            components["application"][1],
            image_config,
            layout.region("application").size,
        )
    except BekenImageError as error:
        raise PackageError(str(error)) from error
    ota_description = package.data.get("ota")
    ota_artifacts = ota_description.get("artifacts") if isinstance(ota_description, dict) else None
    if not isinstance(ota_artifacts, list) or len(ota_artifacts) != 1:
        raise PackageError("Beken package must contain exactly one OTA artifact")
    ota_artifact = ota_artifacts[0]
    if not isinstance(ota_artifact, dict) or ota_artifact.get("role") != "application-update":
        raise PackageError("Beken package OTA artifact is invalid")
    ota_path_name = ota_artifact.get("path")
    ota_region = layout.region("ota")
    if (
        not isinstance(ota_path_name, str)
        or ota_artifact.get("format") != "rbl"
        or ota_artifact.get("offset") != ota_region.offset
        or ota_artifact.get("size_limit") != (ota_region.image_budget or ota_region.size)
    ):
        raise PackageError("Beken package OTA artifact does not match the OTA region")
    ota_path = package.file(ota_path_name)
    if ota_artifact.get("size") != ota_path.stat().st_size:
        raise PackageError("Beken package OTA artifact size does not match its file")
    try:
        validate_ota_image(ota_path, image_config, ota_region.size, layout)
    except BekenImageError as error:
        raise PackageError(str(error)) from error
    return components["application"], components["filesystem"], layout


def _write_plan(
    environment: Environment,
    package_or_update: FirmwarePackage | FactoryPackage | ApplicationUpdate,
    probe: ProbeResult,
    components: list[dict[str, object]],
    operation: str,
) -> Path:
    if isinstance(package_or_update, FactoryPackage):
        source = {
            "package": _display(package_or_update.path),
            "package_sha256": sha256(package_or_update.path),
        }
    elif isinstance(package_or_update, FirmwarePackage):
        source = {
            "package": _display(package_or_update.manifest_path),
            "package_sha256": sha256(package_or_update.manifest_path),
        }
    else:
        source = {
            "image": _display(package_or_update.image),
            "image_sha256": sha256(package_or_update.image),
        }
    plan = {
        "schema": 1,
        "operation": operation,
        "environment": environment.name,
        "chip": environment.sdk_target,
        "port": probe.port,
        "flash_bytes": probe.flash_bytes,
        "identifier": probe.identifier,
        "protocol": "bk7231tools-python",
        "components": components,
        **source,
    }
    path = probe.work_dir / f"{operation}-plan.json"
    path.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    print(f"Beken {operation} plan: {_display(path)}")
    return path


class BekenFlasher(DeviceAdapter):
    """Install fresh configuration on factory flash; preserve NET on updates."""

    platform_name = "Beken"
    default_monitor_baud = DEFAULT_MONITOR_BAUD

    def probe(self, environment: Environment, port: str, baud: int | None) -> ProbeResult:
        with _connected(port, baud) as device:
            return self._probe_connected(device, environment, port)

    @staticmethod
    def _probe_connected(device: Any, environment: Environment, port: str) -> ProbeResult:
        """Identify the chip and flash on an open connection and record the probe."""

        _validate_chip(device, environment)
        flash_bytes = int(getattr(device, "flash_size", 0) or 0)
        if flash_bytes < 2 * 1024 * 1024:
            raise DeviceError(
                f"Beken flash capacity is below Newmota's 2 MiB minimum: {flash_bytes}"
            )
        work_dir = _probe_directory(environment, flash_bytes)
        security = {
            "protocol": _protocol_name(device),
            "bootloader": getattr(getattr(device, "bootloader_type", None), "name", None),
        }
        data = {
            "schema": 1,
            "platform": environment.platform,
            "environment": environment.name,
            "chip": environment.chip,
            "sdk_target": environment.sdk_target,
            "port": port,
            "identifier": _identifier(device),
            "flash_bytes": flash_bytes,
            "security": security,
        }
        path = work_dir / "probe.json"
        path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        print(f"Probe record: {_display(path)}")
        print(
            f"Beken probe: {environment.sdk_target}, {flash_bytes:#x} bytes, "
            f"protocol={security['protocol']}"
        )
        return ProbeResult(
            environment.platform,
            environment.chip,
            port,
            flash_bytes,
            _identifier(device),
            security,
            work_dir,
        )

    def backup(
        self,
        environment: Environment,
        port: str,
        output: Path,
        baud: int | None,
    ) -> None:
        output = output.expanduser().resolve()
        sidecar = Path(f"{output}.json")
        temporary = Path(f"{output}.part")
        for candidate in (output, sidecar, temporary):
            if candidate.exists():
                raise DeviceError(f"refusing to overwrite backup path: {candidate}")
        output.parent.mkdir(parents=True, exist_ok=True)
        try:
            with _connected(port, baud) as device, temporary.open("wb") as stream:
                probe = self._probe_connected(device, environment, port)
                progress = Progress("Reading flash", probe.flash_bytes)
                for chunk in device.flash_read(0, probe.flash_bytes, crc_check=True):
                    stream.write(chunk)
                    progress.update(progress.done + len(chunk))
                progress.end_line()
                stream.flush()
                os.fsync(stream.fileno())
            temporary.replace(output)
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
        metadata = {
            "schema": 1,
            "warning": "This full-chip backup may contain credentials and device secrets.",
            "platform": environment.platform,
            "environment": environment.name,
            "identifier": probe.identifier,
            "port": port,
            "flash_bytes": probe.flash_bytes,
            "file": output.name,
            "sha256": sha256(output),
        }
        sidecar.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        print(f"Backup: {output}")
        print(f"Backup metadata: {sidecar}")
        print("Keep this backup private; it may contain credentials and device secrets.")

    @staticmethod
    def _check_capacity(probe: ProbeResult, required: int) -> None:
        if probe.flash_bytes < required:
            raise DeviceError(
                f"physical Beken flash is {probe.flash_bytes} bytes; package requires {required}"
            )

    def flash(
        self,
        environment: Environment,
        package: FirmwarePackage,
        port: str,
        baud: int | None,
    ) -> None:
        application, filesystem, layout = _package_components(environment, package)
        components = [
            {
                "partition": "application",
                "path": _display(application[1]),
                "offset": layout.region("application").offset,
                "size": application[1].stat().st_size,
            },
            {
                "partition": "filesystem",
                "path": _display(filesystem[1]),
                "offset": layout.region("filesystem").offset,
                "size": filesystem[1].stat().st_size,
            },
        ]
        print(
            "Beken factory flash resets LittleFS and Newmota NET credentials; everything else, "
            "including bootloader, RF calibration, BLE bonding and vendor data, is preserved.",
            flush=True,
        )
        try:
            with _connected(port, baud) as device:
                probe = self._probe_connected(device, environment, port)
                self._check_capacity(probe, package.flash_bytes)
                _check_flash_layout(device, layout, ("application", "net", "filesystem"))
                net = layout.region("net")
                original = b"".join(device.flash_read(net.offset, net.size, crc_check=True))
                # Prepare and validate the reset before any factory write.
                replacement = factory_net_sector(original)
                components.append(
                    {
                        "partition": "net",
                        "offset": net.offset,
                        "size": net.size,
                        "policy": NET_RESET_POLICY,
                        "changed": replacement != original,
                    }
                )
                _write_plan(environment, package, probe, components, "factory-flash")
                total = application[1].stat().st_size + filesystem[1].stat().st_size
                if replacement != original:
                    total += net.size
                progress = Progress("Writing application", total)
                done = 0
                for partition, (_, path) in (
                    ("application", application),
                    ("filesystem", filesystem),
                ):
                    done = _program(
                        device,
                        path,
                        None,
                        layout.region(partition).offset,
                        progress,
                        f"Writing {partition}",
                        done,
                    )
                if replacement != original:
                    _program(
                        device,
                        None,
                        replacement,
                        net.offset,
                        progress,
                        "Initializing NET credentials",
                        done,
                    )
                progress.end_line()
                actual = b"".join(device.flash_read(net.offset, net.size, crc_check=True))
                if actual != replacement:
                    raise DeviceError("Beken NET initialization read-back verification failed")
                device.reboot_chip()
        except DeviceError:
            raise
        except Exception as error:
            raise DeviceError(f"Beken factory flash failed: {error}") from error
        print("Beken factory flash write/verify completed; chip reboot requested.")

    def flash_factory(
        self,
        environment: Environment,
        factory: FactoryPackage,
        port: str,
        baud: int | None,
    ) -> None:
        """Write a factory package from its description alone, as the web flasher does."""

        description = factory.description
        steps = description["steps"]
        foreign = [
            step["kind"] for step in steps if step["kind"] not in (BEKEN_RF_CHECK, BEKEN_NET_RESET)
        ]
        if foreign:
            raise PackageError(
                "Beken cannot carry out factory package steps: " + ", ".join(foreign)
            )
        rf = next((step for step in steps if step["kind"] == BEKEN_RF_CHECK), None)
        if rf is None:
            raise PackageError("Beken factory package has no RF calibration check")
        net = next((step for step in steps if step["kind"] == BEKEN_NET_RESET), None)
        if net is not None and (
            net["policy"] != NET_RESET_POLICY or net["size"] != NET_SECTOR_SIZE
        ):
            raise PackageError("Beken factory package needs a NET reset this flasher does not have")
        writes = description["write"]
        written = [(item["offset"], item["offset"] + item["size"]) for item in writes]
        if net is not None:
            written.append((net["offset"], net["offset"] + net["size"]))
        components: list[dict[str, object]] = [
            {
                "name": item["name"],
                "offset": item["offset"],
                "size": item["size"],
                "sha256": item["sha256"],
            }
            for item in writes
        ]
        print(
            "Beken factory flash writes the package images"
            + (" and resets Newmota NET credentials" if net is not None else "")
            + "; everything else, including "
            + ", ".join(description["preserve"] or ["the rest of the flash"])
            + ", is preserved.",
            flush=True,
        )
        try:
            with _connected(port, baud) as device:
                probe = self._probe_connected(device, environment, port)
                self._check_capacity(probe, description["target"]["flash_bytes"])
                _check_rf_calibration(device, rf["offset"], written)
                original = replacement = b""
                if net is not None:
                    original = b"".join(
                        device.flash_read(net["offset"], net["size"], crc_check=True)
                    )
                    # Prepare and validate the reset before any factory write.
                    replacement = factory_net_sector(original)
                    components.append(
                        {
                            "partition": "net",
                            "offset": net["offset"],
                            "size": net["size"],
                            "policy": NET_RESET_POLICY,
                            "changed": replacement != original,
                        }
                    )
                _write_plan(environment, factory, probe, components, "factory-flash")
                total = sum(item["size"] for item in writes)
                if replacement != original:
                    total += net["size"]
                progress = Progress("Writing", total)
                done = 0
                for item in writes:
                    done = _program(
                        device,
                        None,
                        factory.images[item["file"]],
                        item["offset"],
                        progress,
                        f"Writing {item['name']}",
                        done,
                    )
                if net is not None and replacement != original:
                    _program(
                        device,
                        None,
                        replacement,
                        net["offset"],
                        progress,
                        "Initializing NET credentials",
                        done,
                    )
                progress.end_line()
                if net is not None:
                    actual = b"".join(device.flash_read(net["offset"], net["size"], crc_check=True))
                    if actual != replacement:
                        raise DeviceError("Beken NET initialization read-back verification failed")
                device.reboot_chip()
        except DeviceError:
            raise
        except Exception as error:
            raise DeviceError(f"Beken factory flash failed: {error}") from error
        print("Beken factory flash write/verify completed; chip reboot requested.")

    def update(
        self,
        environment: Environment,
        update: ApplicationUpdate,
        port: str,
        baud: int | None,
    ) -> None:
        layout = _layout_for(environment)
        ota = layout.region("ota")
        limit = ota.image_budget or ota.size
        if update.offset != ota.offset or update.size_limit != limit:
            raise DeviceError("Beken update does not match the resolved stock OTA staging region")
        if not update.image.is_file():
            raise DeviceError(f"Beken update image is missing: {update.image}")
        image_size = update.image.stat().st_size
        if image_size <= 0 or image_size > limit:
            raise DeviceError(f"Beken update image does not fit stock OTA staging: {image_size}")
        try:
            image_config = load_image_config(environment)
            validate_image_layout(image_config, layout)
            validate_ota_image(update.image, image_config, limit, layout)
        except BekenImageError as error:
            raise DeviceError(str(error)) from error
        components = [
            {
                "partition": "ota",
                "path": _display(update.image),
                "offset": update.offset,
                "size": image_size,
                "size_limit": limit,
            }
        ]
        try:
            with _connected(port, baud) as device:
                probe = self._probe_connected(device, environment, port)
                self._check_capacity(probe, environment.flash_kb * 1024)
                _check_flash_layout(device, layout, ("ota",))
                _write_plan(environment, update, probe, components, "ota-update")
                progress = Progress("Writing application", image_size)
                _program(
                    device, update.image, None, update.offset, progress, "Writing application", 0
                )
                progress.end_line()
                device.reboot_chip()
        except DeviceError:
            raise
        except Exception as error:
            raise DeviceError(f"Beken OTA update failed: {error}") from error
        print("Beken OTA update write/verify completed; chip reboot requested.")

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
