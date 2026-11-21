"""Resolve declarative architecture and board GPIO topology for Newmota."""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - exercised only by Python 3.10
    import tomli as tomllib

from newmota.host import sha256, write_if_changed
from newmota.model import DIRECTORY_PLATFORMS, HARDWARE_DIR, ROOT, CatalogError, Environment

if TYPE_CHECKING:
    from newmota.hardware import HardwareModule

TOPOLOGIES_DIR = HARDWARE_DIR / "topologies"
CATALOG_ID = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*")
SDK_ID = re.compile(r"[A-Za-z0-9](?:[A-Za-z0-9_.-]*[A-Za-z0-9])?")
REVISION = re.compile(r"[0-9a-f]{40}")
PIN_CAPABILITIES = (
    "input",
    "output",
    "pull-up",
    "pull-down",
    "open-drain",
    "interrupt",
    "analog",
)
CAPABILITY_BITS = {name: 1 << index for index, name in enumerate(PIN_CAPABILITIES)}
NUMBERING = {"gpio", "amebad-port"}
FLASH_MODES = {"dio", "qio", "opi"}
PSRAM_MODES = {"quad", "octal", "hex"}
CONSOLES = {"uart0", "usb-cdc", "usb-serial-jtag"}


@dataclass(frozen=True)
class PinOverride:
    """One exception to an architecture's default physical-pin properties."""

    pin: int
    present: bool
    capabilities: tuple[str, ...] | None
    cautions: tuple[str, ...]


@dataclass(frozen=True)
class PinReservation:
    """One conditional build-time owner of physical pins."""

    owner: str
    pins: tuple[int, ...]
    flash_modes: tuple[str, ...]
    psram_modes: tuple[str, ...]
    consoles: tuple[str, ...]
    sdk_boards: tuple[str, ...]
    boards: tuple[str, ...]
    # A console that stops when a template or runtime owner takes one of its pins.
    template_may_claim: bool = False


@dataclass(frozen=True)
class ArchitectureTopology:
    """Validated topology rules for one SDK architecture target."""

    platform_directory: str
    architecture: str
    pin_count: int
    template_slots: tuple[int, ...]
    numbering: str
    default_capabilities: tuple[str, ...]
    source: dict[str, object]
    compatibility: dict[str, str] | None
    overrides: tuple[PinOverride, ...]
    reservations: tuple[PinReservation, ...]
    path: Path


@dataclass(frozen=True)
class ResolvedPin:
    """One environment-specific physical pin and its effective availability."""

    pin: int
    label: str
    template_slot: int | None
    present: bool
    exposed: bool
    available: bool
    capabilities: tuple[str, ...]
    cautions: tuple[str, ...]
    reserved_by: tuple[str, ...]
    yields_from: tuple[str, ...] = ()


@dataclass(frozen=True)
class ResolvedPinTopology:
    """Generated compact and host-readable topology for one environment."""

    identity: str
    header: Path
    document: dict[str, object]
    definition: ArchitectureTopology
    pins: tuple[ResolvedPin, ...]


def _read_toml(path: Path) -> dict[str, object]:
    """Read one topology TOML document and normalize parser failures."""

    try:
        with path.open("rb") as handle:
            data = tomllib.load(handle)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise CatalogError(f"cannot read {path.relative_to(ROOT)}: {error}") from error
    if not isinstance(data, dict):
        raise CatalogError(f"{path.relative_to(ROOT)} must contain a TOML table")
    return data


def _unknown_fields(data: dict[str, object], allowed: set[str], path: Path, location: str) -> None:
    """Reject topology fields that would otherwise be silently ignored."""

    unknown = sorted(set(data) - allowed)
    if unknown:
        raise CatalogError(
            f"{path.relative_to(ROOT)} {location} has unknown fields: {', '.join(unknown)}"
        )


def _table(value: object, path: Path, location: str) -> dict[str, object]:
    """Require one TOML table."""

    if not isinstance(value, dict):
        raise CatalogError(f"{path.relative_to(ROOT)} {location} must be a table")
    return value


def _table_array(value: object, path: Path, location: str) -> list[dict[str, object]]:
    """Require one TOML array of tables."""

    if not isinstance(value, list) or any(not isinstance(item, dict) for item in value):
        raise CatalogError(f"{path.relative_to(ROOT)} {location} must be an array of tables")
    return value


