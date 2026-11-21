"""Validate and generate Newmota's Tasmota-compatible hardware catalog."""

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
from newmota.topology import (
    CAPABILITY_BITS,
    ResolvedPinTopology,
    load_architecture_topologies,
    resolve_pin_topology,
    validate_module_slots,
)

if TYPE_CHECKING:
    from newmota.features import ResolvedFeatures

GPIO_REGISTRY_PATH = HARDWARE_DIR / "gpio-functions.toml"
MODULES_DIR = HARDWARE_DIR / "modules"
GPIO_SYMBOL = re.compile(r"GPIO_[A-Z0-9_]+")
FEATURE_NAME = re.compile(r"USE_[A-Z0-9_]+")
CATALOG_ID = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*")
REVISION = re.compile(r"[0-9a-f]{40}")
ENUM_DIGEST = re.compile(r"[0-9a-f]{64}")
PIN_REQUIREMENTS = frozenset(
    {
        "input",
        "output",
        "pull-up",
        "pull-down",
        "open-drain",
        "interrupt",
        "analog",
    }
)
FUNCTION_MODIFIERS = frozenset(
    {
        "inverted",
        "no-pull",
        "pull-down",
        "bistable",
        "fixed",
        "initial-high",
        "initial-low",
    }
)
CANONICAL_ROLES = {
    "none": 0,
    "input": 1,
    "fixed-output": 2,
    "button": 3,
    "relay": 4,
    "status-led": 5,
    "pwm": 6,
    "i2c-scl": 7,
    "i2c-sda": 8,
    "led-strip": 9,
    "dht11": 10,
    "dht22": 11,
    "si7021": 12,
    "i2s-dout": 13,
    "i2s-bclk": 14,
    "i2s-ws": 15,
    "i2s-din": 16,
    "i2s-mclk": 17,
    "link-led": 18,
    "gps-rx": 19,
    "gps-tx": 20,
    "energy-rx": 21,
    "energy-tx": 22,
    "energy-cf": 23,
    "energy-cf1": 24,
    "energy-sel": 25,
    "energy-irq": 26,
    "energy-reset": 27,
    "energy-cs": 28,
    "energy-voltage": 29,
    "energy-current": 30,
    "energy-option": 31,
    "switch": 32,
    "buzzer": 33,
}
MODIFIER_BITS = {
    "inverted": 1 << 0,
    "no-pull": 1 << 1,
    "pull-down": 1 << 2,
    "bistable": 1 << 3,
    "fixed": 1 << 4,
    "initial-high": 1 << 5,
    "initial-low": 1 << 6,
}


@dataclass(frozen=True)
class GpioIdentity:
    """One immutable Tasmota GPIO base number and its stable source symbol."""

    base: int
    symbol: str


@dataclass(frozen=True)
class GpioFunction:
    """One GPIO identity whose runtime semantics have been qualified."""

    identity: GpioIdentity
    display: str
    instances: int
    owner: str
    requirements: tuple[str, ...]
    canonical: str
    modifiers: tuple[str, ...]


@dataclass(frozen=True)
class GpioRegistry:
    """Validated GPIO identity namespace and qualified function descriptors."""

    schema: int
    source: dict[str, object]
    shift: int
    instance_mask: int
    sensor_end: int
    fixed_start: int
    user_base: int
    template_user_value: int
    identities: tuple[GpioIdentity, ...]
    functions: tuple[GpioFunction, ...]

    def wire_value(self, base: int, instance: int = 1) -> int:
        """Encode a one-based function instance into Tasmota's 16-bit wire value."""

        return (base << self.shift) | (instance - 1)


@dataclass(frozen=True)
class ModuleAssignment:
    """One architecture-template slot and its symbolic GPIO assignment."""

    slot: int
    symbol: str
    instance: int
    template_value: int


@dataclass(frozen=True)
class HardwareModule:
    """One validated built-in runtime module definition."""

    platform_directory: str
    architecture: str
    number: int
    name: str
    flag: int
    base: int | None
    requirements: tuple[str, ...]
    provenance: dict[str, str]
    assignments: tuple[ModuleAssignment, ...]
    path: Path


@dataclass(frozen=True)
class ResolvedHardwareCatalog:
    """Generated hardware inputs and their build-configuration identity."""

    identity: str
    header: Path
    names_header: Path
    topology_header: Path
    manifest: Path
    source_revision: str


