"""Load and validate the declarative build catalog and its native requirements."""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass, replace
from pathlib import Path

from newmota.berry_drivers import check_solidify, solidify_list
from newmota.config import BuildConfigError, parse_native_config
from newmota.factory_files import check_filesystem, filesystem_list
from newmota.i18n import DEFAULT_LANGUAGE, I18nError, canonical_locale_id, load_locale
from newmota.model import (
    BOARD_DIR,
    CHIP_DIR,
    DIRECTORY_PLATFORMS,
    ENV_DIR,
    ENV_MATRIX_DIR,
    PARTITIONS_DIR,
    PLATFORM_DIRECTORIES,
    ROOT,
    USER_CONFIG_DIR,
    BoardDefinition,
    BoardGpioDefinition,
    BuildError,
    CatalogError,
    ChipDefinition,
    Environment,
    FactoryFile,
    MemoryDefinition,
    esp_sdk_key,
)
from newmota.platforms import platform_for_build

TARGET_ID = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*")
SDK_ID = re.compile(r"[A-Za-z0-9](?:[A-Za-z0-9_.-]*[A-Za-z0-9])?")
VERSION_ID = re.compile(r"[0-9a-z]+(?:[0-9a-z.-]*[0-9a-z])?")
CAPABILITIES = (
    "wifi",
    "ble",
    "i2s",
    "psram",
    "usb-otg",
    "usb-serial-jtag",
    "ieee802154",
    "lcd-controller",
    "camera-dvp",
    "mipi-csi",
    # Wi-Fi through an ESP-Hosted companion; no SoC macro, chips and boards set it.
    "wifi-hosted",
)
ESP_IDF_CAPABILITY_MACROS = {
    "wifi": "SOC_WIFI_SUPPORTED",
    "ble": "SOC_BLE_SUPPORTED",
    "i2s": "SOC_I2S_SUPPORTED",
    "psram": "SOC_SPIRAM_SUPPORTED",
    "usb-otg": "SOC_USB_OTG_SUPPORTED",
    "usb-serial-jtag": "SOC_USB_SERIAL_JTAG_SUPPORTED",
    "ieee802154": "SOC_IEEE802154_SUPPORTED",
    "lcd-controller": "SOC_LCDCAM_SUPPORTED",
    "camera-dvp": "SOC_LCDCAM_CAM_SUPPORTED",
    "mipi-csi": "SOC_MIPI_CSI_SUPPORTED",
}
CHIP_FIELDS = {"sdk_target", "build_status", "capability_overrides", "idf_version"}
BOARD_FIELDS = {
    "chip",
    "flash",
    "psram",
    "console",
    "sdk_board",
    "profile_default",
    "revision",
    "cpu_cores",
    "gpio",
    "capability_overrides",
    "flash_layout",
}
MEMORY_FIELDS = {"size_kb", "mode", "frequency_mhz"}
BOARD_GPIO_FIELDS = {"source", "exposed", "reserved"}
ENVIRONMENT_FIELDS = {
    "name",
    "board",
    "policy",
    "role",
    "profile",
    "safeboot",
    "language",
    "native",
    "crypto",
    "features",
    "idf_version",
    "device_name",
    "template",
    "solidify",
    "filesystem",
}
CONSOLES = {"uart0", "usb-cdc", "usb-serial-jtag"}
ENVIRONMENT_ROLES = {"application", "safeboot"}
PROFILES = {"Compact", "Standard", "Large", "Safeboot"}


def _device_name(data: dict[str, object], role: object, path: Path) -> str | None:
    """Return a checked default for the persisted DeviceName, or None."""

    value = data.get("device_name")
    if value is None:
        return None
    source = path.relative_to(ROOT)
    if role != "application":
        raise CatalogError(f"{source} device_name is only valid for application environments")
    if (
        not isinstance(value, str)
        or not 1 <= len(value.encode("utf-8")) <= 32
        or any(ord(character) < 32 or ord(character) == 127 for character in value)
    ):
        raise CatalogError(f"{source} device_name must be text of 1 to 32 bytes")
    return value


def _idf_version(
    data: dict[str, object], platform: str, path: Path, default: str | None = None
) -> str | None:
    if platform != "esp":
        if "idf_version" in data:
            raise CatalogError(f"{path.relative_to(ROOT)} idf_version is only valid for ESP-IDF")
        return None
    value = data.get("idf_version", default or "5.5")
    if value not in ("5.5", "6.1"):
        raise CatalogError(f'{path.relative_to(ROOT)} idf_version must be "5.5" or "6.1"')
    return str(value)


def recovery_for_environment(
    item: Environment, available: dict[str, Environment]
) -> Environment | None:
    if item.safeboot is None:
        return None
    recovery = available[item.safeboot]
    if item.platform != "esp" or item.sdk_key == recovery.sdk_key:
        return recovery
    suffix = f"-idf-{item.idf_version.replace('.', '-')}"
    name = recovery.name + suffix
    if len(name) >= 32:
        digest = hashlib.sha256(recovery.name.encode("ascii")).hexdigest()[:8]
        prefix = recovery.name[: 31 - len(suffix) - 9].rstrip("-")
        name = f"{prefix}-{digest}{suffix}"
    return replace(
        recovery,
        name=name,
        idf_version=item.idf_version,
        idf_version_source=item.idf_version_source,
        capabilities=item.capabilities,
        recovery_base_environment=recovery.name,
    )


def _yaml_scalar(value: str) -> object:
    value = value.strip()
    if not value:
        return ""
    if value[0:1] == value[-1:] and value.startswith(("'", '"')):
        return value[1:-1]
    lowered = value.lower()
    if lowered in {"true", "yes"}:
        return True
    if lowered in {"false", "no"}:
        return False
    if lowered in {"null", "~"}:
        return None
    try:
        return int(value, 0)
    except ValueError:
        if re.fullmatch(r"[+-]?\d+\.\d+", value):
            return float(value)
        return value