def _string(value: object, path: Path, location: str, maximum: int = 128) -> str:
    """Require one non-empty bounded string."""

    if not isinstance(value, str) or not value.strip() or len(value) > maximum:
        raise CatalogError(
            f"{path.relative_to(ROOT)} {location} must be a non-empty string of at most "
            f"{maximum} characters"
        )
    return value


def _string_list(value: object, path: Path, location: str) -> tuple[str, ...]:
    """Require one duplicate-free string list in declared order."""

    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise CatalogError(f"{path.relative_to(ROOT)} {location} must be a string array")
    result = tuple(value)
    if len(result) != len(set(result)):
        raise CatalogError(f"{path.relative_to(ROOT)} {location} contains duplicates")
    return result


def _pin_list(value: object, path: Path, location: str, pin_count: int) -> tuple[int, ...]:
    """Require one strictly ascending physical-pin list within the architecture bound."""

    if not isinstance(value, list) or any(
        not isinstance(pin, int) or isinstance(pin, bool) or pin < 0 or pin >= pin_count
        for pin in value
    ):
        raise CatalogError(
            f"{path.relative_to(ROOT)} {location} must contain pins from 0 to {pin_count - 1}"
        )
    result = tuple(value)
    if tuple(sorted(set(result))) != result:
        raise CatalogError(
            f"{path.relative_to(ROOT)} {location} must be strictly ascending without duplicates"
        )
    return result


def _slot_list(value: object, path: Path, location: str, pin_count: int) -> tuple[int, ...]:
    """Require a duplicate-free ordered Template-slot to physical-pin map."""

    if not isinstance(value, list) or any(
        not isinstance(pin, int) or isinstance(pin, bool) or pin < 0 or pin >= pin_count
        for pin in value
    ):
        raise CatalogError(
            f"{path.relative_to(ROOT)} {location} must contain pins from 0 to {pin_count - 1}"
        )
    result = tuple(value)
    if len(result) != len(set(result)):
        raise CatalogError(f"{path.relative_to(ROOT)} {location} contains duplicate pins")
    return result


def _capabilities(value: object, path: Path, location: str) -> tuple[str, ...]:
    """Validate and canonically order one physical-pin capability set."""

    values = _string_list(value, path, location)
    unknown = sorted(set(values) - set(PIN_CAPABILITIES))
    if unknown:
        raise CatalogError(
            f"{path.relative_to(ROOT)} {location} has unknown capabilities: " + ", ".join(unknown)
        )
    return tuple(name for name in PIN_CAPABILITIES if name in values)


def _topology_location(path: Path) -> tuple[str, str]:
    """Return the platform and architecture encoded by a topology path."""

    try:
        relative = path.relative_to(TOPOLOGIES_DIR)
    except ValueError as error:
        raise CatalogError(
            f"topology is outside {TOPOLOGIES_DIR.relative_to(ROOT)}: {path}"
        ) from error
    if len(relative.parts) != 2 or relative.suffix != ".toml":
        raise CatalogError(f"{path.relative_to(ROOT)} must be <platform>/<architecture>.toml")
    platform_directory, filename = relative.parts
    architecture = Path(filename).stem
    if platform_directory not in DIRECTORY_PLATFORMS:
        raise CatalogError(f"{path.relative_to(ROOT)} uses an unknown platform directory")
    if not CATALOG_ID.fullmatch(architecture):
        raise CatalogError(f"{path.relative_to(ROOT)} architecture must use lowercase kebab-case")
    return platform_directory, architecture


def _source_table(value: object, path: Path) -> dict[str, object]:
    """Validate the SDK files from which topology facts were reviewed."""

    data = _table(value, path, "source")
    _unknown_fields(data, {"project", "paths"}, path, "source")
    project = _string(data.get("project"), path, "source.project")
    paths = _string_list(data.get("paths"), path, "source.paths")
    if not paths:
        raise CatalogError(f"{path.relative_to(ROOT)} source.paths cannot be empty")
    for item in paths:
        candidate = Path(item)
        if candidate.is_absolute() or ".." in candidate.parts:
            raise CatalogError(f"{path.relative_to(ROOT)} source.paths must stay inside its SDK")
    return {"project": project, "paths": paths}