def _read_toml(path: Path) -> dict[str, object]:
    """Read one TOML object and translate parser failures into catalog errors."""

    try:
        with path.open("rb") as handle:
            data = tomllib.load(handle)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise CatalogError(f"cannot read {path.relative_to(ROOT)}: {error}") from error
    if not isinstance(data, dict):
        raise CatalogError(f"{path.relative_to(ROOT)} must contain a TOML table")
    return data


def _unknown_fields(data: dict[str, object], allowed: set[str], path: Path, location: str) -> None:
    """Reject fields that would otherwise look supported while being ignored."""

    unknown = sorted(set(data) - allowed)
    if unknown:
        raise CatalogError(
            f"{path.relative_to(ROOT)} {location} has unknown fields: {', '.join(unknown)}"
        )


def _table(value: object, path: Path, location: str) -> dict[str, object]:
    """Require a TOML table at the named schema location."""

    if not isinstance(value, dict):
        raise CatalogError(f"{path.relative_to(ROOT)} {location} must be a table")
    return value


def _table_array(value: object, path: Path, location: str) -> list[dict[str, object]]:
    """Require an array of TOML tables at the named schema location."""

    if not isinstance(value, list) or any(not isinstance(item, dict) for item in value):
        raise CatalogError(f"{path.relative_to(ROOT)} {location} must be an array of tables")
    return value


def _integer(value: object, path: Path, location: str, minimum: int, maximum: int) -> int:
    """Require one non-boolean integer inside an inclusive range."""

    if not isinstance(value, int) or isinstance(value, bool) or value < minimum or value > maximum:
        raise CatalogError(
            f"{path.relative_to(ROOT)} {location} must be between {minimum} and {maximum}"
        )
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
    """Require a duplicate-free string array while preserving declared order."""

    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise CatalogError(f"{path.relative_to(ROOT)} {location} must be a string array")
    if len(value) != len(set(value)):
        raise CatalogError(f"{path.relative_to(ROOT)} {location} contains duplicates")
    return tuple(value)


def _validate_source(data: dict[str, object], path: Path) -> dict[str, object]:
    """Validate the immutable upstream provenance attached to the GPIO namespace."""

    _unknown_fields(
        data,
        {"project", "repository", "revision", "path", "defines", "enum_sha256"},
        path,
        "source",
    )
    project = _string(data.get("project"), path, "source.project")
    repository = _string(data.get("repository"), path, "source.repository", 256)
    revision = _string(data.get("revision"), path, "source.revision")
    source_path = _string(data.get("path"), path, "source.path", 256)
    defines = _string_list(data.get("defines"), path, "source.defines")
    enum_sha256 = _string(data.get("enum_sha256"), path, "source.enum_sha256")
    if not REVISION.fullmatch(revision):
        raise CatalogError(f"{path.relative_to(ROOT)} source.revision must be a full commit hash")
    if not ENUM_DIGEST.fullmatch(enum_sha256):
        raise CatalogError(f"{path.relative_to(ROOT)} source.enum_sha256 must be a SHA-256")
    if any(not re.fullmatch(r"[A-Z][A-Z0-9_]*", item) for item in defines):
        raise CatalogError(f"{path.relative_to(ROOT)} source.defines contains an invalid macro")
    return {
        "project": project,
        "repository": repository,
        "revision": revision,
        "path": source_path,
        "defines": defines,
        "enum_sha256": enum_sha256,
    }