def _read_environment_yaml_minimal(path: Path) -> dict[str, object]:
    """Read setup-critical fields when the project Python environment is absent."""
    data: dict[str, object] = {}
    section_name: str | None = None
    for line_number, original in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        content = original.split("#", 1)[0].rstrip()
        if not content.strip():
            continue
        indentation = len(content) - len(content.lstrip())
        if indentation and section_name != "native":
            # Setup needs only top-level identity and board selection. Other
            # nested feature data is validated by PyYAML during normal builds.
            continue
        key, separator, value = content.strip().partition(":")
        if not separator or not key:
            raise CatalogError(f"{path.relative_to(ROOT)}:{line_number}: expected KEY: VALUE")
        if indentation:
            section = data.get("native")
            if not isinstance(section, dict):
                raise CatalogError(
                    f"{path.relative_to(ROOT)}:{line_number}: unexpected nested setting"
                )
            section[key] = _yaml_scalar(value)
            continue
        section_name = None
        if value.strip():
            data[key] = _yaml_scalar(value)
        else:
            section_name = key
            data[key] = {}
    return data


def _read_environment_yaml(path: Path) -> dict[str, object]:
    try:
        import yaml
    except ModuleNotFoundError:
        return _read_environment_yaml_minimal(path)

    class UniqueKeyLoader(yaml.SafeLoader):
        """Reject repeated keys, which PyYAML otherwise resolves by keeping the last."""

        def construct_mapping(self, node, deep=False):
            seen = set()
            for key_node, _ in node.value:
                key = self.construct_object(key_node, deep=deep)
                if key in seen:
                    raise CatalogError(
                        f"{path.relative_to(ROOT)}:{key_node.start_mark.line + 1}: "
                        f"duplicate key '{key}'"
                    )
                seen.add(key)
            return super().construct_mapping(node, deep=deep)

    data = yaml.load(path.read_text(encoding="utf-8"), Loader=UniqueKeyLoader) or {}
    if not isinstance(data, dict):
        raise CatalogError(f"{path.relative_to(ROOT)} must contain a YAML mapping")
    return data


def _read_json(path: Path) -> dict[str, object]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CatalogError(f"cannot read {path.relative_to(ROOT)}: {error}") from error
    if not isinstance(data, dict):
        raise CatalogError(f"{path.relative_to(ROOT)} must contain a JSON object")
    return data


def _catalog_identity(path: Path, root: Path) -> tuple[str, str, str]:
    try:
        relative = path.relative_to(root)
    except ValueError as error:
        raise CatalogError(f"catalog entry is outside {root.relative_to(ROOT)}: {path}") from error
    if len(relative.parts) != 2 or relative.suffix != ".json":
        raise CatalogError(f"{path.relative_to(ROOT)} must be <platform>/<name>.json")
    platform_directory, filename = relative.parts
    if platform_directory not in DIRECTORY_PLATFORMS:
        raise CatalogError(f"{path.relative_to(ROOT)} uses an unknown platform directory")
    name = Path(filename).stem
    if not TARGET_ID.fullmatch(name):
        raise CatalogError(f"{path.relative_to(ROOT)} filename must use lowercase kebab-case")
    return DIRECTORY_PLATFORMS[platform_directory], platform_directory, name


def _solidify(data: dict[str, object], path: Path) -> tuple[str, ...]:
    """Return the checked `solidify` file list of one environment."""

    try:
        return solidify_list(data.get("solidify"), str(path.relative_to(ROOT)))
    except BuildError as error:
        raise CatalogError(str(error)) from error


def _filesystem(data: dict[str, object], path: Path) -> tuple[FactoryFile, ...]:
    """Return the checked `filesystem` entries of one environment."""

    try:
        return filesystem_list(data.get("filesystem"), str(path.relative_to(ROOT)))
    except BuildError as error:
        raise CatalogError(str(error)) from error


def _unknown_fields(data: dict[str, object], allowed: set[str], path: Path) -> None:
    unknown = sorted(set(data) - allowed)
    if unknown:
        raise CatalogError(f"{path.relative_to(ROOT)} has unknown fields: {', '.join(unknown)}")


def _derive_capabilities(
    platform_directory: str, sdk_target: str, overrides: dict[str, bool], sdk_key: str = "esp-idf"
) -> dict[str, bool | None]:
    capabilities: dict[str, bool | None] = {name: None for name in CAPABILITIES}
    if platform_directory == "esp-idf":
        from newmota.host import load_sdk_version

        header = (
            load_sdk_version(sdk_key).source
            / "components"
            / "soc"
            / sdk_target
            / "include"
            / "soc"
            / "soc_caps.h"
        )
        if header.is_file():
            content = header.read_text(encoding="utf-8", errors="replace")
            defined = set(re.findall(r"^#define\s+(SOC_[A-Z0-9_]+)\b", content, re.MULTILINE))
            capabilities.update(
                {name: macro in defined for name, macro in ESP_IDF_CAPABILITY_MACROS.items()}
            )
    capabilities.update(overrides)
    return capabilities


def load_chip_definition(path: Path) -> ChipDefinition:
    platform, platform_directory, name = _catalog_identity(path, CHIP_DIR)
    data = _read_json(path)
    _unknown_fields(data, CHIP_FIELDS, path)
    sdk_target = data.get("sdk_target")
    if not isinstance(sdk_target, str) or not SDK_ID.fullmatch(sdk_target):
        raise CatalogError(f"{path.relative_to(ROOT)} requires a valid vendor sdk_target")
    build_status = data.get("build_status", "native")
    if build_status not in {"native", "planned"}:
        raise CatalogError(f"{path.relative_to(ROOT)} build_status must be 'native' or 'planned'")
    idf_version = _idf_version(data, platform, path)
    raw_overrides = data.get("capability_overrides", {})
    if not isinstance(raw_overrides, dict):
        raise CatalogError(f"{path.relative_to(ROOT)} capability_overrides must be an object")
    overrides: dict[str, bool] = {}
    for capability, value in raw_overrides.items():
        if capability not in CAPABILITIES:
            raise CatalogError(
                f"{path.relative_to(ROOT)} has unknown capability override '{capability}'"
            )
        if not isinstance(value, bool):
            raise CatalogError(
                f"{path.relative_to(ROOT)} capability override '{capability}' must be boolean"
            )
        overrides[capability] = value
    return ChipDefinition(
        name=name,
        platform=platform,
        platform_directory=platform_directory,
        sdk_target=sdk_target,
        build_status=str(build_status),
        capability_overrides=overrides,
        capabilities=_derive_capabilities(
            platform_directory, sdk_target, overrides, esp_sdk_key(idf_version)
        ),
        idf_version=idf_version,
        path=path,
    )