def _compatibility_table(value: object, path: Path) -> dict[str, str] | None:
    """Validate optional Tasmota slot-map provenance."""

    if value is None:
        return None
    data = _table(value, path, "compatibility")
    _unknown_fields(data, {"project", "revision", "path"}, path, "compatibility")
    project = _string(data.get("project"), path, "compatibility.project")
    revision = _string(data.get("revision"), path, "compatibility.revision")
    source_path = _string(data.get("path"), path, "compatibility.path", 256)
    if not REVISION.fullmatch(revision):
        raise CatalogError(
            f"{path.relative_to(ROOT)} compatibility.revision must be a full commit hash"
        )
    return {"project": project, "revision": revision, "path": source_path}


def _condition_values(
    data: dict[str, object],
    field: str,
    path: Path,
    location: str,
    pattern: re.Pattern[str] = CATALOG_ID,
    allowed: set[str] | None = None,
) -> tuple[str, ...]:
    """Load one optional list used by a reservation predicate."""

    values = _string_list(data.get(field, []), path, f"{location}.{field}")
    if any(not pattern.fullmatch(value) for value in values):
        raise CatalogError(f"{path.relative_to(ROOT)} {location}.{field} has invalid identifiers")
    if allowed is not None and not set(values).issubset(allowed):
        unknown = ", ".join(sorted(set(values) - allowed))
        raise CatalogError(
            f"{path.relative_to(ROOT)} {location}.{field} has unknown values: {unknown}"
        )
    return values


def load_architecture_topology(path: Path) -> ArchitectureTopology:
    """Load and strictly validate one architecture topology."""

    platform_directory, architecture_name = _topology_location(path)
    data = _read_toml(path)
    _unknown_fields(
        data,
        {"schema", "source", "compatibility", "architecture", "pin", "reservation"},
        path,
        "root",
    )
    if data.get("schema") != 1:
        raise CatalogError(f"{path.relative_to(ROOT)} schema must be 1")
    source = _source_table(data.get("source"), path)
    compatibility = _compatibility_table(data.get("compatibility"), path)

    architecture = _table(data.get("architecture"), path, "architecture")
    _unknown_fields(
        architecture,
        {"pin_count", "template_slots", "numbering", "default_capabilities"},
        path,
        "architecture",
    )
    pin_count = architecture.get("pin_count")
    if (
        not isinstance(pin_count, int)
        or isinstance(pin_count, bool)
        or pin_count < 1
        or pin_count > 256
    ):
        raise CatalogError(f"{path.relative_to(ROOT)} architecture.pin_count must be 1..256")
    template_slots = _slot_list(
        architecture.get("template_slots"), path, "architecture.template_slots", pin_count
    )
    if not template_slots or len(template_slots) > 255:
        raise CatalogError(
            f"{path.relative_to(ROOT)} architecture.template_slots must contain 1..255 pins"
        )
    numbering = architecture.get("numbering", "gpio")
    if numbering not in NUMBERING:
        raise CatalogError(
            f"{path.relative_to(ROOT)} architecture.numbering must be one of "
            + ", ".join(sorted(NUMBERING))
        )
    default_capabilities = _capabilities(
        architecture.get("default_capabilities"), path, "architecture.default_capabilities"
    )

    overrides: list[PinOverride] = []
    seen_pins: set[int] = set()
    for index, item in enumerate(_table_array(data.get("pin", []), path, "pin")):
        location = f"pin[{index}]"
        _unknown_fields(item, {"id", "present", "capabilities", "cautions"}, path, location)
        pin = item.get("id")
        if (
            not isinstance(pin, int)
            or isinstance(pin, bool)
            or pin < 0
            or pin >= pin_count
            or pin in seen_pins
        ):
            raise CatalogError(
                f"{path.relative_to(ROOT)} {location}.id is out of range or duplicated"
            )
        present = item.get("present", True)
        if not isinstance(present, bool):
            raise CatalogError(f"{path.relative_to(ROOT)} {location}.present must be boolean")
        raw_capabilities = item.get("capabilities")
        capabilities = (
            None
            if raw_capabilities is None
            else _capabilities(raw_capabilities, path, f"{location}.capabilities")
        )
        cautions = _string_list(item.get("cautions", []), path, f"{location}.cautions")
        if any(not CATALOG_ID.fullmatch(caution) for caution in cautions):
            raise CatalogError(
                f"{path.relative_to(ROOT)} {location}.cautions must use lowercase kebab-case"
            )
        overrides.append(PinOverride(pin, present, capabilities, cautions))
        seen_pins.add(pin)

    reservations: list[PinReservation] = []
    for index, item in enumerate(_table_array(data.get("reservation", []), path, "reservation")):
        location = f"reservation[{index}]"
        _unknown_fields(
            item,
            {
                "owner",
                "pins",
                "flash_modes",
                "psram_modes",
                "consoles",
                "sdk_boards",
                "boards",
                "template_may_claim",
            },
            path,
            location,
        )
        owner = _string(item.get("owner"), path, f"{location}.owner", 48)
        if not CATALOG_ID.fullmatch(owner):
            raise CatalogError(
                f"{path.relative_to(ROOT)} {location}.owner must use lowercase kebab-case"
            )
        pins = _pin_list(item.get("pins"), path, f"{location}.pins", pin_count)
        if not pins:
            raise CatalogError(f"{path.relative_to(ROOT)} {location}.pins cannot be empty")
        template_may_claim = item.get("template_may_claim", False)
        if not isinstance(template_may_claim, bool):
            raise CatalogError(
                f"{path.relative_to(ROOT)} {location}.template_may_claim must be boolean"
            )
        # Only the console can be stopped at boot; other owners keep their pins.
        if template_may_claim and not owner.startswith("console-"):
            raise CatalogError(
                f"{path.relative_to(ROOT)} {location}.template_may_claim is only valid "
                "for console owners"
            )
        reservations.append(
            PinReservation(
                owner,
                pins,
                _condition_values(item, "flash_modes", path, location, allowed=FLASH_MODES),
                _condition_values(item, "psram_modes", path, location, allowed=PSRAM_MODES),
                _condition_values(item, "consoles", path, location, allowed=CONSOLES),
                _condition_values(item, "sdk_boards", path, location, SDK_ID),
                _condition_values(item, "boards", path, location),
                template_may_claim,
            )
        )

    return ArchitectureTopology(
        platform_directory,
        architecture_name,
        pin_count,
        template_slots,
        str(numbering),
        default_capabilities,
        source,
        compatibility,
        tuple(sorted(overrides, key=lambda item: item.pin)),
        tuple(reservations),
        path,
    )