def load_gpio_registry(path: Path = GPIO_REGISTRY_PATH) -> GpioRegistry:
    """Load and strictly validate the frozen GPIO identity registry."""

    data = _read_toml(path)
    _unknown_fields(data, {"schema", "source", "encoding", "id", "function"}, path, "root")
    if data.get("schema") != 1:
        raise CatalogError(f"{path.relative_to(ROOT)} schema must be 1")
    source = _validate_source(_table(data.get("source"), path, "source"), path)

    encoding = _table(data.get("encoding"), path, "encoding")
    _unknown_fields(
        encoding,
        {
            "shift",
            "instance_mask",
            "sensor_end",
            "fixed_start",
            "user_base",
            "template_user_value",
        },
        path,
        "encoding",
    )
    shift = _integer(encoding.get("shift"), path, "encoding.shift", 1, 15)
    instance_mask = _integer(
        encoding.get("instance_mask"), path, "encoding.instance_mask", 1, 0x7FFF
    )
    sensor_end = _integer(encoding.get("sensor_end"), path, "encoding.sensor_end", 1, 2045)
    fixed_start = _integer(encoding.get("fixed_start"), path, "encoding.fixed_start", 1, 2046)
    user_base = _integer(encoding.get("user_base"), path, "encoding.user_base", 1, 2047)
    template_user_value = _integer(
        encoding.get("template_user_value"), path, "encoding.template_user_value", 1, 0xFFFF
    )
    if (shift, instance_mask, fixed_start, user_base, template_user_value) != (
        5,
        31,
        2046,
        2047,
        1,
    ):
        raise CatalogError(f"{path.relative_to(ROOT)} changes Tasmota's fixed GPIO encoding")

    raw_ids = _table_array(data.get("id"), path, "id")
    identities: list[GpioIdentity] = []
    symbols: set[str] = set()
    for expected_base, item in enumerate(raw_ids):
        _unknown_fields(item, {"base", "symbol"}, path, f"id[{expected_base}]")
        base = _integer(item.get("base"), path, f"id[{expected_base}].base", 0, sensor_end - 1)
        symbol = _string(item.get("symbol"), path, f"id[{expected_base}].symbol", 64)
        if base != expected_base:
            raise CatalogError(
                f"{path.relative_to(ROOT)} id[{expected_base}].base must be {expected_base}"
            )
        if not GPIO_SYMBOL.fullmatch(symbol) or symbol in symbols:
            raise CatalogError(
                f"{path.relative_to(ROOT)} id[{expected_base}].symbol is invalid or duplicated"
            )
        symbols.add(symbol)
        identities.append(GpioIdentity(base, symbol))
    if len(identities) != sensor_end or identities[0].symbol != "GPIO_NONE":
        raise CatalogError(
            f"{path.relative_to(ROOT)} must contain every base from GPIO_NONE through sensor_end"
        )
    canonical_enum = "".join(
        f"{identity.base}:{identity.symbol}\n" for identity in identities
    ).encode("utf-8")
    if hashlib.sha256(canonical_enum).hexdigest() != source["enum_sha256"]:
        raise CatalogError(f"{path.relative_to(ROOT)} GPIO identity snapshot digest mismatch")

    identities_by_symbol = {identity.symbol: identity for identity in identities}
    raw_functions = _table_array(data.get("function", []), path, "function")
    functions: list[GpioFunction] = []
    described: set[str] = set()
    previous_base = -1
    for index, item in enumerate(raw_functions):
        _unknown_fields(
            item,
            {"symbol", "display", "instances", "owner", "requires", "canonical", "modifiers"},
            path,
            f"function[{index}]",
        )
        symbol = _string(item.get("symbol"), path, f"function[{index}].symbol", 64)
        identity = identities_by_symbol.get(symbol)
        if identity is None or symbol in described:
            raise CatalogError(
                f"{path.relative_to(ROOT)} function[{index}].symbol is unknown or duplicated"
            )
        if identity.base <= previous_base:
            raise CatalogError(f"{path.relative_to(ROOT)} function descriptors must be base-sorted")
        display = _string(item.get("display"), path, f"function[{index}].display", 48)
        instances = _integer(
            item.get("instances"), path, f"function[{index}].instances", 1, instance_mask + 1
        )
        owner = _string(item.get("owner"), path, f"function[{index}].owner", 192)
        if owner != "core" and not all(FEATURE_NAME.fullmatch(name) for name in owner.split("|")):
            raise CatalogError(f"{path.relative_to(ROOT)} function[{index}].owner is invalid")
        requirements = _string_list(item.get("requires"), path, f"function[{index}].requires")
        unknown_requirements = sorted(set(requirements) - PIN_REQUIREMENTS)
        if unknown_requirements:
            raise CatalogError(
                f"{path.relative_to(ROOT)} function[{index}] has unknown pin requirements: "
                + ", ".join(unknown_requirements)
            )
        canonical = _string(item.get("canonical"), path, f"function[{index}].canonical", 64)
        if not CATALOG_ID.fullmatch(canonical) or canonical not in CANONICAL_ROLES:
            raise CatalogError(f"{path.relative_to(ROOT)} function[{index}].canonical is invalid")
        modifiers = _string_list(item.get("modifiers"), path, f"function[{index}].modifiers")
        unknown_modifiers = sorted(set(modifiers) - FUNCTION_MODIFIERS)
        if unknown_modifiers:
            raise CatalogError(
                f"{path.relative_to(ROOT)} function[{index}] has unknown modifiers: "
                + ", ".join(unknown_modifiers)
            )
        functions.append(
            GpioFunction(identity, display, instances, owner, requirements, canonical, modifiers)
        )
        described.add(symbol)
        previous_base = identity.base

    return GpioRegistry(
        1,
        source,
        shift,
        instance_mask,
        sensor_end,
        fixed_start,
        user_base,
        template_user_value,
        tuple(identities),
        tuple(functions),
    )


