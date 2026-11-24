"""Shared repository paths, data models, and native build contract."""

from __future__ import annotations

import hashlib
from abc import ABC, abstractmethod
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from newmota.config import ResolvedNativeConfig
    from newmota.features import ResolvedFeatures
    from newmota.hardware import ResolvedHardwareCatalog
    from newmota.host import SdkVersion
    from newmota.package import LayoutFacts

ROOT = Path(__file__).resolve().parents[2]
CONFIG_DIR = ROOT / "config"
ENV_DIR = CONFIG_DIR / "env"
USER_CONFIG_DIR = ROOT / "user_config"
ENV_MATRIX_DIR = ENV_DIR / "matrices"
BOARD_DIR = CONFIG_DIR / "boards"
CHIP_DIR = CONFIG_DIR / "chips"
PARTITIONS_DIR = CONFIG_DIR / "partitions"
HARDWARE_DIR = CONFIG_DIR / "hardware"
WORK_DIR = ROOT / "build" / "work"
FIRMWARE_DIR = ROOT / "build" / "firmware"

# Configuration directories use the native SDK name where that is clearer,
# while the public setup selector remains the shorter, established name.
PLATFORM_DIRECTORIES = {
    "esp": "esp-idf",
    "bouffalo": "bouffalo",
    "beken": "beken",
    "amebad": "amebad",
    "pico": "pico-sdk",
}
DIRECTORY_PLATFORMS = {directory: key for key, directory in PLATFORM_DIRECTORIES.items()}
PLATFORMS = tuple(PLATFORM_DIRECTORIES)


def esp_sdk_key(version: str | None) -> str:
    return "esp-idf-v6.1" if version == "6.1" else "esp-idf"


class CatalogError(Exception):
    """Raised when declarative chip, board, or environment data is invalid."""


class BuildError(Exception):
    """Raised when a firmware build cannot complete safely."""


class SetupUnavailable(Exception):
    """Raised when a requested SDK or toolchain cannot be prepared."""


@dataclass(frozen=True)
class ChipDefinition:
    """One supported public chip name and its native SDK target."""

    name: str
    platform: str
    platform_directory: str
    sdk_target: str
    build_status: str
    capability_overrides: dict[str, bool]
    capabilities: dict[str, bool | None]
    path: Path
    idf_version: str | None = None

    @property
    def sdk_key(self) -> str:
        return esp_sdk_key(self.idf_version) if self.platform == "esp" else self.platform_directory


@dataclass(frozen=True)
class MemoryDefinition:
    """Installed or required non-volatile/volatile external memory."""

    size_kb: int | None
    mode: str | None
    frequency_mhz: int | None


@dataclass(frozen=True)
class BoardGpioDefinition:
    """Optional board-level limits layered over an SDK chip topology."""

    source: str | None
    exposed: tuple[int, ...] | None
    reservations: tuple[tuple[str, tuple[int, ...]], ...]


@dataclass(frozen=True)
class BoardDefinition:
    """A build-driving board recipe independent of the default test matrix."""

    name: str
    platform: str
    platform_directory: str
    chip: ChipDefinition
    flash: MemoryDefinition
    psram: MemoryDefinition | None
    console: str | None
    sdk_board: str | None
    profile_default: str
    revision: int | None
    gpio: BoardGpioDefinition
    flash_layout: str | None
    capability_overrides: dict[str, bool]
    capabilities: dict[str, bool | None]
    data: dict[str, object]
    path: Path


@dataclass(frozen=True)
class FactoryFile:
    """One `filesystem` entry: a repository file and where the factory image puts it."""

    source: str
    target: str


@dataclass(frozen=True)
class Environment:
    name: str
    board: str
    policy: bool
    chip: str
    sdk_target: str
    build_status: str
    sdk_board: str | None
    role: str
    profile: str
    safeboot: str | None
    language: str
    flash_kb: int
    psram_kb: int | None
    console: str | None
    gpio: BoardGpioDefinition
    capabilities: dict[str, bool | None]
    platform: str
    platform_directory: str
    board_data: dict[str, object]
    native: dict[str, object]
    partition: Path | None
    path: Path | None
    crypto: object = None
    features: dict[str, bool | dict[str, int]] = field(default_factory=dict)
    device_name: str | None = None
    template: dict[str, object] | None = None
    solidify: tuple[str, ...] = ()
    filesystem: tuple[FactoryFile, ...] = ()
    base_environment: str | None = None
    configuration_paths: tuple[Path, ...] = ()
    idf_version: str | None = None
    idf_version_source: str | None = None
    recovery_base_environment: str | None = None

    @property
    def sdk_key(self) -> str:
        return esp_sdk_key(self.idf_version) if self.platform == "esp" else self.platform_directory

    def configuration_provenance(self) -> dict[str, object]:
        """Return build classification and hashed declarative inputs."""

        return {
            "build_class": "custom" if self.base_environment else "standard",
            "base_environment": self.base_environment,
            "idf_version": self.idf_version,
            "idf_version_source": self.idf_version_source,
            "recovery_base_environment": self.recovery_base_environment,
            "inputs": [
                {
                    "path": str(path.relative_to(ROOT)),
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                }
                for path in self.configuration_paths
            ],
        }