def _memory_definition(
    value: object, path: Path, field: str, *, require_size: bool
) -> MemoryDefinition:
    if not isinstance(value, dict):
        raise CatalogError(f"{path.relative_to(ROOT)} {field} must be an object")
    _unknown_fields(value, MEMORY_FIELDS, path)
    size_kb = value.get("size_kb")
    if require_size and (not isinstance(size_kb, int) or isinstance(size_kb, bool)):
        raise CatalogError(f"{path.relative_to(ROOT)} {field}.size_kb is required")
    if size_kb is not None and (
        not isinstance(size_kb, int) or isinstance(size_kb, bool) or size_kb <= 0
    ):
        raise CatalogError(f"{path.relative_to(ROOT)} {field}.size_kb must be positive")
    mode = value.get("mode")
    if mode is not None and (not isinstance(mode, str) or not TARGET_ID.fullmatch(mode)):
        raise CatalogError(f"{path.relative_to(ROOT)} {field}.mode must use lowercase kebab-case")
    frequency = value.get("frequency_mhz")
    if frequency is not None and (
        not isinstance(frequency, int) or isinstance(frequency, bool) or frequency <= 0
    ):
        raise CatalogError(f"{path.relative_to(ROOT)} {field}.frequency_mhz must be positive")
    return MemoryDefinition(size_kb=size_kb, mode=mode, frequency_mhz=frequency)


def _pin_list(value: object, path: Path, field: str) -> tuple[int, ...]:
    """Validate one duplicate-free, ascending physical-pin list."""

    if not isinstance(value, list) or any(
        not isinstance(pin, int) or isinstance(pin, bool) or pin < 0 or pin > 255 for pin in value
    ):
        raise CatalogError(f"{path.relative_to(ROOT)} {field} must be a physical-pin array")
    pins = tuple(value)
    if tuple(sorted(set(pins))) != pins:
        raise CatalogError(
            f"{path.relative_to(ROOT)} {field} must be strictly ascending without duplicates"
        )
    return pins


def _board_gpio_definition(value: object, path: Path) -> BoardGpioDefinition:
    """Load optional board/package GPIO exposure and fixed reservations."""

    if value is None:
        return BoardGpioDefinition(None, None, ())
    if not isinstance(value, dict):
        raise CatalogError(f"{path.relative_to(ROOT)} gpio must be an object")
    _unknown_fields(value, BOARD_GPIO_FIELDS, path)
    source = value.get("source")
    if source is not None and (
        not isinstance(source, str)
        or not source.startswith("https://")
        or any(character.isspace() for character in source)
        or len(source) > 512
    ):
        raise CatalogError(f"{path.relative_to(ROOT)} gpio.source must be an HTTPS URL")
    raw_exposed = value.get("exposed")
    exposed = None if raw_exposed is None else _pin_list(raw_exposed, path, "gpio.exposed")
    if exposed == ():
        raise CatalogError(f"{path.relative_to(ROOT)} gpio.exposed cannot be empty")
    raw_reserved = value.get("reserved", {})
    if not isinstance(raw_reserved, dict):
        raise CatalogError(f"{path.relative_to(ROOT)} gpio.reserved must be an object")
    reservations: list[tuple[str, tuple[int, ...]]] = []
    for owner, pins in sorted(raw_reserved.items()):
        if not isinstance(owner, str) or not TARGET_ID.fullmatch(owner):
            raise CatalogError(
                f"{path.relative_to(ROOT)} gpio.reserved owner must use lowercase kebab-case"
            )
        resolved_pins = _pin_list(pins, path, f"gpio.reserved.{owner}")
        if not resolved_pins:
            raise CatalogError(f"{path.relative_to(ROOT)} gpio.reserved.{owner} cannot be empty")
        reservations.append((owner, resolved_pins))
    if exposed is None and not reservations:
        raise CatalogError(f"{path.relative_to(ROOT)} gpio must define exposed or reserved pins")
    if source is None:
        raise CatalogError(f"{path.relative_to(ROOT)} gpio facts require gpio.source provenance")
    return BoardGpioDefinition(source, exposed, tuple(reservations))