def _module_location(path: Path) -> tuple[str, str]:
    """Validate and return the platform/architecture encoded by a module path."""

    try:
        relative = path.relative_to(MODULES_DIR)
    except ValueError as error:
        raise CatalogError(f"module is outside {MODULES_DIR.relative_to(ROOT)}: {path}") from error
    if len(relative.parts) != 3 or relative.suffix != ".toml":
        raise CatalogError(
            f"{path.relative_to(ROOT)} must be <platform>/<architecture>/<module>.toml"
        )
    platform, architecture, filename = relative.parts
    if platform not in DIRECTORY_PLATFORMS:
        raise CatalogError(f"{path.relative_to(ROOT)} uses an unknown platform directory")
    if not CATALOG_ID.fullmatch(architecture) or not CATALOG_ID.fullmatch(Path(filename).stem):
        raise CatalogError(
            f"{path.relative_to(ROOT)} architecture and filename must use lowercase kebab-case"
        )
    return platform, architecture


def _load_hardware_module(path: Path, registry: GpioRegistry) -> HardwareModule:
    """Load one built-in module and validate symbolic assignments against the registry."""

    platform, architecture = _module_location(path)
    data = _read_toml(path)
    _unknown_fields(data, {"schema", "module", "provenance", "assignment"}, path, "root")
    if data.get("schema") != 1:
        raise CatalogError(f"{path.relative_to(ROOT)} schema must be 1")

    module = _table(data.get("module"), path, "module")
    _unknown_fields(module, {"number", "name", "flag", "base", "requires"}, path, "module")
    number = _integer(module.get("number"), path, "module.number", 1, 254)
    name = _string(module.get("name"), path, "module.name", 32)
    flag = _integer(module.get("flag", 0), path, "module.flag", 0, 0xFFFFFFFF)
    raw_base = module.get("base")
    base = None if raw_base is None else _integer(raw_base, path, "module.base", 1, 254)
    requirements = _string_list(module.get("requires", []), path, "module.requires")
    if any(not FEATURE_NAME.fullmatch(item) for item in requirements):
        raise CatalogError(f"{path.relative_to(ROOT)} module.requires has an invalid feature")

    provenance_table = _table(data.get("provenance"), path, "provenance")
    _unknown_fields(provenance_table, {"source", "revision", "path"}, path, "provenance")
    provenance = {
        "source": _string(provenance_table.get("source"), path, "provenance.source"),
        "path": _string(provenance_table.get("path"), path, "provenance.path", 256),
    }
    raw_revision = provenance_table.get("revision")
    if raw_revision is not None:
        revision = _string(raw_revision, path, "provenance.revision")
        if not REVISION.fullmatch(revision):
            raise CatalogError(
                f"{path.relative_to(ROOT)} provenance.revision must be a full commit hash"
            )
        provenance["revision"] = revision

    identities = {item.symbol: item for item in registry.identities}
    descriptors = {item.identity.symbol: item for item in registry.functions}
    raw_assignments = _table_array(data.get("assignment", []), path, "assignment")
    assignments: list[ModuleAssignment] = []
    slots: set[int] = set()
    for index, item in enumerate(raw_assignments):
        _unknown_fields(item, {"slot", "function", "instance"}, path, f"assignment[{index}]")
        slot = _integer(item.get("slot"), path, f"assignment[{index}].slot", 0, 255)
        symbol = _string(item.get("function"), path, f"assignment[{index}].function", 64)
        instance = _integer(
            item.get("instance", 1),
            path,
            f"assignment[{index}].instance",
            1,
            registry.instance_mask + 1,
        )
        if slot in slots:
            raise CatalogError(f"{path.relative_to(ROOT)} duplicates assignment slot {slot}")
        if symbol == "GPIO_USER":
            if instance != 1:
                raise CatalogError(f"{path.relative_to(ROOT)} GPIO_USER cannot have an instance")
            template_value = registry.template_user_value
        else:
            identity = identities.get(symbol)
            if identity is None:
                raise CatalogError(
                    f"{path.relative_to(ROOT)} assignment[{index}] references unknown {symbol}"
                )
            descriptor = descriptors.get(symbol)
            if descriptor is not None and instance > descriptor.instances:
                raise CatalogError(
                    f"{path.relative_to(ROOT)} assignment[{index}] instance exceeds {symbol} limit"
                )
            if symbol == "GPIO_NONE" and instance != 1:
                raise CatalogError(f"{path.relative_to(ROOT)} GPIO_NONE cannot have an instance")
            template_value = registry.wire_value(identity.base, instance)
        assignments.append(ModuleAssignment(slot, symbol, instance, template_value))
        slots.add(slot)

    return HardwareModule(
        platform,
        architecture,
        number,
        name,
        flag,
        base,
        requirements,
        provenance,
        tuple(sorted(assignments, key=lambda item: item.slot)),
        path,
    )