def load_architecture_topologies() -> dict[tuple[str, str], ArchitectureTopology]:
    """Load every topology and reject duplicate platform/architecture identities."""

    topologies: dict[tuple[str, str], ArchitectureTopology] = {}
    for path in sorted(TOPOLOGIES_DIR.glob("*/*.toml")):
        topology = load_architecture_topology(path)
        key = (topology.platform_directory, topology.architecture)
        if key in topologies:
            raise CatalogError(f"duplicate topology for {key[0]}/{key[1]}")
        topologies[key] = topology
    return topologies


def validate_module_slots(
    modules: tuple[HardwareModule, ...],
    topologies: dict[tuple[str, str], ArchitectureTopology],
) -> None:
    """Ensure every built-in module assignment names a real architecture slot."""

    for module in modules:
        key = (module.platform_directory, module.architecture)
        topology = topologies.get(key)
        if topology is None:
            raise CatalogError(
                f"{module.path.relative_to(ROOT)} has no topology for {key[0]}/{key[1]}"
            )
        for assignment in module.assignments:
            if assignment.slot >= len(topology.template_slots):
                raise CatalogError(
                    f"{module.path.relative_to(ROOT)} assignment slot {assignment.slot} exceeds "
                    f"the {len(topology.template_slots)}-slot architecture"
                )


def _reservation_matches(rule: PinReservation, environment: Environment) -> bool:
    """Return whether all declared reservation predicates match the environment."""

    flash = environment.board_data.get("flash")
    flash_mode = flash.get("mode") if isinstance(flash, dict) else None
    psram = environment.board_data.get("psram")
    psram_mode = psram.get("mode") if isinstance(psram, dict) else None
    return (
        (not rule.flash_modes or flash_mode in rule.flash_modes)
        and (not rule.psram_modes or psram_mode in rule.psram_modes)
        and (not rule.consoles or environment.console in rule.consoles)
        and (not rule.sdk_boards or environment.sdk_board in rule.sdk_boards)
        and (not rule.boards or environment.board in rule.boards)
    )


def _pin_label(numbering: str, pin: int) -> str:
    """Format a physical pin without embedding labels in firmware data."""

    if numbering == "amebad-port":
        return f"P{'A' if pin < 32 else 'B'}{pin % 32}"
    return f"GPIO{pin}"