def load_board_definition(path: Path) -> BoardDefinition:
    platform, platform_directory, name = _catalog_identity(path, BOARD_DIR)
    data = _read_json(path)
    _unknown_fields(data, BOARD_FIELDS, path)
    chip_name = data.get("chip")
    if not isinstance(chip_name, str) or not TARGET_ID.fullmatch(chip_name):
        raise CatalogError(f"{path.relative_to(ROOT)} requires a lowercase chip")
    chip_path = CHIP_DIR / platform_directory / f"{chip_name}.json"
    if not chip_path.is_file():
        raise CatalogError(
            f"{path.relative_to(ROOT)} references missing chip {chip_path.relative_to(ROOT)}"
        )
    chip = load_chip_definition(chip_path)
    flash = _memory_definition(data.get("flash"), path, "flash", require_size=True)
    if flash.size_kb is None or flash.size_kb % 1024:
        raise CatalogError(f"{path.relative_to(ROOT)} flash.size_kb must be a whole MiB")
    if flash.size_kb < 2048:
        raise CatalogError(f"{path.relative_to(ROOT)} has less than the supported 2 MiB flash")
    raw_psram = data.get("psram")
    psram = (
        None
        if raw_psram is None
        else _memory_definition(raw_psram, path, "psram", require_size=False)
    )
    console = data.get("console")
    if console is not None and console not in CONSOLES:
        raise CatalogError(
            f"{path.relative_to(ROOT)} console must be one of {', '.join(sorted(CONSOLES))}"
        )
    sdk_board = data.get("sdk_board")
    if sdk_board is not None and (
        not isinstance(sdk_board, str) or not SDK_ID.fullmatch(sdk_board)
    ):
        raise CatalogError(f"{path.relative_to(ROOT)} sdk_board is not a valid vendor identifier")
    profile = data.get("profile_default", "Standard")
    if profile not in PROFILES:
        raise CatalogError(f"{path.relative_to(ROOT)} has invalid profile_default '{profile}'")
    revision = data.get("revision")
    if revision is not None and (
        not isinstance(revision, int) or isinstance(revision, bool) or revision < 0
    ):
        raise CatalogError(f"{path.relative_to(ROOT)} revision must be a non-negative integer")
    cpu_cores = data.get("cpu_cores")
    if cpu_cores is not None and (
        not isinstance(cpu_cores, int) or isinstance(cpu_cores, bool) or cpu_cores <= 0
    ):
        raise CatalogError(f"{path.relative_to(ROOT)} cpu_cores must be a positive integer")
    gpio = _board_gpio_definition(data.get("gpio"), path)
    # A vendor flash map, such as the Tuya one, selects `<size>-<chip>-<layout>`.
    flash_layout = data.get("flash_layout")
    if flash_layout is not None and (
        not isinstance(flash_layout, str) or not TARGET_ID.fullmatch(flash_layout)
    ):
        raise CatalogError(f"{path.relative_to(ROOT)} flash_layout must use lowercase kebab-case")
    raw_capability_overrides = data.get("capability_overrides", {})
    if not isinstance(raw_capability_overrides, dict):
        raise CatalogError(f"{path.relative_to(ROOT)} capability_overrides must be an object")
    capability_overrides: dict[str, bool] = {}
    for capability, value in raw_capability_overrides.items():
        if capability not in CAPABILITIES:
            raise CatalogError(
                f"{path.relative_to(ROOT)} has unknown capability override '{capability}'"
            )
        if not isinstance(value, bool):
            raise CatalogError(
                f"{path.relative_to(ROOT)} capability override '{capability}' must be boolean"
            )
        capability_overrides[capability] = value
    if platform_directory == "esp-idf":
        if flash.mode is None or flash.frequency_mhz is None:
            raise CatalogError(
                f"{path.relative_to(ROOT)} ESP-IDF flash requires mode and frequency_mhz"
            )
        if console is None:
            raise CatalogError(f"{path.relative_to(ROOT)} ESP-IDF console is required")
        if sdk_board is not None:
            raise CatalogError(f"{path.relative_to(ROOT)} ESP-IDF does not use sdk_board")
        if psram is not None and psram.mode is None:
            raise CatalogError(f"{path.relative_to(ROOT)} ESP-IDF psram.mode is required")
        if cpu_cores is not None and cpu_cores != 1:
            raise CatalogError(
                f"{path.relative_to(ROOT)} cpu_cores is only used to select a single core"
            )
    elif platform_directory == "pico-sdk":
        if flash.mode is not None or flash.frequency_mhz is not None:
            raise CatalogError(
                f"{path.relative_to(ROOT)} Pico SDK flash bus settings come from sdk_board"
            )
        if psram is not None:
            raise CatalogError(f"{path.relative_to(ROOT)} Pico SDK PSRAM is not integrated")
        if console != "usb-cdc":
            raise CatalogError(f"{path.relative_to(ROOT)} Pico SDK console must be usb-cdc")
        if sdk_board is None:
            raise CatalogError(f"{path.relative_to(ROOT)} Pico SDK requires sdk_board")
        if revision is not None or cpu_cores is not None:
            raise CatalogError(f"{path.relative_to(ROOT)} has unsupported Pico SDK board settings")
    else:
        if flash.mode is not None or flash.frequency_mhz is not None:
            raise CatalogError(
                f"{path.relative_to(ROOT)} memory bus settings are not integrated for "
                f"{platform_directory}"
            )
        if psram is not None and (psram.mode is not None or psram.frequency_mhz is not None):
            raise CatalogError(
                f"{path.relative_to(ROOT)} PSRAM bus settings are not integrated for "
                f"{platform_directory}"
            )
        if console is not None or revision is not None or cpu_cores is not None:
            raise CatalogError(f"{path.relative_to(ROOT)} has ESP-IDF-only board settings")
        if platform_directory in {"bouffalo", "amebad"} and sdk_board is None:
            raise CatalogError(f"{path.relative_to(ROOT)} requires sdk_board")
        if platform_directory == "beken" and sdk_board is not None:
            raise CatalogError(f"{path.relative_to(ROOT)} Beken does not use sdk_board")
    return BoardDefinition(
        name=name,
        platform=platform,
        platform_directory=platform_directory,
        chip=chip,
        flash=flash,
        psram=psram,
        console=console,
        sdk_board=sdk_board,
        profile_default=str(profile),
        revision=revision,
        gpio=gpio,
        flash_layout=flash_layout,
        capability_overrides=capability_overrides,
        capabilities={**chip.capabilities, **capability_overrides},
        data=data,
        path=path,
    )


def load_chips() -> dict[str, ChipDefinition]:
    chips: dict[str, ChipDefinition] = {}
    for path in sorted(CHIP_DIR.glob("*/*.json")):
        item = load_chip_definition(path)
        key = f"{item.platform_directory}/{item.name}"
        chips[key] = item
    return chips


def load_boards() -> dict[str, BoardDefinition]:
    boards: dict[str, BoardDefinition] = {}
    names: dict[str, Path] = {}
    for path in sorted(BOARD_DIR.glob("*/*.json")):
        item = load_board_definition(path)
        if item.name in names:
            raise CatalogError(
                f"board name '{item.name}' is duplicated by "
                f"{names[item.name].relative_to(ROOT)} and {path.relative_to(ROOT)}"
            )
        names[item.name] = path
        boards[f"{item.platform_directory}/{item.name}"] = item
    return boards


def _environment_location(path: Path) -> tuple[str, str, str]:
    try:
        relative = path.relative_to(ENV_DIR)
    except ValueError as error:
        raise CatalogError(f"environment is outside {ENV_DIR.relative_to(ROOT)}: {path}") from error
    if len(relative.parts) != 3 or relative.suffix != ".yaml":
        raise CatalogError(f"{path.relative_to(ROOT)} must be <platform>/<role>/<environment>.yaml")
    platform_directory, role, _ = relative.parts
    if platform_directory not in DIRECTORY_PLATFORMS:
        raise CatalogError(f"{path.relative_to(ROOT)} uses an unknown platform directory")
    if role not in ENVIRONMENT_ROLES:
        raise CatalogError(
            f"{path.relative_to(ROOT)} role directory must be one of "
            + ", ".join(sorted(ENVIRONMENT_ROLES))
        )
    return DIRECTORY_PLATFORMS[platform_directory], platform_directory, role