def load_hardware_modules(registry: GpioRegistry) -> tuple[HardwareModule, ...]:
    """Load every runtime module and reject architecture-scoped identity conflicts."""

    modules = tuple(
        _load_hardware_module(path, registry) for path in sorted(MODULES_DIR.rglob("*.toml"))
    )
    numbers: dict[tuple[str, str, int], Path] = {}
    names: dict[tuple[str, str, str], Path] = {}
    for module in modules:
        number_key = (module.platform_directory, module.architecture, module.number)
        name_key = (module.platform_directory, module.architecture, module.name.casefold())
        if number_key in numbers:
            raise CatalogError(
                f"{module.path.relative_to(ROOT)} duplicates module {module.number} from "
                f"{numbers[number_key].relative_to(ROOT)}"
            )
        if name_key in names:
            raise CatalogError(
                f"{module.path.relative_to(ROOT)} duplicates module name from "
                f"{names[name_key].relative_to(ROOT)}"
            )
        numbers[number_key] = module.path
        names[name_key] = module.path
    return modules


def _cpp_name(symbol: str) -> str:
    """Convert an upstream GPIO symbol into a namespaced generated C++ constant."""

    return "k" + "".join(part.title() for part in symbol.lower().split("_"))


def _validate_active_modules(
    modules: tuple[HardwareModule, ...],
    registry: GpioRegistry,
    topology: ResolvedPinTopology,
    environment: Environment,
) -> None:
    """Reject built-in assignments that cannot exist on the selected board topology."""

    functions = {item.identity.symbol: item for item in registry.functions}
    pins = {item.pin: item for item in topology.pins}
    for module in modules:
        for assignment in module.assignments:
            if assignment.symbol in {"GPIO_NONE", "GPIO_USER"}:
                continue
            physical = topology.definition.template_slots[assignment.slot]
            pin = pins[physical]
            if not pin.available:
                raise CatalogError(
                    f"{module.path.relative_to(ROOT)} assigns {assignment.symbol} to unavailable "
                    f"{pin.label} for board '{environment.board}'"
                )
            function = functions.get(assignment.symbol)
            if function is not None and not set(function.requirements).issubset(pin.capabilities):
                missing = sorted(set(function.requirements) - set(pin.capabilities))
                raise CatalogError(
                    f"{module.path.relative_to(ROOT)} assigns {assignment.symbol} to {pin.label}, "
                    f"which lacks {', '.join(missing)}"
                )


def _template_number(template: dict[str, object], key: str, maximum: int, label: str) -> None:
    value = template.get(key, 0)
    if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= maximum:
        raise CatalogError(f"{label} template {key} must be a whole number from 0 to {maximum}")


