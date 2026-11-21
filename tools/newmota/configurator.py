"""Edit a user build configuration on top of the real feature resolver.

`make config` and its scripted form drive `ConfigSession`. It never repeats feature policy:
whether a selection is legal, which dependencies it pulls in, and what a profile enables are
all answered by `features.feature_values`, the function the build itself uses.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field, replace
from pathlib import Path

from newmota.catalog import (
    TARGET_ID,
    CatalogError,
    _read_environment_yaml,
    load_environments,
    resolve_user_environment,
    user_reserved_names,
)
from newmota.feature_docs import FEATURE_DOCS, GROUPS
from newmota.features import CORE_FEATURES, FEATURE_DEPENDENCIES, OPTIONAL_FEATURES, feature_values
from newmota.model import ROOT, USER_CONFIG_DIR, BuildError, Environment

SELECTABLE = (*OPTIONAL_FEATURES, *CORE_FEATURES)
# feature_values also forces USE_DISPLAY for USE_LVGL, outside the dependency table.
DEPENDENCIES = {
    **FEATURE_DEPENDENCIES,
    "USE_LVGL": (*FEATURE_DEPENDENCIES["USE_LVGL"], "USE_DISPLAY"),
}
PROFILES = ("Compact", "Standard", "Large")
LANGUAGE_DIR = ROOT / "language"


def languages() -> list[str]:
    """Return the build-time locale IDs."""

    return sorted(path.stem for path in LANGUAGE_DIR.glob("*.toml"))


def user_files() -> list[Path]:
    """Return the user configuration files, ordered by name."""

    return sorted(USER_CONFIG_DIR.glob("*.yaml"))


def dependency_closure(name: str) -> set[str]:
    """Return every feature that `name` needs, directly or through other features."""

    seen: set[str] = set()
    pending = [name]
    while pending:
        for dependency in DEPENDENCIES.get(pending.pop(), ()):
            if dependency not in seen:
                seen.add(dependency)
                pending.append(dependency)
    return seen


def application_environments() -> list[Environment]:
    """Return the checked-in applications a user build can start from."""

    return sorted(
        (item for item in load_environments().values() if item.role == "application"),
        key=lambda item: (item.platform_directory, item.name),
    )


def platforms() -> list[str]:
    """Return the platform families that have an application environment."""

    return list(dict.fromkeys(item.platform_directory for item in application_environments()))


def chips(platform: str) -> list[str]:
    """Return the chips of one platform family that have an application environment."""

    return sorted(
        {item.chip for item in application_environments() if item.platform_directory == platform}
    )


def bases(platform: str, chip: str | None = None) -> list[Environment]:
    """Return the application environments for a platform family and optionally one chip."""

    return [
        item
        for item in application_environments()
        if item.platform_directory == platform and chip in (None, item.chip)
    ]


def default_base(platform: str, chip: str | None = None) -> Environment:
    """Pick the natural start: the environment named after the chip, else the policy one."""

    candidates = bases(platform, chip)
    for item in candidates:
        if chip and item.name == chip:
            return item
    return next((item for item in candidates if item.policy), candidates[0])


@dataclass(frozen=True)
class FeatureState:
    """How one selectable feature stands in the current configuration."""

    name: str
    enabled: bool
    # selected: chosen here; default: on in the base; implied: pulled in by `required_by`;
    # disabled: switched off here; blocked: cannot be enabled; off: simply off.
    kind: str
    group: str
    summary: str
    needs: tuple[str, ...]
    required_by: tuple[str, ...]
    reason: str = ""


@dataclass
class ConfigSession:
    """One user configuration: a base environment plus the user's own choices."""

    base: Environment
    name: str
    profile: str
    language: str
    requested: dict[str, bool | dict[str, int]] = field(default_factory=dict)
    extras: dict[str, object] = field(default_factory=dict)
    loaded_path: Path | None = None
    # The suggested name follows the base until the user names the configuration.
    auto_name: bool = True
    # Settings written under a feature's key, kept while the feature is switched off and on.
    feature_settings: dict[str, dict[str, int]] = field(default_factory=dict)
    _cache: dict[tuple, tuple[dict[str, bool] | None, str]] = field(default_factory=dict)

    @classmethod
    def from_base(cls, base: Environment) -> ConfigSession:
        """Start a new configuration from a checked-in application environment."""

        return cls(base, suggest_name(base), base.profile, base.language)

    @classmethod
    def from_file(cls, path: Path) -> ConfigSession:
        """Reopen an existing user configuration."""

        child = _read_environment_yaml(path)
        official = load_environments()
        base = official.get(str(child.get("inherit")))
        if base is None:
            raise CatalogError(f"{path.name}: inherit must name a checked-in environment")
        features = child.get("features", {})
        if not isinstance(features, dict) or not all(
            isinstance(value, (bool, dict)) for value in features.values()
        ):
            raise CatalogError(
                f"{path.name}: features must map USE_* names to booleans or settings"
            )
        extras = {
            key: child[key]
            for key in (
                "crypto",
                "native",
                "idf_version",
                "device_name",
                "template",
                "solidify",
                "filesystem",
            )
            if key in child
        }
        session = cls(
            base,
            str(child.get("name", path.stem)),
            str(child.get("profile", base.profile)),
            str(child.get("language", base.language)),
            dict(features),
            extras,
            path,
            auto_name=False,
            feature_settings={
                name: value for name, value in features.items() if isinstance(value, dict)
            },
        )
        session.requested = session._normalized(session.requested)
        return session

    def retarget(self, base: Environment) -> tuple[ConfigSession, list[str]]:
        """Move the choices to another base; return the new session and what no longer fits."""

        profile = self.profile if self.profile != self.base.profile else base.profile
        language = self.language if self.language != self.base.language else base.language
        extras = self.extras if base.platform == self.base.platform else {}
        name = suggest_name(base) if self.auto_name else self.name
        other = ConfigSession(
            base,
            name,
            profile,
            language,
            extras=dict(extras),
            loaded_path=self.loaded_path,
            auto_name=self.auto_name,
            feature_settings=dict(self.feature_settings),
        )
        dropped = [] if extras == self.extras else sorted(self.extras)
        for name, value in self.requested.items():
            if other.enable(name) if value else other.disable(name):
                dropped.append(name)
        return other, dropped

    # -- resolution ---------------------------------------------------------------------

    def environment(self, requested: dict[str, bool] | None = None, profile: str | None = None):
        """Return the environment the build would see for these choices."""

        chosen = self.requested if requested is None else requested
        return replace(
            self.base,
            name=self.name,
            profile=profile or self.profile,
            language=self.language,
            features={**self.base.features, **chosen},
            base_environment=self.base.name,
        )

    def _resolve(
        self, requested: dict[str, bool], profile: str | None = None
    ) -> tuple[dict[str, bool] | None, str]:
        key = (profile or self.profile, repr(sorted(requested.items())))
        if key not in self._cache:
            try:
                self._cache[key] = (feature_values(self.environment(requested, profile)), "")
            except BuildError as error:
                self._cache[key] = (None, str(error))
        return self._cache[key]

    def _normalized(self, requested: dict[str, bool], profile: str | None = None):
        baseline, _ = self._resolve({}, profile)
        return {
            name: value
            for name, value in sorted(requested.items())
            if baseline is None or baseline.get(name) != value
        }

    @property
    def values(self) -> dict[str, bool]:
        """The resolved feature values, or the baseline when the choices are invalid."""

        values, _ = self._resolve(self.requested)
        return values if values is not None else self._resolve({})[0] or {}

    @property
    def error(self) -> str:
        """Why the current choices do not resolve; empty when they do."""

        return self._resolve(self.requested)[1]

    @property
    def baseline(self) -> dict[str, bool]:
        """The feature values of the unmodified base at the current profile."""

        return self._resolve({})[0] or {}

    def _is_own(self, name: str) -> bool:
        """Whether a feature is on by choice or by base default, not through a dependency."""

        return self.values.get(name, False) and (
            self._chosen(name) or self.baseline.get(name, False)
        )

    def _chosen(self, name: str) -> bool:
        """Whether this configuration turns a feature on, with or without settings."""

        return self.requested.get(name) not in (None, False)

    def required_by(self, name: str) -> tuple[str, ...]:
        """Enabled features that list `name` as a direct dependency."""

        return tuple(
            other
            for other in SELECTABLE
            if self.values.get(other) and name in DEPENDENCIES.get(other, ())
        )

    def state(self, name: str) -> FeatureState:
        """Classify a feature for display."""

        group, summary = FEATURE_DOCS[name]
        enabled = self.values.get(name, False)
        needs = tuple(DEPENDENCIES.get(name, ()))
        users = self.required_by(name)
        reason = ""
        if enabled:
            if self._chosen(name):
                kind = "selected"
            elif self.baseline.get(name, False):
                kind = "default"
            else:
                kind = "implied"
        elif self.requested.get(name) is False:
            kind = "disabled"
        else:
            kind = "off"
            blocked = self._enable_error(name)
            if blocked:
                kind, reason = "blocked", blocked
        return FeatureState(name, enabled, kind, group, summary, needs, users, reason)

    def _enable_candidate(self, name: str) -> dict[str, bool]:
        candidate = dict(self.requested)
        for dependency in dependency_closure(name):
            if candidate.get(dependency) is False:
                del candidate[dependency]
        candidate[name] = self.feature_settings.get(name, True)
        return candidate

    def _enable_error(self, name: str) -> str:
        return self._resolve(self._enable_candidate(name))[1]

    # -- editing ------------------------------------------------------------------------

    def enable(self, name: str) -> str:
        """Select a feature and, implicitly, what it needs. Return a problem, or ''."""

        candidate = self._enable_candidate(name)
        _, error = self._resolve(candidate)
        if error:
            return error
        self.requested = self._normalized(candidate)
        return ""

    def disable(self, name: str) -> str:
        """Deselect a feature and the features chosen on top of it. Return a problem, or ''."""

        if not self.values.get(name):
            return ""
        if not self._is_own(name):
            return f"{name} is required by {', '.join(self.required_by(name))}; turn that off first"
        candidate = dict(self.requested)
        for other in SELECTABLE:
            if other == name or not self._is_own(other):
                continue
            if name in dependency_closure(other):
                candidate[other] = False
        candidate[name] = False
        _, error = self._resolve(candidate)
        if error:
            return error
        self.requested = self._normalized(candidate)
        return ""

    def toggle(self, name: str) -> str:
        """Flip a feature. Return a problem, or ''."""

        return self.disable(name) if self.values.get(name) else self.enable(name)

    def set_profile(self, profile: str) -> str:
        """Change the feature baseline. Return a problem, or ''."""

        values, error = self._resolve(self.requested, profile)
        if values is None:
            return error
        self.requested = self._normalized(self.requested, profile)
        self.profile = profile
        return ""

    # -- output -------------------------------------------------------------------------

    def implied(self) -> list[str]:
        """Features that are on only because something selected needs them."""

        return [
            name
            for name in SELECTABLE
            if self.values.get(name)
            and not self.baseline.get(name, False)
            and not self._chosen(name)
        ]

    @property
    def path(self) -> Path:
        return USER_CONFIG_DIR / f"{self.name}.yaml"

    def child(self) -> dict[str, object]:
        """The user-configuration mapping, omitting what the base already says."""

        child: dict[str, object] = {"name": self.name, "inherit": self.base.name}
        if self.profile != self.base.profile:
            child["profile"] = self.profile
        if self.language != self.base.language:
            child["language"] = self.language
        child.update(self.extras)
        if self.requested:
            child["features"] = dict(self.requested)
        return child

    def check(self) -> str:
        """Validate the configuration as the build would. Return a problem, or ''."""

        if not TARGET_ID.fullmatch(self.name):
            return "name must be lowercase kebab-case, for example my-device"
        official = load_environments()
        reserved = user_reserved_names(official)
        for other in user_files():
            if other != self.path and other != self.loaded_path:
                reserved.add(other.stem)
        try:
            environment = resolve_user_environment(self.path, self.child(), official, reserved)
            feature_values(environment)
        except (CatalogError, BuildError) as error:
            return str(error)
        return ""

    def document(self) -> str:
        """Render the user configuration file."""

        import yaml

        child = self.child()
        lines = [
            "# User build configuration written by `make config`.",
            f"# Build it with: make build ENV={self.name}",
        ]
        for key, value in child.items():
            if key == "features":
                lines.append("features:")
                for name, on in value.items():
                    if isinstance(on, dict):
                        lines.append(f"  {name}:")
                        lines.extend(f"    {setting}: {number}" for setting, number in on.items())
                    else:
                        lines.append(f"  {name}: {str(on).lower()}")
            elif key == "template":
                lines.append(
                    f"template: {json.dumps(value, ensure_ascii=False, separators=(',', ':'))}"
                )
            elif isinstance(value, (dict, list)) or key == "device_name":
                lines.append(
                    yaml.safe_dump({key: value}, sort_keys=False, allow_unicode=True).rstrip()
                )
            else:
                lines.append(f"{key}: {value}")
        implied = self.implied()
        if implied:
            lines.append("# Selected automatically through dependencies: " + ", ".join(implied))
        return "\n".join(lines) + "\n"

    def write(self, overwrite: bool = False) -> str:
        """Validate and write the file. Return a problem, or ''."""

        if problem := self.check():
            return problem
        if self.path.exists() and not overwrite and self.path != self.loaded_path:
            return f"{self.path.relative_to(ROOT)} already exists"
        USER_CONFIG_DIR.mkdir(exist_ok=True)
        self.path.write_text(self.document(), encoding="utf-8")
        self.loaded_path = self.path
        return ""


def suggest_name(base: Environment) -> str:
    """Return an unused user configuration name derived from the base."""

    taken = user_reserved_names(load_environments()) | {path.stem for path in user_files()}
    stem = f"my-{base.name}"
    candidate, number = stem, 2
    while candidate in taken:
        candidate, number = f"{stem}-{number}", number + 1
    return candidate


def grouped_features() -> list[tuple[str, list[str]]]:
    """Return the selectable features by group, in display order."""

    return [
        (group, [name for name in SELECTABLE if FEATURE_DOCS[name][0] == group]) for group in GROUPS
    ]