def _default_partition(
    platform_directory: str,
    flash_kb: int,
    source: Path,
    sdk_target: str | None = None,
    flash_layout: str | None = None,
    build_status: str = "native",
) -> Path | None:
    """Select a platform-native default from the board's flash capacity."""

    if flash_kb <= 0 or flash_kb % 1024:
        raise CatalogError(f"{source.relative_to(ROOT)} flash_kb must be a whole MiB value")
    directory = PARTITIONS_DIR / platform_directory
    if not directory.is_dir():
        return None
    flash_name = f"{flash_kb // 1024}m"
    if flash_layout is not None:
        if sdk_target is None:
            raise CatalogError(f"{source.relative_to(ROOT)} flash_layout needs an SDK target")
        named = sorted(directory.glob(f"{flash_name}-{sdk_target}-{flash_layout}.*"))
        if not named and build_status == "planned":
            return None
        if len(named) != 1:
            raise CatalogError(
                f"{source.relative_to(ROOT)} flash_layout '{flash_layout}' needs exactly one "
                f"{flash_name}-{sdk_target}-{flash_layout} layout in "
                f"{directory.relative_to(ROOT)}"
            )
        return named[0]
    # RP2350 layout names identify the chip, independent of the SDK's
    # architecture/security suffix (for example rp2350-arm-s).
    if (
        platform_directory == "pico-sdk"
        and sdk_target is not None
        and (sdk_target == "rp2350" or sdk_target.startswith("rp2350-"))
    ):
        sdk_target = "rp2350"
    if sdk_target is not None:
        target_matches = sorted(
            path for path in directory.glob(f"{flash_name}-{sdk_target}.*") if path.is_file()
        )
        if len(target_matches) > 1:
            raise CatalogError(
                f"{directory.relative_to(ROOT)} has multiple {flash_name} MiB defaults for "
                f"{sdk_target}: " + ", ".join(path.name for path in target_matches)
            )
        if target_matches:
            return target_matches[0]
    # Beken layouts depend on the chip and its bootloader, so there is no generic one.
    # A planned chip only fails once it is built.
    if platform_directory == "beken":
        if build_status == "planned":
            return None
        raise CatalogError(
            f"{source.relative_to(ROOT)} has no {flash_name}-{sdk_target} layout in "
            f"{directory.relative_to(ROOT)}"
        )
    matches = sorted(path for path in directory.glob(f"{flash_name}.*") if path.is_file())
    if not matches:
        raise CatalogError(
            f"{source.relative_to(ROOT)} has no {flash_kb // 1024} MiB default in "
            f"{directory.relative_to(ROOT)}"
        )
    if len(matches) > 1:
        raise CatalogError(
            f"{directory.relative_to(ROOT)} has multiple {flash_kb // 1024} MiB defaults: "
            + ", ".join(path.name for path in matches)
        )
    return matches[0]


def load_environment(
    path: Path,
    *,
    data: dict[str, object] | None = None,
    location: tuple[str, str, str] | None = None,
    base_environment: str | None = None,
) -> Environment:
    data = _read_environment_yaml(path) if data is None else data
    _unknown_fields(data, ENVIRONMENT_FIELDS, path)
    name = data.get("name", path.stem)
    board_name = data.get("board")
    if not isinstance(name, str) or not isinstance(board_name, str):
        raise CatalogError(f"{path.relative_to(ROOT)} requires string name and board fields")
    if name != path.stem:
        raise CatalogError(f"{path.relative_to(ROOT)} name must match its filename ('{path.stem}')")
    if not TARGET_ID.fullmatch(name) or not TARGET_ID.fullmatch(board_name):
        raise CatalogError(
            f"{path.relative_to(ROOT)} environment and board IDs must use lowercase kebab-case"
        )

    platform, platform_directory, directory_role = location or _environment_location(path)
    board_path = BOARD_DIR / platform_directory / f"{board_name}.json"
    if not board_path.is_file():
        raise CatalogError(
            f"{path.relative_to(ROOT)} references missing board {board_path.relative_to(ROOT)}"
        )
    board = load_board_definition(board_path)
    if board.name != board_name:
        raise CatalogError(f"{board_path.relative_to(ROOT)} identity does not match '{board_name}'")

    idf_version = _idf_version(data, platform, path, board.chip.idf_version)
    capabilities = {
        **_derive_capabilities(
            platform_directory,
            board.chip.sdk_target,
            board.chip.capability_overrides,
            esp_sdk_key(idf_version),
        ),
        **board.capability_overrides,
    }
    native = data.get("native", {})
    if native is None:
        native = {}
    if not isinstance(native, dict):
        raise CatalogError(f"{path.relative_to(ROOT)} native settings must be a mapping")
    role = data.get("role")
    if role not in ENVIRONMENT_ROLES:
        raise CatalogError(
            f"{path.relative_to(ROOT)} role must be one of " + ", ".join(sorted(ENVIRONMENT_ROLES))
        )
    if role != directory_role:
        raise CatalogError(
            f"{path.relative_to(ROOT)} role '{role}' does not match its '{directory_role}' directory"
        )
    policy = data.get("policy", False)
    if not isinstance(policy, bool):
        raise CatalogError(f"{path.relative_to(ROOT)} policy must be boolean")
    if policy and role != "application":
        raise CatalogError(
            f"{path.relative_to(ROOT)} policy is only valid for application environments"
        )
    profile = data.get("profile", board.profile_default)
    if profile not in PROFILES:
        raise CatalogError(f"{path.relative_to(ROOT)} has invalid profile '{profile}'")
    if role == "safeboot" and profile != "Safeboot":
        raise CatalogError(f"{path.relative_to(ROOT)} safeboot role requires profile 'Safeboot'")
    if role == "application" and profile == "Safeboot":
        raise CatalogError(
            f"{path.relative_to(ROOT)} application role cannot use profile 'Safeboot'"
        )
    safeboot = data.get("safeboot")
    if safeboot is not None and (
        not isinstance(safeboot, str) or not TARGET_ID.fullmatch(safeboot)
    ):
        raise CatalogError(f"{path.relative_to(ROOT)} safeboot must be a lowercase environment ID")
    if role == "safeboot" and safeboot is not None:
        raise CatalogError(
            f"{path.relative_to(ROOT)} safeboot role cannot reference another safeboot"
        )
    language_value = data.get("language", DEFAULT_LANGUAGE)
    if not isinstance(language_value, str):
        raise CatalogError(f"{path.relative_to(ROOT)} has invalid language '{language_value}'")
    try:
        language = canonical_locale_id(language_value)
        load_locale(language)
    except I18nError as error:
        raise CatalogError(
            f"{path.relative_to(ROOT)} has invalid language '{language_value}': {error}"
        ) from error
    item = Environment(
        name=name,
        board=board_name,
        policy=policy,
        chip=board.chip.name,
        sdk_target=board.chip.sdk_target,
        build_status=board.chip.build_status,
        sdk_board=board.sdk_board,
        role=str(role),
        profile=str(profile),
        safeboot=str(safeboot) if safeboot is not None else None,
        language=language,
        flash_kb=board.flash.size_kb or 0,
        psram_kb=board.psram.size_kb if board.psram else None,
        console=board.console,
        gpio=board.gpio,
        capabilities=capabilities,
        idf_version=idf_version,
        idf_version_source=str(
            (path if "idf_version" in data else board.chip.path).relative_to(ROOT)
        )
        if platform == "esp"
        else None,
        platform=platform,
        platform_directory=platform_directory,
        board_data=board.data,
        native=native,
        crypto=data.get("crypto"),
        features=data.get("features", {}),
        device_name=_device_name(data, role, path),
        template=data.get("template"),
        solidify=_solidify(data, path),
        filesystem=_filesystem(data, path),
        partition=_default_partition(
            platform_directory,
            board.flash.size_kb or 0,
            board_path,
            board.chip.sdk_target,
            board.flash_layout,
            board.chip.build_status,
        ),
        path=path,
        configuration_paths=(path,),
        base_environment=base_environment,
    )
    from newmota.features import feature_values, personal_settings

    try:
        selected = feature_values(item)
    except BuildError as error:
        raise CatalogError(f"{path.relative_to(ROOT)}: {error}") from error
    if USER_CONFIG_DIR not in path.parents and (personal := personal_settings(item.features)):
        raise CatalogError(
            f"{path.relative_to(ROOT)}: {', '.join(personal)} belongs in user_config/"
        )
    if item.template is not None:
        from newmota.hardware import check_template

        if USER_CONFIG_DIR not in path.parents:
            raise CatalogError(f"{path.relative_to(ROOT)}: template belongs in user_config/")
        check_template(item, selected)
    if item.solidify:
        if USER_CONFIG_DIR not in path.parents:
            raise CatalogError(f"{path.relative_to(ROOT)}: solidify belongs in user_config/")
        try:
            check_solidify(item.solidify, selected)
        except BuildError as error:
            raise CatalogError(f"{path.relative_to(ROOT)}: {error}") from error
    if item.filesystem:
        if USER_CONFIG_DIR not in path.parents:
            raise CatalogError(f"{path.relative_to(ROOT)}: filesystem belongs in user_config/")
        try:
            check_filesystem(item.filesystem, item.solidify, selected)
        except BuildError as error:
            raise CatalogError(f"{path.relative_to(ROOT)}: {error}") from error
    return item