def check_template(environment: Environment, selected: dict[str, bool]) -> list[str]:
    """Apply the Template command's rules at build time. Return what will stay inactive."""

    assert environment.path is not None
    label = str(environment.path.relative_to(ROOT))
    template = environment.template
    if environment.role != "application":
        raise CatalogError(f"{label} template is only valid for application environments")
    if not isinstance(template, dict):
        raise CatalogError(f"{label} template must be a JSON object")
    unknown = sorted(
        str(key) for key in template if key not in {"NAME", "ARCH", "GPIO", "FLAG", "BASE"}
    )
    if unknown:
        raise CatalogError(f"{label} template has unknown fields: {', '.join(unknown)}")
    name = template.get("NAME", "")
    if (
        not isinstance(name, str)
        or len(name.encode("utf-8")) > 32
        or any(ord(character) < 32 or ord(character) == 127 for character in name)
    ):
        raise CatalogError(f"{label} template NAME must be text of at most 32 bytes")
    if "ARCH" in template and template["ARCH"] != environment.chip:
        raise CatalogError(f"{label} template ARCH must be {environment.chip} for this board")
    _template_number(template, "FLAG", 0xFFFFFFFF, label)
    _template_number(template, "BASE", 254, label)

    registry = load_gpio_registry()
    topology = resolve_pin_topology(environment, ROOT / "build" / "unused", write_header=False)
    slots = topology.definition.template_slots
    assignments = template.get("GPIO")
    if not isinstance(assignments, list) or len(assignments) != len(slots):
        raise CatalogError(
            f"{label} template GPIO must have {len(slots)} values for {environment.chip}"
        )
    pins = {item.pin: item for item in topology.pins}

    def first_of(function: GpioFunction) -> int:
        return registry.wire_value(function.identity.base)

    active = sorted(
        (
            function
            for function in registry.functions
            if function.owner == "core"
            or any(selected.get(name, False) for name in function.owner.split("|"))
        ),
        key=first_of,
    )
    inactive: list[str] = []
    used_pads: dict[int, int] = {}
    used_functions: dict[tuple[str, int], int] = {}
    for slot, value in enumerate(assignments):
        if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= 0xFFFF:
            raise CatalogError(
                f"{label} template GPIO[{slot}] must be a whole number from 0 to 65535"
            )
        if value in {0, registry.template_user_value}:
            continue
        physical = slots[slot]
        function = next(
            (item for item in active if 0 <= value - first_of(item) < item.instances), None
        )
        if function is None:
            if value >> registry.shift >= registry.sensor_end:
                raise CatalogError(
                    f"{label} template GPIO[{slot}] value {value} is not a Tasmota function"
                )
            owner = next(
                (
                    item.owner
                    for item in registry.functions
                    if 0 <= value - first_of(item) < item.instances
                ),
                None,
            )
            inactive.append(
                f"GPIO[{slot}] value {value} needs {owner}"
                if owner
                else f"GPIO[{slot}] value {value} has no driver"
            )
            continue
        pin = pins.get(physical)
        if pin is None or not pin.available:
            raise CatalogError(
                f"{label} template GPIO[{slot}] uses a pin this board does not offer"
            )
        missing = sorted(set(function.requirements) - set(pin.capabilities))
        if missing:
            raise CatalogError(
                f"{label} template GPIO[{slot}] uses {pin.label}, which lacks {', '.join(missing)}"
            )
        # Functions without a shared role, such as SPI or webcam pins, are distinct by value.
        role = (
            (function.canonical, value - first_of(function))
            if function.canonical != "none"
            else ("none", value)
        )
        if physical in used_pads:
            raise CatalogError(
                f"{label} template GPIO[{slot}] uses the same pin as GPIO[{used_pads[physical]}]"
            )
        if role in used_functions:
            raise CatalogError(
                f"{label} template GPIO[{slot}] repeats the function of GPIO[{used_functions[role]}]"
            )
        used_pads[physical] = slot
        used_functions[role] = slot
    return inactive