def _source_hashes(topology: ArchitectureTopology, sdk_key: str | None = None) -> dict[str, str]:
    """Hash the reviewed SDK inputs so SDK changes invalidate topology identity."""

    from newmota.host import load_sdk_version

    sdk_key = sdk_key or topology.platform_directory
    sdk_root = load_sdk_version(sdk_key).source
    result: dict[str, str] = {}
    for raw_path in topology.source["paths"]:
        raw_path = str(raw_path)
        if sdk_key == "esp-idf":
            raw_path = raw_path.replace("components/esp_hal_gpspi/", "components/soc/")
        relative = Path(raw_path)
        source = sdk_root / relative
        if not source.is_file():
            raise CatalogError(
                f"{topology.path.relative_to(ROOT)} references missing SDK source "
                f"{source.relative_to(ROOT)}"
            )
        result[str(relative)] = sha256(source)
    return result


def resolve_pin_topology(
    environment: Environment,
    directory: Path,
    *,
    topologies: dict[tuple[str, str], ArchitectureTopology] | None = None,
    write_header: bool = True,
) -> ResolvedPinTopology:
    """Resolve one architecture topology against concrete board and memory facts."""

    if topologies is None:
        topologies = load_architecture_topologies()
    key = (environment.platform_directory, environment.chip)
    try:
        topology = topologies[key]
    except KeyError as error:
        raise CatalogError(f"missing hardware topology for {key[0]}/{key[1]}") from error

    exposed = set(environment.gpio.exposed) if environment.gpio.exposed is not None else None
    board_reserved = {owner: set(pins) for owner, pins in environment.gpio.reservations}
    if exposed is not None and any(pin >= topology.pin_count for pin in exposed):
        raise CatalogError(f"board '{environment.board}' exposes a pin outside {environment.chip}")
    if any(pin >= topology.pin_count for pins in board_reserved.values() for pin in pins):
        raise CatalogError(f"board '{environment.board}' reserves a pin outside {environment.chip}")

    overrides = {item.pin: item for item in topology.overrides}
    absent = {pin for pin, override in overrides.items() if not override.present}
    if exposed is not None and exposed & absent:
        invalid = ", ".join(_pin_label(topology.numbering, pin) for pin in sorted(exposed & absent))
        raise CatalogError(f"board '{environment.board}' exposes absent pins: {invalid}")
    reserved_absent = absent & {pin for pins in board_reserved.values() for pin in pins}
    if reserved_absent:
        invalid = ", ".join(_pin_label(topology.numbering, pin) for pin in sorted(reserved_absent))
        raise CatalogError(f"board '{environment.board}' reserves absent pins: {invalid}")
    conditional_owners: dict[int, set[str]] = {}
    yielding_owners: dict[int, set[str]] = {}
    for reservation in topology.reservations:
        if _reservation_matches(reservation, environment):
            owners = yielding_owners if reservation.template_may_claim else conditional_owners
            for pin in reservation.pins:
                owners.setdefault(pin, set()).add(reservation.owner)
    for owner, pins in board_reserved.items():
        for pin in pins:
            conditional_owners.setdefault(pin, set()).add(owner)

    slot_by_pin = {pin: slot for slot, pin in enumerate(topology.template_slots)}
    pins: list[ResolvedPin] = []
    for pin in range(topology.pin_count):
        override = overrides.get(pin)
        present = override.present if override is not None else True
        capabilities = (
            override.capabilities
            if override is not None and override.capabilities is not None
            else topology.default_capabilities
        )
        is_exposed = exposed is None or pin in exposed
        reserved_by = tuple(sorted(conditional_owners.get(pin, set())))
        pins.append(
            ResolvedPin(
                pin,
                _pin_label(topology.numbering, pin),
                slot_by_pin.get(pin),
                present,
                is_exposed,
                present and is_exposed and not reserved_by,
                capabilities if present else (),
                override.cautions if override is not None else (),
                reserved_by,
                tuple(sorted(yielding_owners.get(pin, set()))),
            )
        )

    source_hashes = _source_hashes(topology, environment.sdk_key)
    identity_input = {
        "environment": environment.name,
        "board": environment.board,
        "platform": environment.platform_directory,
        "architecture": environment.chip,
        "generator_sha256": sha256(Path(__file__)),
        "topology_sha256": sha256(topology.path),
        "source_sha256": source_hashes,
        "flash": environment.board_data.get("flash"),
        "psram": environment.board_data.get("psram"),
        "console": environment.console,
        "sdk_board": environment.sdk_board,
        "board_gpio": {
            "source": environment.gpio.source,
            "exposed": (
                list(environment.gpio.exposed) if environment.gpio.exposed is not None else None
            ),
            "reserved": {
                owner: list(reserved) for owner, reserved in environment.gpio.reservations
            },
        },
    }
    encoded = json.dumps(identity_input, sort_keys=True, separators=(",", ":")).encode("utf-8")
    identity = hashlib.sha256(encoded).hexdigest()
    header = directory / "pin_topology.h"

    header_lines = [
        "/* Generated by tools/newmota/topology.py; do not edit. */",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace newmota::generated::hardware {",
        f'inline constexpr char kArchitecture[] = "{topology.architecture}";',
        "inline constexpr std::uint8_t kNoTemplateSlot = 0xffu;",
        "inline constexpr std::uint8_t kPinAvailable = 0x01u;",
        "inline constexpr std::uint8_t kPinCaution = 0x02u;",
        "inline constexpr std::uint8_t kPinConsole = 0x04u;",
    ]
    for name, bit in CAPABILITY_BITS.items():
        cpp_name = "".join(part.title() for part in name.split("-"))
        header_lines.append(f"inline constexpr std::uint16_t k{cpp_name} = 0x{bit:04x}u;")
    header_lines.extend(
        (
            "",
            "struct PinTopology {",
            "  std::uint16_t id;",
            "  std::uint16_t capabilities;",
            "  std::uint8_t template_slot;",
            "  std::uint8_t flags;",
            "};",
            "",
            f"inline constexpr std::uint16_t kTemplateToPhysical[{len(topology.template_slots)}] = {{",
            "  " + ", ".join(f"{pin}u" for pin in topology.template_slots),
            "};",
            "",
            f"inline constexpr PinTopology kPins[{len(pins)}] = {{",
        )
    )
    for pin in pins:
        capability_mask = sum(CAPABILITY_BITS[item] for item in pin.capabilities)
        flags = (
            (1 if pin.available else 0) | (2 if pin.cautions else 0) | (4 if pin.yields_from else 0)
        )
        slot = pin.template_slot if pin.template_slot is not None else 0xFF
        header_lines.append(f"  {{{pin.pin}u, 0x{capability_mask:04x}u, {slot}u, 0x{flags:02x}u}},")
    header_lines.extend(
        (
            "};",
            f"inline constexpr std::size_t kPinCount = {len(pins)}u;",
            f"inline constexpr std::size_t kTemplateSlotCount = {len(topology.template_slots)}u;",
            "",
            "}  // namespace newmota::generated::hardware",
            "",
        )
    )
    if write_header:
        write_if_changed(header, "\n".join(header_lines))

    document: dict[str, object] = {
        "schema": 1,
        "identity": identity,
        "definition": str(topology.path.relative_to(ROOT)),
        "source": {
            **topology.source,
            "paths": list(topology.source["paths"]),
            "sha256": source_hashes,
        },
        "compatibility": topology.compatibility,
        "selection": {
            "environment": environment.name,
            "board": environment.board,
            "flash_mode": identity_input["flash"].get("mode")
            if isinstance(identity_input["flash"], dict)
            else None,
            "psram_mode": identity_input["psram"].get("mode")
            if isinstance(identity_input["psram"], dict)
            else None,
            "console": environment.console,
            "sdk_board": environment.sdk_board,
            "board_gpio_source": environment.gpio.source,
        },
        "numbering": topology.numbering,
        "template_slots": [
            {"slot": slot, "pin": pin} for slot, pin in enumerate(topology.template_slots)
        ],
        "pins": [
            {
                "id": pin.pin,
                "label": pin.label,
                "template_slot": pin.template_slot,
                "present": pin.present,
                "exposed": pin.exposed,
                "available": pin.available,
                "capabilities": list(pin.capabilities),
                "cautions": list(pin.cautions),
                "reserved_by": list(pin.reserved_by),
                "yields_from": list(pin.yields_from),
            }
            for pin in pins
        ],
    }
    return ResolvedPinTopology(identity, header, document, topology, tuple(pins))