def load_environments() -> dict[str, Environment]:
    environments: dict[str, Environment] = {}
    for path in sorted(ENV_DIR.glob("*/*/*.yaml")):
        item = load_environment(path)
        if item.name in environments:
            raise CatalogError(f"environment '{item.name}' is defined more than once")
        environments[item.name] = item
    for item in environments.values():
        if item.safeboot is None:
            continue
        recovery = environments.get(item.safeboot)
        if recovery is None:
            raise CatalogError(
                f"{item.path.relative_to(ROOT) if item.path else item.name} references unknown "
                f"safeboot environment '{item.safeboot}'"
            )
        if recovery.role != "safeboot":
            raise CatalogError(
                f"{item.name} references '{recovery.name}', which is not a safeboot environment"
            )
        if recovery.platform != item.platform or recovery.sdk_target != item.sdk_target:
            raise CatalogError(
                f"{item.name} and safeboot environment '{recovery.name}' must use the same "
                "platform and SDK target"
            )
        if recovery.board != item.board:
            raise CatalogError(
                f"{item.name} and safeboot environment '{recovery.name}' must initially use "
                "the same verified board recipe"
            )
    policies: dict[tuple[str, str], list[Environment]] = {}
    for item in environments.values():
        if item.policy:
            policies.setdefault((item.platform_directory, item.role), []).append(item)
    for (platform_directory, role), candidates in policies.items():
        if len(candidates) > 1:
            names = ", ".join(sorted(item.name for item in candidates))
            raise CatalogError(
                f"{platform_directory}/{role} has multiple policy environments: {names}"
            )
    return environments


def load_environment_matrix(name: str) -> list[str]:
    """Load an ordered, named selection of existing environments."""

    if not TARGET_ID.fullmatch(name):
        raise CatalogError(f"invalid environment matrix name '{name}'")
    path = ENV_MATRIX_DIR / f"{name}.txt"
    if not path.is_file():
        available = ", ".join(sorted(item.stem for item in ENV_MATRIX_DIR.glob("*.txt")))
        suffix = f" (available: {available})" if available else ""
        raise CatalogError(f"unknown environment matrix '{name}'{suffix}")

    names: list[str] = []
    seen: set[str] = set()
    for line_number, original in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        environment = original.split("#", 1)[0].strip()
        if not environment:
            continue
        if not TARGET_ID.fullmatch(environment):
            raise CatalogError(
                f"{path.relative_to(ROOT)}:{line_number}: invalid environment '{environment}'"
            )
        if environment in seen:
            raise CatalogError(
                f"{path.relative_to(ROOT)}:{line_number}: duplicate environment '{environment}'"
            )
        seen.add(environment)
        names.append(environment)
    if not names:
        raise CatalogError(f"{path.relative_to(ROOT)} contains no environments")
    return names


USER_FIELDS = {
    "name",
    "inherit",
    "profile",
    "language",
    "native",
    "crypto",
    "features",
    "idf_version",
    "device_name",
    "template",
    "solidify",
    "filesystem",
}


def user_reserved_names(official: dict[str, Environment]) -> set[str]:
    """Names a personal environment may not take."""

    return set(official) | {board.name for board in load_boards().values()} | MAKE_TARGETS


