"""ESP-IDF probing, backup, size-aware factory flashing, and monitoring."""

from __future__ import annotations

import csv
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path, PurePosixPath

from newmota.device import DeviceAdapter, DeviceError, ProbeResult
from newmota.factory_package import ESP_PARTITION_TABLE, FactoryPackage
from newmota.host import SdkVersion, run, sha256
from newmota.model import ROOT, WORK_DIR, ApplicationUpdate, Environment
from newmota.package import FirmwarePackage, PackageError
from newmota.platforms.esp.build import ensure_flash_tools

FLASH_SIZE = re.compile(r"Detected flash size:\s*(\d+)\s*(KB|MB)", re.IGNORECASE)
MAC_ADDRESS = re.compile(r"(?:MAC|BASE MAC):\s*([0-9a-f]{2}(?::[0-9a-f]{2}){5})", re.IGNORECASE)
CHIP_DESCRIPTION = re.compile(r"Chip is\s+(.+)")
CHIP_REVISION = re.compile(r"\(revision v(\d+)\.\d+\)")
READ_MEM = re.compile(r"0x([0-9a-f]{8}) = 0x([0-9a-f]{8})", re.IGNORECASE)
# The ESP32 ROM has no GET_SECURITY_INFO; esptool's ESP32ROM reads these eFuse words instead.
ESP32_EFUSE_BLK0_RDATA0 = 0x3FF5A000
ESP32_EFUSE_BLK0_RDATA6 = 0x3FF5A018
ESP32_ABS_DONE_0 = 1 << 4
ESP32_ABS_DONE_1 = 1 << 5
FLASH_MANUFACTURER = re.compile(r"Manufacturer:\s*(\S+)", re.IGNORECASE)
FLASH_DEVICE = re.compile(r"Device:\s*(\S+)", re.IGNORECASE)
DOWNLOAD_MODE_HINT = "hold BOOT, press and release RESET/EN, then release BOOT"
# ESP-IDF partition table: 32-byte entries, then an MD5 entry over them (gen_esp32part.py).
PARTITION_ENTRY = struct.Struct("<2sBBII16sI")
PARTITION_MAGIC = b"\xaa\x50"
PARTITION_MD5 = b"\xeb\xeb" + b"\xff" * 14


def _flash_size_name(size: int) -> str:
    if size < 2 * 1024 * 1024 or size % (1024 * 1024):
        raise DeviceError(f"unsupported physical flash capacity: {size} bytes")
    megabytes = size // (1024 * 1024)
    if megabytes not in {2, 4, 8, 16, 32, 64, 128}:
        raise DeviceError(f"esptool does not support {megabytes}MB flash for this workflow")
    return f"{megabytes}MB"