@dataclass(frozen=True)
class Artifact:
    """One verified output with an explicit role in the firmware lifecycle."""

    path: Path
    role: str


@dataclass(frozen=True)
class ApplicationUpdate:
    """One native application image and its fixed writable partition boundary."""

    image: Path
    offset: int
    size_limit: int


@dataclass(frozen=True)
class PackageResult:
    """Result of the package phase, including its qualification state."""

    status: str
    flashable: bool = False
    deployable: bool = False
    artifacts: tuple[Artifact, ...] = ()

    @classmethod
    def unqualified(cls) -> PackageResult:
        return cls(status="unqualified")


@dataclass
class BuildContext:
    environment: Environment
    version: str
    sdk: SdkVersion
    native_config: ResolvedNativeConfig
    features: ResolvedFeatures
    hardware: ResolvedHardwareCatalog
    layout: LayoutFacts | None
    work_dir: Path
    jobs: int
    build_datetime: str = ""
    artifacts: list[Artifact] = field(default_factory=list)
    package: PackageResult = field(default_factory=PackageResult.unqualified)

    @property
    def build_date(self) -> str:
        """Return the date component used by native compiler definitions."""

        return self.build_datetime.partition(" ")[0]

    @property
    def build_time(self) -> str:
        """Return the time component used by native compiler definitions."""

        return self.build_datetime.partition(" ")[2]


@dataclass(frozen=True)
class NativeConfigSpec:
    directory: str
    filename: str


class PlatformAdapter(ABC):
    native_config: NativeConfigSpec
    # SDK log level when SDK_LOG_LEVEL is not set; None keeps the global default.
    default_sdk_log_level: str | None = None

    def __init__(self, context: BuildContext) -> None:
        self.context = context

    @classmethod
    def board_native_settings(
        cls,
        environment: Environment,
        source: str,
        partition: Path | None = None,
    ) -> Mapping[str, Any]:
        return {}

    @classmethod
    def effective_sdk_log_level(cls, environment: Environment, requested: str) -> str:
        """Select the SDK log level under platform-specific image constraints."""

        return requested

    @classmethod
    def sdk_log_settings(cls, level: str) -> Mapping[str, Any]:
        """Translate the global SDK log policy into native settings."""

        return {}

    @classmethod
    def resolve_layout(
        cls, environment: Environment, sdk: SdkVersion, work_dir: Path
    ) -> LayoutFacts | None:
        """Resolve optional native storage facts before feature selection."""

        return None

    @abstractmethod
    def prepare(self) -> None:
        """Create writable projects, overlays, and platform work directories."""

    @abstractmethod
    def configure(self) -> None:
        """Apply configuration and initialize the native build system."""

    @abstractmethod
    def pre_build(self) -> None:
        """Generate Newmota-owned inputs required before compilation."""

    @abstractmethod
    def build(self) -> None:
        """Compile and link with the real platform SDK and cross-toolchain."""

    def build_application(self) -> None:
        """Build only the normal application image for a wired update."""

        raise BuildError(
            f"{self.context.environment.platform} has no qualified application-only build"
        )

    def application_update(self) -> ApplicationUpdate:
        """Validate and describe the application-only build output."""

        raise BuildError(
            f"{self.context.environment.platform} has no qualified application-only output"
        )

    @abstractmethod
    def compilation_database(self) -> Path:
        """Generate and return native compile commands for project-owned sources."""

    @abstractmethod
    def post_build(self) -> tuple[Artifact, ...]:
        """Verify and explicitly classify raw native SDK outputs."""

    @abstractmethod
    def package(self) -> PackageResult:
        """Create qualified full and OTA firmware, or report unqualified."""


# WPA3 reaches ESP builds through ESP-IDF's WPA supplicant. Builds below 4 MiB flash are built
# without it. The C5, C6 and C61 Safeboot images fit the fixed 832 KiB recovery slot of
# Tasmota's 4 MiB layout with WPA3 only when the supplicant runs on BearSSL.
WPA3_MINIMUM_FLASH_KB = 4096
WPA3_BEARSSL_ONLY_SAFEBOOT_CHIPS = frozenset({"esp32c5", "esp32c6", "esp32c61"})


def esp_keeps_wpa3(environment: Environment, bearssl: bool) -> bool:
    """Whether an ESP build keeps WPA3, with its supplicant on BearSSL or on mbedTLS."""
    if environment.flash_kb < WPA3_MINIMUM_FLASH_KB:
        return False
    return bearssl or not (
        environment.chip in WPA3_BEARSSL_ONLY_SAFEBOOT_CHIPS and environment.role == "safeboot"
    )