def resolve_user_environment(
    path: Path, child: dict[str, object], official: dict[str, Environment], reserved: set[str]
) -> Environment:
    """Validate one personal configuration mapping against its checked-in base."""

    _unknown_fields(child, USER_FIELDS, path)
    name, parent = child.get("name"), child.get("inherit")
    source = str(path.relative_to(ROOT))
    if not isinstance(name, str) or not TARGET_ID.fullmatch(name) or name != path.stem:
        raise CatalogError(f"{source} requires a lowercase kebab-case name matching its filename")
    if name in reserved:
        raise CatalogError(f"{source} name '{name}' conflicts with an existing target")
    if not isinstance(parent, str) or parent not in official:
        raise CatalogError(f"{source} inherit must name a checked-in application environment")
    base = official[parent]
    if base.role != "application":
        raise CatalogError(f"{source} inherit must name an application environment")
    if "crypto" in child and child["crypto"] is None:
        raise CatalogError(f"{source} crypto must be omitted or select a build-time profile")
    native = child.get("native", {})
    if not isinstance(native, dict):
        raise CatalogError(f"{source} native settings must be a mapping")
    features = child.get("features", {})
    if not isinstance(features, dict):
        raise CatalogError(f"{source} features must be a mapping")
    solidify = child.get("solidify", [])
    if not isinstance(solidify, list):
        raise CatalogError(f"{source} solidify must be a list of file paths")
    filesystem = child.get("filesystem", [])
    if not isinstance(filesystem, list):
        raise CatalogError(f"{source} filesystem must be a list of files")
    data = {
        "name": name,
        "board": base.board,
        "role": base.role,
        "profile": child.get("profile", base.profile),
        "language": child.get("language", base.language),
        "safeboot": base.safeboot,
        "native": {**base.native, **native},
        "crypto": child.get("crypto", base.crypto),
        "features": {**base.features, **features},
        "device_name": child.get("device_name", base.device_name),
        "template": child.get("template"),
        "solidify": [*base.solidify, *solidify],
        "filesystem": [
            *({"source": entry.source, "target": entry.target} for entry in base.filesystem),
            *filesystem,
        ],
    }
    if base.platform == "esp":
        data["idf_version"] = child.get("idf_version", base.idf_version)
    elif "idf_version" in child:
        raise CatalogError(f"{source} idf_version is only valid for ESP-IDF")
    item = load_environment(
        path,
        data=data,
        location=(base.platform, base.platform_directory, base.role),
        base_environment=parent,
    )
    return replace(
        item,
        base_environment=parent,
        configuration_paths=base.configuration_paths + (path,),
        idf_version_source=item.idf_version_source
        if "idf_version" in child
        else base.idf_version_source,
    )


def load_user_environments(
    official: dict[str, Environment] | None = None,
) -> dict[str, Environment]:
    """Resolve explicitly selected personal builds from checked-in applications."""

    official = load_environments() if official is None else official
    reserved = user_reserved_names(official)
    result: dict[str, Environment] = {}
    for path in sorted(USER_CONFIG_DIR.glob("*.yaml")):
        item = resolve_user_environment(
            path, _read_environment_yaml(path), official, reserved | set(result)
        )
        result[item.name] = item
    return result


def resolve_environment(name: str) -> Environment:
    if not TARGET_ID.fullmatch(name):
        raise CatalogError(f"invalid environment name '{name}'; use lowercase kebab-case")
    environments = load_environments()
    environments.update(load_user_environments(environments))
    try:
        return environments[name]
    except KeyError as error:
        raise CatalogError(
            f"unknown environment '{name}' (available: {', '.join(environments)})"
        ) from error


def resolve_board_environment(name: str) -> Environment:
    """Resolve a board through its environment or an explicit platform fallback."""

    boards = load_boards()
    if "/" in name:
        key = name
    else:
        matches = [key for key in boards if key.endswith(f"/{name}")]
        if not matches:
            raise CatalogError(f"unknown board '{name}'")
        if len(matches) > 1:
            raise CatalogError(f"board '{name}' is ambiguous; use <platform>/{name}")
        key = matches[0]
    try:
        board = boards[key]
    except KeyError as error:
        raise CatalogError(f"unknown board '{name}'") from error

    environments = load_environments()
    exact = environments.get(board.name)
    if exact is not None:
        if (
            exact.platform_directory != board.platform_directory
            or exact.board != board.name
            or exact.role != "application"
        ):
            raise CatalogError(
                f"environment '{board.name}' is not the application environment for board "
                f"{board.platform_directory}/{board.name}"
            )
        return exact

    policies = [
        item
        for item in environments.values()
        if item.platform_directory == board.platform_directory
        and item.role == "application"
        and item.policy
    ]
    if len(policies) != 1:
        raise CatalogError(
            f"board '{board.name}' has no same-named environment and "
            f"{board.platform_directory} has {len(policies)} policy environments"
        )
    policy = policies[0]
    return Environment(
        name=board.name,
        board=board.name,
        policy=False,
        chip=board.chip.name,
        sdk_target=board.chip.sdk_target,
        build_status=board.chip.build_status,
        sdk_board=board.sdk_board,
        role="application",
        profile=board.profile_default,
        safeboot=None,
        language=policy.language,
        flash_kb=board.flash.size_kb or 0,
        psram_kb=board.psram.size_kb if board.psram else None,
        console=board.console,
        gpio=board.gpio,
        capabilities=board.capabilities,
        platform=board.platform,
        platform_directory=board.platform_directory,
        board_data=board.data,
        native=policy.native,
        partition=_default_partition(
            board.platform_directory,
            board.flash.size_kb or 0,
            board.path,
            board.chip.sdk_target,
            board.flash_layout,
            board.chip.build_status,
        ),
        path=None,
        idf_version=board.chip.idf_version,
        idf_version_source=str(board.chip.path.relative_to(ROOT))
        if board.platform == "esp"
        else None,
    )


def chips_for_platform(platform: str) -> list[str]:
    if platform not in PLATFORM_DIRECTORIES:
        raise CatalogError(f"unknown platform '{platform}'")
    return sorted(
        {item.sdk_target for item in load_environments().values() if item.platform == platform}
    )


def project_version() -> str:
    pattern = re.compile(r'^version\s*=\s*"([^"]+)"\s*$')
    for line in (ROOT / "pyproject.toml").read_text(encoding="utf-8").splitlines():
        match = pattern.fullmatch(line.strip())
        if match:
            version = match.group(1)
            if not VERSION_ID.fullmatch(version):
                raise CatalogError(
                    "project version must contain only lowercase letters, digits, dots, and hyphens"
                )
            return version
    raise CatalogError("pyproject.toml has no project version")