def _display(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def _replace_flash_size(arguments: list[str], size: str) -> list[str]:
    result = list(arguments)
    for option in ("--flash-size", "--flash_size"):
        if option in result:
            index = result.index(option)
            if index + 1 >= len(result):
                raise PackageError(f"{option} has no value in package write arguments")
            result[index + 1] = size
            return result
    result.extend(("--flash-size", size))
    return result


def _recommended_idf_tool(sdk: Path, tools_path: Path, executable: str) -> Path | None:
    """Find an executable only inside tool versions recommended by this IDF checkout."""

    try:
        manifest = json.loads((sdk / "tools" / "tools.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    matches: list[Path] = []
    for tool in manifest.get("tools", []):
        if not isinstance(tool, dict) or not isinstance(tool.get("name"), str):
            continue
        versions = tool.get("versions")
        if not isinstance(versions, list):
            continue
        recommended = next(
            (
                version.get("name")
                for version in versions
                if isinstance(version, dict)
                and version.get("status") == "recommended"
                and isinstance(version.get("name"), str)
            ),
            None,
        )
        if recommended is None:
            continue
        destination = tools_path / "tools" / str(tool["name"]) / recommended
        matches.extend(path for path in destination.rglob(executable) if path.is_file())
    return matches[0] if len(matches) == 1 else None


def _grow_partition_csv(
    source: Path,
    destination: Path,
    name: str,
    offset: int,
    flash_bytes: int,
) -> None:
    comments: list[str] = []
    rows: list[list[str]] = []
    for line in source.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("#"):
            comments.append(line)
            continue
        parsed = next(csv.reader([line]))
        if parsed and any(field.strip() for field in parsed):
            rows.append([field.strip() for field in parsed])
    if not rows:
        raise DeviceError("packaged ESP-IDF partition layout is empty")
    matches = [index for index, row in enumerate(rows) if row and row[0] == name]
    if matches != [len(rows) - 1]:
        raise DeviceError("expandable filesystem is not the final partition in its layout")
    row = rows[-1]
    if len(row) < 5 or row[1:3] not in (["data", "spiffs"], ["data", "littlefs"]):
        raise DeviceError("expandable final partition is not a supported filesystem")
    try:
        source_offset = int(row[3], 0)
    except ValueError as error:
        raise DeviceError("expandable filesystem has an invalid offset") from error
    if source_offset != offset or offset % 0x1000:
        raise DeviceError("expandable filesystem boundary does not match its manifest")
    new_size = flash_bytes - offset
    if new_size <= 0 or new_size % 0x1000:
        raise DeviceError("detected flash cannot produce an aligned filesystem partition")
    row[4] = f"0x{new_size:X}"
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("w", newline="", encoding="utf-8") as output:
        if comments:
            output.write("\n".join(comments) + "\n")
        output.write("# Device-specific copy: only the final filesystem size is adjusted.\n")
        writer = csv.writer(output, lineterminator="\n")
        writer.writerows(rows)


def _grow_partition_table(table: bytes, name: str, offset: int, flash_bytes: int) -> bytes:
    """Return the binary @p table with its final filesystem grown to the end of the flash.

    Only that entry's size and the MD5 entry change, so flags and every other partition stay
    exactly as packaged.
    """

    entries: list[list[object]] = []
    position = 0
    while (
        position + PARTITION_ENTRY.size <= len(table)
        and table[position : position + 2] == PARTITION_MAGIC
    ):
        entries.append(list(PARTITION_ENTRY.unpack_from(table, position)))
        position += PARTITION_ENTRY.size
    checksum = table[position : position + 32]
    if (
        not entries
        or checksum[:16] != PARTITION_MD5
        or checksum[16:] != hashlib.md5(table[:position]).digest()
    ):
        raise DeviceError("packaged ESP-IDF partition table is not a checksummed table")
    final = entries[-1]
    label = bytes(final[5]).rstrip(b"\0").decode("ascii", "replace")
    if label != name or final[3] != offset:
        raise DeviceError("expandable filesystem is not the final partition in its table")
    new_size = flash_bytes - offset
    if new_size <= 0 or new_size % 0x1000:
        raise DeviceError("detected flash cannot produce an aligned filesystem partition")
    final[4] = new_size
    body = b"".join(PARTITION_ENTRY.pack(*entry) for entry in entries)
    grown = body + PARTITION_MD5 + hashlib.md5(body).digest()
    return grown + b"\xff" * (len(table) - len(grown))


def _security_from_info(output: str) -> dict[str, bool | None]:
    """Read esptool get-security-info output; a state it does not print stays unknown."""

    def state(name: str) -> bool | None:
        if f"{name}: Enabled" in output:
            return True
        if f"{name}: Disabled" in output:
            return False
        return None

    return {
        "secure_boot": state("Secure Boot"),
        "flash_encryption": state("Flash Encryption"),
        "secure_download_mode": "Secure Download Mode" in output,
    }


def _esp32_security(output: str) -> dict[str, bool | None]:
    """Decide the ESP32 security state from read-mem eFuse words, as esptool's ESP32ROM does."""

    words = {int(address, 16): int(value, 16) for address, value in READ_MEM.findall(output)}
    rdata0 = words.get(ESP32_EFUSE_BLK0_RDATA0)
    abs_done = words.get(ESP32_EFUSE_BLK0_RDATA6)
    revision = CHIP_REVISION.search(output)
    if rdata0 is None or abs_done is None or revision is None:
        return {"secure_boot": None, "flash_encryption": None, "secure_download_mode": None}
    # Secure boot v2 (ABS_DONE_1) exists from chip revision 3.
    secure_boot = bool(abs_done & ESP32_ABS_DONE_0) or (
        int(revision.group(1)) >= 3 and bool(abs_done & ESP32_ABS_DONE_1)
    )
    # Flash encryption is on while FLASH_CRYPT_CNT has an odd number of bits set.
    flash_encryption = ((rdata0 >> 20) & 0x7F).bit_count() % 2 == 1
    # The ESP32 ROM has no secure download mode.
    return {
        "secure_boot": secure_boot,
        "flash_encryption": flash_encryption,
        "secure_download_mode": False,
    }


def _check_factory_security(probe: ProbeResult) -> None:
    if any(probe.security.get(field) is not False for field in ("secure_boot", "flash_encryption")):
        raise DeviceError(
            "factory write requires secure boot and flash encryption to be explicitly disabled"
        )
    if probe.security.get("secure_download_mode") is not False:
        raise DeviceError("factory write is not supported in ESP secure download mode")


class EspIdfFlasher(DeviceAdapter):
    default_monitor_baud = 115200

    def __init__(self) -> None:
        self._tools_cache: dict[str, tuple[dict[str, str], Path, SdkVersion]] = {}
        self._sdk_key = "esp-idf"

    def _tools(self) -> tuple[dict[str, str], Path, SdkVersion]:
        if self._sdk_key not in self._tools_cache:
            sdk, environment, python = ensure_flash_tools(self._sdk_key)
            self._tools_cache[self._sdk_key] = (environment, python, sdk)
        return self._tools_cache[self._sdk_key]

    @staticmethod
    def _post_operation_reset(environment: Environment, configured: str = "hard-reset") -> str:
        # A conventional RTS hard reset can leave ESP32-S3 USB Serial/JTAG in
        # ROM download mode. Esptool's watchdog reset starts the SPI image
        # without resampling that USB control-line boot sequence.
        if environment.sdk_target == "esp32s3" and environment.console == "usb-serial-jtag":
            return "watchdog-reset"
        return configured.replace("_", "-")

    def _esptool(
        self,
        chip: str,
        port: str,
        baud: int | None,
        before: str,
        after: str,
        command: str,
        *arguments: str,
        capture: bool = False,
        no_stub: bool = False,
    ):
        environment, python, sdk = self._tools()
        separator = "-" if sdk.key == "esp-idf-v6.1" else "_"

        def dialect(value: str) -> str:
            return value.replace("_", "-") if separator == "-" else value.replace("-", "_")

        invocation = [
            str(python),
            "-m",
            "esptool",
            "--chip",
            chip,
            "--port",
            port,
        ]
        if baud is not None:
            invocation.extend(("--baud", str(baud)))
        if no_stub:
            invocation.append("--no-stub")
        normalized_arguments = tuple(
            "--" + dialect(argument[2:]) if argument.startswith("--") else argument
            for argument in arguments
        )
        invocation.extend(
            (
                "--before",
                dialect(before),
                "--after",
                dialect(after),
                dialect(command),
                *normalized_arguments,
            )
        )
        if command.replace("_", "-") == "write-flash" and separator == "_":
            invocation.insert(invocation.index("write_flash") + 1, "--verify")
        try:
            result = run(invocation, environment=environment, capture=capture)
        except subprocess.CalledProcessError as error:
            if capture:
                if error.stdout:
                    print(error.stdout.rstrip())
                if error.stderr:
                    print(error.stderr.rstrip(), file=sys.stderr)
                if "Failed to connect" in f"{error.stdout or ''}{error.stderr or ''}":
                    print(
                        f"ESP: no answer. Enter download mode: {DOWNLOAD_MODE_HINT}, then retry",
                        file=sys.stderr,
                        flush=True,
                    )
            raise
        if capture and result.stdout:
            print(result.stdout.rstrip())
        return result

    def probe(self, environment: Environment, port: str, baud: int | None) -> ProbeResult:
        self._sdk_key = environment.sdk_key
        flash_result = self._esptool(
            environment.sdk_target,
            port,
            baud,
            "default-reset",
            "no-reset",
            "flash-id",
            capture=True,
            # Keep the ROM loader active for the following query. Exiting an
            # uploaded stub can re-enumerate a native USB-OTG connection.
            no_stub=True,
        )
        if environment.sdk_target == "esp32":
            security_output = "\n".join(
                self._esptool(
                    environment.sdk_target,
                    port,
                    baud,
                    "no-reset",
                    self._post_operation_reset(environment) if last else "no-reset",
                    "read-mem",
                    hex(address),
                    capture=True,
                    no_stub=True,
                ).stdout
                for address, last in (
                    (ESP32_EFUSE_BLK0_RDATA0, False),
                    (ESP32_EFUSE_BLK0_RDATA6, True),
                )
            )
        else:
            security_output = self._esptool(
                environment.sdk_target,
                port,
                baud,
                "no-reset",
                self._post_operation_reset(environment),
                "get-security-info",
                capture=True,
                no_stub=True,
            ).stdout
        combined = flash_result.stdout + "\n" + security_output
        size_match = FLASH_SIZE.search(combined)
        if size_match is None:
            raise DeviceError("esptool did not report a supported physical flash size")
        amount, unit = size_match.groups()
        flash_bytes = int(amount) * (1024 if unit.upper() == "KB" else 1024 * 1024)
        _flash_size_name(flash_bytes)
        mac_match = MAC_ADDRESS.search(combined)
        description_match = CHIP_DESCRIPTION.search(combined)
        security = (
            _esp32_security(combined)
            if environment.sdk_target == "esp32"
            else _security_from_info(combined)
        )
        work_dir = WORK_DIR / environment.name / "flash" / _flash_size_name(flash_bytes).lower()
        work_dir.mkdir(parents=True, exist_ok=True)
        (work_dir / "probe.txt").write_text(combined.rstrip() + "\n", encoding="utf-8")
        probe_data = {
            "schema": 1,
            "platform": environment.platform,
            "environment": environment.name,
            "expected_chip": environment.sdk_target,
            "description": description_match.group(1).strip() if description_match else None,
            "identifier": mac_match.group(1).lower() if mac_match else None,
            "flash_manufacturer": (
                FLASH_MANUFACTURER.search(combined).group(1)
                if FLASH_MANUFACTURER.search(combined)
                else None
            ),
            "flash_device": (
                FLASH_DEVICE.search(combined).group(1) if FLASH_DEVICE.search(combined) else None
            ),
            "port": port,
            "flash_bytes": flash_bytes,
            "security": security,
        }
        (work_dir / "probe.json").write_text(
            json.dumps(probe_data, indent=2) + "\n", encoding="utf-8"
        )
        print(f"Probe record: {_display(work_dir / 'probe.json')}")
        return ProbeResult(
            environment.platform,
            environment.chip,
            port,
            flash_bytes,
            probe_data["identifier"],
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
        probe = self.probe(environment, port, baud)
        output.parent.mkdir(parents=True, exist_ok=True)
        try:
            self._esptool(
                environment.sdk_target,
                port,
                baud,
                "default-reset",
                self._post_operation_reset(environment),
                "read-flash",
                "0x0",
                hex(probe.flash_bytes),
                str(temporary),
            )
            temporary.replace(output)
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
        metadata = {
            "schema": 1,
            "warning": "This full-chip backup may contain credentials and device secrets.",
            "platform": environment.platform,
            "environment": environment.name,
            "chip": environment.chip,
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

    def _flash_plan(
        self,
        environment: Environment,
        package: FirmwarePackage,
        probe: ProbeResult,
    ) -> tuple[list[str], list[str], Path]:
        target = package.data["target"]
        if target.get("environment") != environment.name:
            raise DeviceError("package environment does not match the selected catalog environment")
        if (
            target.get("chip") != environment.chip
            or target.get("sdk_target") != environment.sdk_target
        ):
            raise DeviceError("package chip does not match the selected catalog environment")
        if probe.flash_bytes < package.flash_bytes:
            raise DeviceError(
                f"physical flash is {probe.flash_bytes} bytes, package requires {package.flash_bytes}"
            )
        _check_factory_security(probe)

        flash = package.data["flash"]
        if not isinstance(flash, dict):
            raise PackageError("package flash field is invalid")
        raw_components = flash.get("components")
        raw_arguments = flash.get("write_args")
        if not isinstance(raw_components, list) or not isinstance(raw_arguments, list):
            raise PackageError("package has no valid ESP-IDF flash plan")
        arguments = _replace_flash_size(
            [str(argument) for argument in raw_arguments], _flash_size_name(probe.flash_bytes)
        )
        components: list[tuple[int, Path, str]] = []
        for raw in raw_components:
            if not isinstance(raw, dict):
                raise PackageError("package contains an invalid flash component")
            path = package.file(str(raw.get("path", "")))
            offset = raw.get("offset")
            role = raw.get("role")
            if not isinstance(offset, int) or not isinstance(role, str):
                raise PackageError("package flash component lacks an offset or role")
            components.append((offset, path, role))

        work_dir = probe.work_dir
        if probe.flash_bytes > package.flash_bytes:
            table = flash.get("partition_table")
            if not isinstance(table, dict):
                raise DeviceError("package does not describe safe partition-table adaptation")
            expandable = table.get("expandable_final")
            if not isinstance(expandable, dict):
                raise DeviceError("package has no expandable final filesystem declaration")
            layout = package.file(str(table.get("layout", "")))
            adjusted_csv = work_dir / "partition.csv"
            adjusted_binary = work_dir / "partition-table.bin"
            name = expandable.get("name")
            offset = expandable.get("offset")
            if not isinstance(name, str) or not isinstance(offset, int):
                raise DeviceError("package expandable filesystem declaration is invalid")
            _grow_partition_csv(layout, adjusted_csv, name, offset, probe.flash_bytes)
            environment_vars, python, sdk = self._tools()
            generator = sdk.source / "components" / "partition_table" / "gen_esp32part.py"
            run(
                [str(python), str(generator), str(adjusted_csv), str(adjusted_binary)],
                environment=environment_vars,
            )
            table_offset = table.get("offset")
            if not isinstance(table_offset, int):
                raise DeviceError("package partition-table offset is invalid")
            replaced = False
            updated: list[tuple[int, Path, str]] = []
            for component_offset, component_path, role in components:
                if role == "partition-table" and component_offset == table_offset:
                    updated.append((component_offset, adjusted_binary, role))
                    replaced = True
                else:
                    updated.append((component_offset, component_path, role))
            if not replaced:
                raise DeviceError("package flash plan does not contain its partition table")
            components = updated

        components.sort(key=lambda item: item[0])
        previous_end = 0
        pairs: list[str] = []
        plan_components: list[dict[str, object]] = []
        for offset, path, role in components:
            size = path.stat().st_size
            if offset < previous_end or offset + size > probe.flash_bytes:
                raise DeviceError(f"flash component {role} overlaps or exceeds physical flash")
            previous_end = offset + size
            pairs.extend((hex(offset), str(path)))
            plan_components.append(
                {
                    "role": role,
                    "offset": offset,
                    "size": size,
                    "sha256": sha256(path),
                    "path": _display(path),
                }
            )
        plan = {
            "schema": 1,
            "package": str(package.manifest_path),
            "package_sha256": sha256(package.manifest_path),
            "port": probe.port,
            "chip": environment.sdk_target,
            "flash_bytes": probe.flash_bytes,
            "partition_adjusted": probe.flash_bytes > package.flash_bytes,
            "write_args": arguments,
            "components": plan_components,
        }
        plan_path = work_dir / "plan.json"
        plan_path.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
        return arguments, pairs, plan_path

    def _validate_tooling(self, package: FirmwarePackage) -> None:
        from newmota.host import load_sdk_version

        package_sdk = package.data.get("sdk")
        if not isinstance(package_sdk, dict):
            raise DeviceError("package has no ESP-IDF SDK identity")
        matching = [
            key
            for key in ("esp-idf", "esp-idf-v6.1")
            if load_sdk_version(key).revision == package_sdk.get("revision")
        ]
        if len(matching) != 1:
            raise DeviceError("package ESP-IDF revision is not a supported pinned SDK")
        pin = load_sdk_version(matching[0])
        if package_sdk.get("version") != pin.version or package_sdk.get("name") != pin.name:
            raise DeviceError("package ESP-IDF identity does not match its pinned SDK")
        self._sdk_key = matching[0]
        environment_vars, python, sdk = self._tools()
        if not isinstance(package_sdk, dict) or package_sdk.get("revision") != sdk.revision:
            raise DeviceError("package ESP-IDF revision does not match the pinned checkout")
        host_tools = package.data.get("host_tools")
        expected = host_tools.get("esptool") if isinstance(host_tools, dict) else None
        if not isinstance(expected, str):
            raise DeviceError("package does not pin its esptool version")
        result = run(
            [str(python), "-m", "esptool", "version"],
            environment=environment_vars,
            capture=True,
        )
        actual = next(
            (line.strip() for line in reversed(result.stdout.splitlines()) if line.strip()), ""
        )
        if actual != expected:
            raise DeviceError(
                f"esptool version mismatch: package requires {expected}, got {actual}"
            )

    def flash(
        self,
        environment: Environment,
        package: FirmwarePackage,
        port: str,
        baud: int | None,
    ) -> None:
        self._validate_tooling(package)
        if self._sdk_key != environment.sdk_key:
            raise DeviceError("package ESP-IDF selection does not match the selected environment")
        probe = self.probe(environment, port, baud)
        arguments, pairs, plan_path = self._flash_plan(environment, package, probe)
        flash = package.data["flash"]
        before = str(flash.get("before", "default-reset"))
        after = self._post_operation_reset(environment, str(flash.get("after", "hard-reset")))
        print(f"Flash plan: {_display(plan_path)}")
        self._esptool(
            environment.sdk_target,
            port,
            baud,
            before,
            after,
            "write-flash",
            *arguments,
            *pairs,
        )

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
        foreign = [step["kind"] for step in steps if step["kind"] != ESP_PARTITION_TABLE]
        if foreign:
            raise PackageError("ESP cannot carry out factory package steps: " + ", ".join(foreign))
        options = description["options"].get("esp")
        if not isinstance(options, dict) or not all(
            isinstance(options.get(name), str)
            for name in ("flash_mode", "flash_freq", "flash_size")
        ):
            raise PackageError("ESP factory package has no flash mode, frequency and size")
        package_bytes = description["target"]["flash_bytes"]
        probe = self.probe(environment, port, baud)
        if probe.flash_bytes < package_bytes:
            raise DeviceError(
                f"physical flash is {probe.flash_bytes} bytes, package requires {package_bytes}"
            )
        _check_factory_security(probe)
        arguments = _replace_flash_size(
            [
                "--flash_mode",
                options["flash_mode"],
                "--flash_freq",
                options["flash_freq"],
                "--flash_size",
                options["flash_size"],
            ],
            _flash_size_name(probe.flash_bytes),
        )
        table = next(iter(steps), None)
        adjusted = probe.flash_bytes > package_bytes
        if adjusted and table is None:
            raise DeviceError("package does not describe safe partition-table adaptation")
        work_dir = probe.work_dir / "factory-package"
        work_dir.mkdir(parents=True, exist_ok=True)
        pairs: list[str] = []
        components: list[dict[str, object]] = []
        for item in sorted(description["write"], key=lambda entry: entry["offset"]):
            data = factory.images[item["file"]]
            if adjusted and item["role"] == "partition-table" and item["offset"] == table["offset"]:
                expandable = table["expandable_final"]
                data = _grow_partition_table(
                    data, str(expandable["name"]), int(expandable["offset"]), probe.flash_bytes
                )
            path = work_dir / PurePosixPath(item["file"]).name
            path.write_bytes(data)
            pairs.extend((hex(item["offset"]), str(path)))
            components.append(
                {
                    "role": item["role"],
                    "offset": item["offset"],
                    "size": len(data),
                    "sha256": hashlib.sha256(data).hexdigest(),
                    "path": _display(path),
                }
            )
        plan = {
            "schema": 1,
            "package": str(factory.path),
            "package_sha256": sha256(factory.path),
            "port": probe.port,
            "chip": environment.sdk_target,
            "flash_bytes": probe.flash_bytes,
            "partition_adjusted": adjusted,
            "write_args": arguments,
            "components": components,
        }
        plan_path = work_dir / "plan.json"
        plan_path.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
        print(f"Flash plan: {_display(plan_path)}")
        self._esptool(
            environment.sdk_target,
            port,
            baud,
            "default-reset",
            self._post_operation_reset(environment),
            "write-flash",
            *arguments,
            *pairs,
        )

    def update(
        self,
        environment: Environment,
        update: ApplicationUpdate,
        port: str,
        baud: int | None,
    ) -> None:
        """Probe and write exactly one image to the fixed app0 partition."""

        probe = self.probe(environment, port, baud)
        if probe.flash_bytes < environment.flash_kb * 1024:
            raise DeviceError(
                f"physical flash is {probe.flash_bytes} bytes, environment requires "
                f"{environment.flash_kb * 1024}"
            )
        if any(
            probe.security.get(field) is not False for field in ("secure_boot", "flash_encryption")
        ):
            raise DeviceError(
                "application update requires secure boot and flash encryption to be "
                "explicitly disabled"
            )
        if probe.security.get("secure_download_mode") is not False:
            raise DeviceError("application update is unsupported in ESP secure download mode")
        image_size = update.image.stat().st_size
        if update.offset < 0 or update.offset % 0x1000:
            raise DeviceError("application update partition offset is invalid")
        if image_size <= 0 or image_size > update.size_limit:
            raise DeviceError("application update image does not fit its app0 partition")
        if update.offset + update.size_limit > probe.flash_bytes:
            raise DeviceError("app0 partition exceeds the detected physical flash")

        plan = {
            "schema": 1,
            "operation": "application-update",
            "port": probe.port,
            "chip": environment.sdk_target,
            "environment": environment.name,
            "flash_bytes": probe.flash_bytes,
            "component": {
                "role": "application",
                "partition": "app0",
                "offset": update.offset,
                "size": image_size,
                "size_limit": update.size_limit,
                "sha256": sha256(update.image),
                "path": _display(update.image),
            },
        }
        plan_path = probe.work_dir / "update-plan.json"
        plan_path.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
        print(f"Update plan: {_display(plan_path)}")
        self._esptool(
            environment.sdk_target,
            port,
            baud,
            "default-reset",
            self._post_operation_reset(environment),
            "write-flash",
            hex(update.offset),
            str(update.image),
        )

    def monitor(
        self,
        environment: Environment,
        package: FirmwarePackage | None,
        port: str,
        baud: int | None,
        after_flash: bool,
    ) -> None:
        self._sdk_key = environment.sdk_key
        if package is not None:
            self._validate_tooling(package)
        environment_vars, python, sdk = self._tools()
        monitor_data: dict[str, object] = {}
        elf: Path | None = None
        if package is not None:
            raw_monitor = package.data.get("monitor")
            if isinstance(raw_monitor, dict):
                monitor_data = raw_monitor
                raw_elf = raw_monitor.get("elf")
                if isinstance(raw_elf, str):
                    elf = package.file(raw_elf)
        else:
            description = WORK_DIR / environment.name / "project_description.json"
            if description.is_file():
                try:
                    loaded = json.loads(description.read_text(encoding="utf-8"))
                except json.JSONDecodeError as error:
                    raise DeviceError(
                        f"invalid ESP-IDF project description: {description}"
                    ) from error
                if isinstance(loaded, dict):
                    monitor_data = loaded
            candidate = WORK_DIR / environment.name / "newmota.elf"
            if candidate.is_file():
                elf = candidate

        selected_baud = baud or int(
            monitor_data.get("baud", monitor_data.get("monitor_baud", 115200))
        )
        prefix = str(
            monitor_data.get("toolchain_prefix", monitor_data.get("monitor_toolprefix", ""))
        )
        on_chip_usb = environment.console in {"usb-cdc", "usb-serial-jtag"}
        monitor = (
            Path(__file__).with_name("usb_monitor.py")
            if on_chip_usb
            else sdk.source / "tools" / "idf_monitor.py"
        )
        command = [
            str(python),
            str(monitor),
            "--port",
            port,
            "--baud",
            str(selected_baud),
            "--target",
            environment.sdk_target,
        ]
        if on_chip_usb:
            command.append("--no-reset")
        if after_flash:
            # Flashing already reset the target. Wait for USB re-enumeration or
            # a slower external serial adapter instead of racing its return.
            command.extend(("--open-port-attempts", "0"))
        if elf is not None and prefix:
            tools_path = Path(
                os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif")
            ).expanduser()
            addr2line = _recommended_idf_tool(sdk.source, tools_path, f"{prefix}addr2line")
            if addr2line is not None:
                existing_path = environment_vars.get("PATH", "")
                environment_vars["PATH"] = os.pathsep.join((str(addr2line.parent), existing_path))
                command.extend(("--toolchain-prefix", prefix, str(elf)))
            else:
                print("ESP-IDF address decoding disabled: matching binutils are not installed.")
                command.append("--disable-address-decoding")
        else:
            command.append("--disable-address-decoding")
        run(command, environment=environment_vars)
