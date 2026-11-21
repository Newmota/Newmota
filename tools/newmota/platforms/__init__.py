"""One explicit, lazy registry for native builds, setup, and device operations."""

from __future__ import annotations

from dataclasses import dataclass
from importlib import import_module
from pathlib import Path
from types import ModuleType
from typing import TYPE_CHECKING

from ..model import ROOT, BuildError, SetupUnavailable

if TYPE_CHECKING:
    from ..device import DeviceAdapter
    from ..model import PlatformAdapter


@dataclass(frozen=True)
class PlatformDefinition:
    key: str
    sdk_key: str
    package: str
    adapter_name: str
    flasher_name: str

    @property
    def build(self) -> ModuleType:
        return import_module(f"{self.package}.build")

    @property
    def adapter(self) -> type[PlatformAdapter]:
        return getattr(self.build, self.adapter_name)

    def setup(self, chips: list[str], dry_run: bool, sdk_key: str | None = None) -> None:
        if self.key == "esp" and sdk_key is not None:
            self.build.setup(chips, dry_run, sdk_key=sdk_key)
        else:
            self.build.setup(chips, dry_run)

    def setup_issue(self, chips: list[str], sdk_key: str | None = None) -> str | None:
        if self.key == "esp" and sdk_key is not None:
            return self.build.setup_issue(chips, sdk_key=sdk_key)
        return self.build.setup_issue(chips)

    def setup_checkout(self, dry_run: bool, sdk_key: str) -> None:
        self.build.setup_checkout(dry_run, sdk_key)

    def flasher(self) -> DeviceAdapter:
        return getattr(import_module(f"{self.package}.device"), self.flasher_name)()

    def build_input_paths(self) -> tuple[Path, ...]:
        """Track shared tooling and all selected-platform helpers, including new ones."""
        root = ROOT / "tools" / "newmota"
        return tuple(
            sorted(
                (
                    *root.glob("*.py"),
                    root / "platforms" / "__init__.py",
                    *(root / "platforms" / self.key).glob("*.py"),
                )
            )
        )


def _registry() -> dict[str, PlatformDefinition]:
    from .amebad import PLATFORM as amebad
    from .beken import PLATFORM as beken
    from .bouffalo import PLATFORM as bouffalo
    from .esp import PLATFORM as esp
    from .pico import PLATFORM as pico

    return {item.key: item for item in (esp, bouffalo, beken, amebad, pico)}


PLATFORM_REGISTRY = _registry()


def platform_for_build(key: str) -> PlatformDefinition:
    try:
        return PLATFORM_REGISTRY[key]
    except KeyError as error:
        raise BuildError(f"platform '{key}' has no native build adapter") from error


def platform_for_setup(key: str) -> PlatformDefinition:
    try:
        return PLATFORM_REGISTRY[key]
    except KeyError as error:
        raise SetupUnavailable(f"unknown setup platform: {key}") from error


def flasher_for(platform: str) -> DeviceAdapter:
    from ..device import DeviceError

    try:
        definition = PLATFORM_REGISTRY[platform]
    except KeyError as error:
        raise DeviceError(f"platform '{platform}' has no wired device adapter") from error
    return definition.flasher()
