"""Beken SDK target profiles and source-tree discovery."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from newmota.host import SdkVersion
from newmota.model import BuildError, Environment


class BekenProfileError(BuildError):
    """Raised when a Beken SDK checkout cannot satisfy a target profile."""


@dataclass(frozen=True)
class BekenTargetSpec:
    """Target facts audited from the common BDK builder and configuration headers."""

    sdk_target: str
    config_target: str
    bdk_soc: str
    library_suffix: str
    linker_script: str
    boot_handler: str
    ble_version: str | None
    ble_platform: str | None
    minimum_bdk: tuple[int, int, int] | None


@dataclass(frozen=True)
class BekenTargetProfile:
    """Resolved inputs for one Beken target in one checked-out SDK tree."""

    spec: BekenTargetSpec
    sdk_kind: str
    sdk_root: Path
    source_root: Path
    partition_root: Path
    printf_source: Path | None
    linker_source: Path | None
    build_supported: bool
    unsupported_reason: str | None

    def native_partition_source(self, relative: str) -> Path:
        """Resolve a partition source below the selected SDK source root."""

        path = (self.partition_root / relative).resolve()
        try:
            path.relative_to(self.partition_root.resolve())
        except ValueError as error:
            raise BekenProfileError(
                f"Beken native partition source escapes the selected SDK target: {relative}"
            ) from error
        return path

    def require_build_support(self) -> None:
        """Stop before build orchestration if the SDK needs the future BDK wrapper."""

        if not self.build_supported:
            reason = self.unsupported_reason or "target build wrapper is not implemented"
            raise BekenProfileError(
                f"Beken target {self.spec.sdk_target} is discovered but not buildable: {reason}"
            )

    def metadata(self) -> dict[str, object]:
        """Return manifest-safe target facts without host-specific absolute paths."""

        minimum = self.spec.minimum_bdk
        return {
            "kind": self.sdk_kind,
            "sdk_target": self.spec.sdk_target,
            "config_target": self.spec.config_target,
            "bdk_soc": self.spec.bdk_soc,
            "library_suffix": self.spec.library_suffix,
            "linker_script": self.spec.linker_script,
            "boot_handler": self.spec.boot_handler,
            "ble_version": self.spec.ble_version,
            "ble_platform": self.spec.ble_platform,
            "minimum_bdk": ".".join(str(part) for part in minimum) if minimum else None,
            "build_supported": self.build_supported,
        }


# These are selection facts, not a claim that each target is currently supported by
# Newmota. They mirror the BDK 3.0.78/LibreTiny target matrix used for qualification.
TARGET_SPECS = {
    "bk7231n": BekenTargetSpec(
        "bk7231n",
        "bk7231n",
        "SOC_BK7231N",
        "bk7231n",
        "bk7231n_bsp.lds",
        "boot_handlers.S",
        "5.1",
        "7231n",
        (3, 0, 4),
    ),
    "bk7231t": BekenTargetSpec(
        "bk7231t",
        "bk7231u",
        "SOC_BK7231U",
        "bk7231u",
        "bk7231_bsp.ld",
        "boot_handlers.S",
        "4.2",
        None,
        None,
    ),
    "bk7231q": BekenTargetSpec(
        "bk7231q",
        "bk7231q",
        "SOC_BK7231",
        "bk7231",
        "bk7231_bsp.ld",
        "boot_handlers_bk7231u.S",
        None,
        None,
        None,
    ),
    "bk7238": BekenTargetSpec(
        "bk7238",
        "bk7238",
        "SOC_BK7238",
        "bk7238",
        "bk7238_bsp.lds",
        "boot_handlers.S",
        "5.2",
        "bk7238",
        (3, 0, 56),
    ),
    "bk7252": BekenTargetSpec(
        "bk7252",
        "bk7251",
        "SOC_BK7221U",
        "bk7251",
        "bk7231_bsp.ld",
        "boot_handlers.S",
        "4.2",
        None,
        (3, 0, 11),
    ),
    "bk7252n": BekenTargetSpec(
        "bk7252n",
        "bk7252n",
        "SOC_BK7252N",
        "bk7252n",
        "bk7252n_bsp.lds",
        "boot_handlers.S",
        "5.2",
        "bk7252n",
        None,
    ),
}


# Targets the common BDK wrapper builds; the others are selection facts only.
BUILD_TARGETS = frozenset({"bk7231n", "bk7231t", "bk7238", "bk7252", "bk7252n"})
# Targets whose prebuilt supplicant takes its crypto from the source-built crypto_mbedtls.c,
# which USE_WIFI_WPA3_BEARSSL replaces. The BK7231T (bk7231u) and BK7252 (bk7251) archives
# carry their own wolfSSL-based crypto (crypto_wolfssl.o, sha1-internal.o, ...).
BEARSSL_SUPPLICANT_TARGETS = frozenset({"bk7231n", "bk7238", "bk7252n"})


def _target_spec(environment: Environment) -> BekenTargetSpec:
    try:
        return TARGET_SPECS[environment.sdk_target]
    except KeyError as error:
        raise BekenProfileError(
            f"Beken has no target profile for SDK target {environment.sdk_target!r}"
        ) from error


def _bdk_profile(sdk: SdkVersion, spec: BekenTargetSpec) -> BekenTargetProfile | None:
    source_root = sdk.source / "beken378"
    if not source_root.is_dir():
        return None
    linker_source = sdk.source / "build" / spec.linker_script
    config_header = source_root / "app" / "config" / f"sys_config_{spec.config_target}.h"
    boot_handler = source_root / "driver" / "entry" / spec.boot_handler
    library_names = (
        "libcal_{suffix}.a",
        "librf_test_{suffix}.a",
        "librf_use_{suffix}.a",
        "librwnx_{suffix}.a",
        "libsupplicant_{suffix}.a",
        "libuart_debug_{suffix}.a",
    )
    missing = [
        str(path.relative_to(sdk.source))
        for path in (config_header, linker_source, boot_handler)
        if not path.is_file()
    ]
    missing.extend(
        f"beken378/lib/{name.format(suffix=spec.library_suffix)}"
        for name in library_names
        if not (source_root / "lib" / name.format(suffix=spec.library_suffix)).is_file()
    )
    minimum = spec.minimum_bdk
    try:
        actual_version = tuple(
            int(part) for part in getattr(sdk, "version", "0.0.0").split(".")[:3]
        )
    except ValueError:
        actual_version = ()
    version_supported = minimum is None or actual_version >= minimum
    if minimum is not None and not version_supported:
        missing.append(f"BDK >= {'.'.join(str(part) for part in minimum)}")
    return BekenTargetProfile(
        spec=spec,
        sdk_kind="bdk-common",
        sdk_root=sdk.source,
        source_root=source_root,
        partition_root=source_root,
        printf_source=source_root / "driver" / "uart" / "printf.c",
        linker_source=linker_source if linker_source.is_file() else None,
        build_supported=(spec.sdk_target in BUILD_TARGETS and not missing and version_supported),
        unsupported_reason=(
            None
            if spec.sdk_target in BUILD_TARGETS and not missing and version_supported
            else "missing or unsupported BDK inputs: " + ", ".join(missing)
        ),
    )


def resolve_target_profile(environment: Environment, sdk: SdkVersion) -> BekenTargetProfile:
    """Resolve an exact target profile from the SDK tree without chip aliases."""

    spec = _target_spec(environment)
    profile = _bdk_profile(sdk, spec)
    if profile is not None:
        return profile
    raise BekenProfileError(
        f"Beken SDK has no source tree for target {spec.sdk_target}; "
        "select a verified SDK target profile before building"
    )