def resolve_hardware_catalog(
    environment: Environment, features: ResolvedFeatures, work_dir: Path
) -> ResolvedHardwareCatalog:
    """Generate deterministic host and C++ views for one resolved environment."""

    registry = load_gpio_registry()
    modules = load_hardware_modules(registry)
    topologies = load_architecture_topologies()
    validate_module_slots(modules, topologies)
    directory = work_dir / "generated" / "hardware"
    topology = resolve_pin_topology(environment, directory, topologies=topologies)
    enabled = dict(features.assignments)
    if environment.template is not None:
        for note in check_template(environment, enabled):
            print(f"warning: template {note}; it stays inactive", flush=True)
    active_functions = tuple(
        function
        for function in registry.functions
        if function.owner == "core"
        or any(enabled.get(name, False) for name in function.owner.split("|"))
    )
    active_modules = tuple(
        module
        for module in modules
        if module.platform_directory == environment.platform_directory
        and module.architecture == environment.chip
        and all(enabled.get(requirement, False) for requirement in module.requirements)
    )
    _validate_active_modules(active_modules, registry, topology, environment)

    inputs = {
        "environment": environment.name,
        "platform": environment.platform_directory,
        "architecture": environment.chip,
        "generator_sha256": sha256(Path(__file__)),
        "registry_sha256": sha256(GPIO_REGISTRY_PATH),
        "topology_identity": topology.identity,
        "module_sha256": {
            str(module.path.relative_to(ROOT)): sha256(module.path) for module in active_modules
        },
        "active_functions": [function.identity.symbol for function in active_functions],
        "active_modules": [module.number for module in active_modules],
    }
    encoded = json.dumps(inputs, sort_keys=True, separators=(",", ":")).encode("utf-8")
    identity = hashlib.sha256(encoded).hexdigest()
    header = directory / "gpio_ids.h"
    manifest = directory / "catalog.json"

    header_lines = [
        "/* Generated by tools/newmota/hardware.py; do not edit. */",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace newmota::generated::gpio {",
        f"inline constexpr std::uint8_t kEncodingShift = {registry.shift}u;",
        f"inline constexpr std::uint16_t kInstanceMask = {registry.instance_mask}u;",
        f"inline constexpr std::uint16_t kSensorEnd = {registry.sensor_end}u;",
        f"inline constexpr std::uint16_t kFixedStart = {registry.fixed_start}u;",
        f"inline constexpr std::uint16_t kUserBase = {registry.user_base}u;",
        f"inline constexpr std::uint16_t kUser = {registry.wire_value(registry.user_base)}u;",
        f"inline constexpr std::uint16_t kTemplateUserValue = {registry.template_user_value}u;",
        *(
            f"inline constexpr std::uint8_t kRole{''.join(part.title() for part in name.split('-'))} = {value}u;"
            for name, value in CANONICAL_ROLES.items()
        ),
        *(
            f"inline constexpr std::uint8_t kModifier{''.join(part.title() for part in name.split('-'))} = 0x{value:02x}u;"
            for name, value in MODIFIER_BITS.items()
        ),
        "",
        "/** Encode a one-based instance as a Tasmota-compatible GPIO value. */",
        "[[nodiscard]] constexpr std::uint16_t encode(",
        "    std::uint16_t base, std::uint16_t instance = 1u) {",
        "  return static_cast<std::uint16_t>(",
        "      (static_cast<std::uint32_t>(base) << kEncodingShift) |",
        "      (static_cast<std::uint32_t>(instance) - 1u));",
        "}",
        "",
    ]
    # Berry uses unshifted GPIO base identities, independently of enabled drivers.
    header_lines.append("#define NEWMOTA_BERRY_GPIO_IDENTITIES(X) " + chr(92))
    for gpio_identity in registry.identities:
        name = gpio_identity.symbol.removeprefix("GPIO_")
        if gpio_identity.symbol == "GPIO_INPUT":
            name = "GPIO_INPUT"
        header_lines.append(f'  X("{name}", {gpio_identity.base}) \\')
        if gpio_identity.symbol == "GPIO_DISPLAY_RESET":
            header_lines.append(f'  X("OLED_RESET", {gpio_identity.base}) \\')
    header_lines.append(f'  X("SENSOR_END", {registry.sensor_end})')
    header_lines.append("")
    for function in active_functions:
        name = _cpp_name(function.identity.symbol)
        header_lines.append(
            f"inline constexpr std::uint16_t {name}Base = {function.identity.base}u;"
        )
        header_lines.append(f"inline constexpr std::uint16_t {name} = encode({name}Base);")
    header_lines.extend(
        (
            "",
            "struct FunctionDescriptor {",
            "  std::uint16_t first;",
            "  std::uint16_t requirements;",
            "  std::uint8_t instances;",
            "  std::uint8_t role;",
            "  std::uint8_t modifiers;",
            "};",
            "",
            "struct ModuleAssignment {",
            "  std::uint16_t function;",
            "  std::uint8_t slot;",
            "};",
            "",
            "struct ModuleDescriptor {",
            "  std::uint32_t flag;",
            "  std::uint16_t assignment_offset;",
            "  std::uint8_t number;",
            "  std::uint8_t assignment_count;",
            "  std::uint8_t base;",
            "};",
            "",
            f"inline constexpr std::array<FunctionDescriptor, {len(active_functions)}> kFunctions{{{{",
        )
    )
    for function in active_functions:
        requirements = sum(CAPABILITY_BITS[item] for item in function.requirements)
        modifiers = sum(MODIFIER_BITS[item] for item in function.modifiers)
        header_lines.append(
            "  {"
            f"{registry.wire_value(function.identity.base)}u, 0x{requirements:04x}u, "
            f"{function.instances}u, {CANONICAL_ROLES[function.canonical]}u, "
            f"0x{modifiers:02x}u"
            "},"
        )
    header_lines.extend(("}};", ""))

    module_assignments = [
        assignment for module in active_modules for assignment in module.assignments
    ]
    header_lines.append(
        "inline constexpr std::array<ModuleAssignment, "
        f"{len(module_assignments)}> kModuleAssignments{{{{"
    )
    for assignment in module_assignments:
        header_lines.append(f"  {{{assignment.template_value}u, {assignment.slot}u}},")
    header_lines.extend(("}};", ""))
    header_lines.append(
        f"inline constexpr std::array<ModuleDescriptor, {len(active_modules)}> kModules{{{{"
    )
    assignment_offset = 0
    for module in active_modules:
        header_lines.append(
            "  {"
            f"0x{module.flag:08x}u, {assignment_offset}u, {module.number}u, "
            f"{len(module.assignments)}u, {module.base or 0}u"
            "},"
        )
        assignment_offset += len(module.assignments)
    header_lines.extend(("}};", "", "}  // namespace newmota::generated::gpio", ""))
    write_if_changed(header, "\n".join(header_lines))

    function_name_offsets: list[int] = []
    function_name_bytes = bytearray()
    for function in active_functions:
        function_name_offsets.append(len(function_name_bytes))
        function_name_bytes.extend(function.display.encode("utf-8"))
        function_name_bytes.append(0)
    module_name_offsets: list[int] = []
    module_name_bytes = bytearray()
    for module in active_modules:
        module_name_offsets.append(len(module_name_bytes))
        module_name_bytes.extend(module.name.encode("utf-8"))
        module_name_bytes.append(0)
    if not function_name_bytes:
        function_name_bytes.append(0)
    if not module_name_bytes:
        module_name_bytes.append(0)
    if len(function_name_bytes) > 0xFFFF or len(module_name_bytes) > 0xFFFF:
        raise CatalogError("generated hardware command name pool exceeds 65535 bytes")

    names_header = directory / "hardware_names.h"
    names_lines = [
        "/* Generated by tools/newmota/hardware.py; do not edit. */",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace newmota::generated::gpio {",
        f"inline constexpr std::array<char, {len(function_name_bytes)}> kFunctionNames{{{{",
        "  " + ", ".join(str(value) for value in function_name_bytes),
        "}};",
        (
            f"inline constexpr std::array<std::uint16_t, {len(function_name_offsets)}> "
            "kFunctionNameOffsets{{"
        ),
        "  " + ", ".join(f"{offset}u" for offset in function_name_offsets),
        "}};",
        f"inline constexpr std::array<char, {len(module_name_bytes)}> kModuleNames{{{{",
        "  " + ", ".join(str(value) for value in module_name_bytes),
        "}};",
        (
            f"inline constexpr std::array<std::uint16_t, {len(module_name_offsets)}> "
            "kModuleNameOffsets{{"
        ),
        "  " + ", ".join(f"{offset}u" for offset in module_name_offsets),
        "}};",
        "",
        "}  // namespace newmota::generated::gpio",
        "",
    ]
    write_if_changed(names_header, "\n".join(names_lines))

    document = {
        "schema": 1,
        "identity": identity,
        "inputs": inputs,
        "source": {**registry.source, "defines": list(registry.source["defines"])},
        "encoding": {
            "shift": registry.shift,
            "instance_mask": registry.instance_mask,
            "sensor_end": registry.sensor_end,
            "fixed_start": registry.fixed_start,
            "user_base": registry.user_base,
            "user_wire_value": registry.wire_value(registry.user_base),
            "template_user_value": registry.template_user_value,
        },
        "topology": topology.document,
        "ids": [
            {
                "base": item.base,
                "symbol": item.symbol,
                "wire_value": registry.wire_value(item.base),
            }
            for item in registry.identities
        ],
        "functions": [
            {
                "base": item.identity.base,
                "symbol": item.identity.symbol,
                "wire_value": registry.wire_value(item.identity.base),
                "display": item.display,
                "instances": item.instances,
                "owner": item.owner,
                "requires": list(item.requirements),
                "canonical": item.canonical,
                "modifiers": list(item.modifiers),
            }
            for item in active_functions
        ],
        "modules": [
            {
                "number": module.number,
                "name": module.name,
                "architecture": module.architecture,
                "flag": module.flag,
                "base": module.base,
                "requires": list(module.requirements),
                "provenance": module.provenance,
                "assignments": [
                    {
                        "slot": assignment.slot,
                        "symbol": assignment.symbol,
                        "instance": assignment.instance,
                        "template_value": assignment.template_value,
                    }
                    for assignment in module.assignments
                ],
            }
            for module in active_modules
        ],
    }
    write_if_changed(manifest, json.dumps(document, indent=2) + "\n")
    print(
        f"Hardware catalog: {len(registry.identities)} IDs, "
        f"{len(active_functions)} active functions, {len(active_modules)} modules "
        f"and {sum(pin.available for pin in topology.pins)}/{len(topology.pins)} available pins "
        f"({identity[:12]})"
    )
    return ResolvedHardwareCatalog(
        identity,
        header,
        names_header,
        topology.header,
        manifest,
        str(registry.source["revision"]),
    )