MAKE_TARGETS = {
    "all",
    "backup",
    "build",
    "clangd",
    "clean",
    "config",
    "flash",
    "flash-monitor",
    "flash-only",
    "flash-only-monitor",
    "help",
    "list",
    "monitor",
    "probe",
    "sdk-check",
    "setup",
    "test",
    "test-crypto",
    "test-host",
    "update",
    "uv-setup",
    "validate-catalog",
}


@dataclass(frozen=True)
class CatalogValidation:
    errors: tuple[str, ...]
    notes: tuple[str, ...]


def validate_catalog() -> CatalogValidation:
    """Validate every entry without installing tools or compiling firmware."""

    from newmota.hardware import load_gpio_registry, load_hardware_modules
    from newmota.topology import (
        load_architecture_topologies,
        resolve_pin_topology,
        validate_module_slots,
    )

    chips = load_chips()
    boards = load_boards()
    environments = load_environments()
    test_matrix = load_environment_matrix("test")
    errors: list[str] = []
    notes: list[str] = []
    # A planned chip's missing pieces are noted once per chip, not once per board.
    planned_gaps: dict[str, tuple[set[str], set[str]]] = {}

    from newmota.configurator import SELECTABLE
    from newmota.feature_docs import validate_dependency_reasons, validate_feature_docs
    from newmota.features import FEATURE_DEPENDENCIES

    errors.extend(validate_feature_docs(set(SELECTABLE)))
    errors.extend(validate_dependency_reasons(FEATURE_DEPENDENCIES))

    try:
        gpio_registry = load_gpio_registry()
        hardware_modules = load_hardware_modules(gpio_registry)
        topologies = load_architecture_topologies()
        validate_module_slots(hardware_modules, topologies)
    except (CatalogError, OSError) as error:
        errors.append(str(error))
        topologies = {}

    unknown_test_environments = [name for name in test_matrix if name not in environments]
    if unknown_test_environments:
        errors.append(
            "test matrix references unknown environments: " + ", ".join(unknown_test_environments)
        )

    conflicting_make_targets = sorted(
        MAKE_TARGETS & ({board.name for board in boards.values()} | set(environments))
    )
    if conflicting_make_targets:
        errors.append(
            "catalog IDs conflict with fixed Make targets: " + ", ".join(conflicting_make_targets)
        )

    user_environments = load_user_environments(environments)
    for environment in (*environments.values(), *user_environments.values()):
        if environment.build_status != "native":
            errors.append(f"active environment {environment.name} is not native-buildable")
        if environment.platform == "esp":
            definition = platform_for_build(environment.platform)
            board = load_board_definition(
                BOARD_DIR / environment.platform_directory / f"{environment.board}.json"
            )
            chip = replace(
                board.chip,
                idf_version=environment.idf_version,
                capabilities=environment.capabilities,
            )
            selected_board = replace(board, chip=chip, capabilities=environment.capabilities)
            issue = definition.build.native_target_issue(selected_board)
            if issue:
                errors.append(f"{environment.name}: {issue}")
            errors.extend(
                f"{environment.name}: {issue}"
                for issue in definition.build.board_issues(selected_board)
            )
        try:
            resolve_pin_topology(
                environment,
                ROOT / "build" / "work" / environment.name / "generated" / "hardware",
                topologies=topologies,
                write_header=False,
            )
        except (CatalogError, OSError) as error:
            errors.append(f"{environment.path.relative_to(ROOT)}: {error}")

    checked_chips: set[str] = set()
    for board in boards.values():
        if board.platform_directory == "esp-idf":
            environment = environments.get(board.name)
            if (
                environment is None
                or environment.board != board.name
                or environment.role != "application"
            ):
                errors.append(
                    f"{board.path.relative_to(ROOT)} has no same-named ESP-IDF environment"
                )
        topology_key = (board.platform_directory, board.chip.name)
        chip_key = f"{board.platform_directory}/{board.chip.name}"
        gaps, gap_boards = planned_gaps.setdefault(chip_key, (set(), set()))
        planned = board.chip.build_status == "planned"
        if topology_key not in topologies:
            if planned:
                gaps.add("no hardware topology")
                gap_boards.add(board.name)
            else:
                errors.append(
                    f"{board.path.relative_to(ROOT)} has no hardware topology for "
                    f"{topology_key[0]}/{topology_key[1]}"
                )
        checked_chips.add(chip_key)
        definition = platform_for_build(board.platform)
        native_default = (
            ROOT
            / "config"
            / "build"
            / definition.adapter.native_config.directory
            / board.chip.name
            / definition.adapter.native_config.filename
        )
        if board.chip.build_status == "native":
            try:
                parse_native_config(native_default)
            except (BuildConfigError, OSError) as error:
                errors.append(f"{board.path.relative_to(ROOT)}: {error}")

        issue = definition.build.native_target_issue(board)
        if issue:
            if planned:
                gaps.add(issue)
                gap_boards.add(board.name)
            else:
                errors.append(f"{board.path.relative_to(ROOT)}: {issue}")

        errors.extend(
            f"{board.path.relative_to(ROOT)}: {issue}"
            for issue in definition.build.board_issues(board)
        )

        try:
            item = resolve_board_environment(f"{board.platform_directory}/{board.name}")
            definition.adapter.board_native_settings(item, f"board '{board.name}'", item.partition)
            if topology_key in topologies:
                resolve_pin_topology(
                    item,
                    ROOT / "build" / "work" / board.name / "generated" / "hardware",
                    topologies=topologies,
                    write_header=False,
                )
        except (BuildError, CatalogError, OSError, ValueError) as error:
            errors.append(f"{board.path.relative_to(ROOT)}: {error}")

    for chip_key, (gaps, gap_boards) in planned_gaps.items():
        if gaps:
            notes.append(
                f"planned {chip_key}: {'; '.join(sorted(gaps))} "
                f"(boards: {', '.join(sorted(gap_boards))})"
            )
    for key, chip in chips.items():
        if key not in checked_chips:
            notes.append(f"{chip.path.relative_to(ROOT)} has no board recipe")

    return CatalogValidation(tuple(sorted(set(errors))), tuple(sorted(set(notes))))
