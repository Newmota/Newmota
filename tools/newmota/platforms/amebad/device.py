"""AmebaD wired-device adapter using the Python UART uploader."""

from __future__ import annotations

import json
from itertools import pairwise
from pathlib import Path

from newmota.device import DeviceAdapter, DeviceError, ProbeResult
from newmota.factory_package import FactoryPackage
from newmota.host import check_sdk_version
from newmota.model import ROOT, WORK_DIR, ApplicationUpdate, Environment
from newmota.package import FirmwarePackage, PackageError
from newmota.platforms.amebad.images import (
    BOOT_IMAGE_LAYOUT,
    DEFAULT_UPLOAD_BAUD,
    FLASH_BASE,
)
from newmota.platforms.amebad.uploader import AmebadUploadError, upload


class AmebadFlasher(DeviceAdapter):
    """Flash a paired AmebaD image set through UART download mode.

    The community protocol currently provides write/verify operations only.
    Probe and backup therefore remain deliberately unavailable until the read
    protocol and the board-specific recovery path have been qualified.
    """

    platform_name = "AmebaD"
    default_monitor_baud = 115200

    def probe(self, environment: Environment, port: str, baud: int | None) -> ProbeResult:
        del environment, port, baud
        raise DeviceError(
            "AmebaD probe is unavailable: the Python uploader does not identify "
            "the chip or flash capacity"
        )

    def backup(
        self,
        environment: Environment,
        port: str,
        output: Path,
        baud: int | None,
    ) -> None:
        del environment, port, output, baud
        raise DeviceError(
            "AmebaD backup is unavailable: the Python uploader has no flash-read "
            "operation; do not flash before securing a recovery image"
        )

    @staticmethod
    def _images(package: FirmwarePackage) -> tuple[list[tuple[str, int, Path]], Path, int, bool]:
        flash = package.data.get("flash")
        if not isinstance(flash, dict) or flash.get("protocol") != "amebad-uart":
            raise PackageError("package has no AmebaD UART flash plan")
        loader_name = flash.get("loader")
        raw_components = flash.get("components")
        speed = flash.get("baud")
        auto_reset = flash.get("auto_reset", False)
        if not isinstance(loader_name, str) or not isinstance(raw_components, list):
            raise PackageError("AmebaD flash plan lacks its loader or components")
        if not isinstance(speed, int) or isinstance(speed, bool):
            raise PackageError("AmebaD flash plan has an invalid baud rate")
        if not isinstance(auto_reset, bool):
            raise PackageError("AmebaD flash plan has an invalid auto-reset setting")

        layout = package.data.get("layout")
        if not isinstance(layout, dict) or layout.get("flash_bytes") != package.flash_bytes:
            raise PackageError("AmebaD package has no valid flash layout metadata")

        def region(name: str) -> tuple[int, int]:
            raw = layout.get(name)
            if not isinstance(raw, dict):
                raise PackageError(f"AmebaD package layout is missing {name}")
            offset = raw.get("offset")
            size = raw.get("size")
            if (
                not isinstance(offset, int)
                or isinstance(offset, bool)
                or not isinstance(size, int)
                or isinstance(size, bool)
                or offset < 0
                or size <= 0
                or offset + size > package.flash_bytes
            ):
                raise PackageError(f"AmebaD package layout has an invalid {name} region")
            return offset, size

        ota1_offset, ota1_size = region("ota1")
        ota2_offset, ota2_size = region("ota2")
        filesystem_offset, filesystem_size = region("filesystem")
        expected = {image.name: image.offset for image in BOOT_IMAGE_LAYOUT}
        expected.update(
            {
                "safeboot-image2.bin": ota1_offset,
                "application-image2.bin": ota2_offset,
                "littlefs.bin": filesystem_offset,
            }
        )
        limits = {
            "km0_boot_all.bin": ota1_offset,
            "km4_boot_all.bin": ota1_offset - 0x4000,
            "safeboot-image2.bin": ota1_size,
            "application-image2.bin": ota2_size,
            "littlefs.bin": filesystem_size,
        }
        parsed: dict[str, tuple[int, Path]] = {}
        for index, raw in enumerate(raw_components):
            if not isinstance(raw, dict):
                raise PackageError(f"AmebaD flash component {index} is invalid")
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
                raise PackageError(f"AmebaD flash component {index} is incomplete")
            if name not in expected:
                raise PackageError(f"AmebaD flash plan contains an unexpected image: {name}")
            if role not in {"factory", "factory-component"}:
                raise PackageError(f"AmebaD image is not a factory artifact: {name}")
            if offset != expected[name] or name in parsed:
                raise PackageError(f"AmebaD image has an invalid or duplicate offset: {name}")
            path = package.file(path_name)
            size = path.stat().st_size
            if not size or offset + size > package.flash_bytes:
                raise DeviceError(f"AmebaD image exceeds the package flash capacity: {name}")
            if size > limits[name]:
                raise DeviceError(f"AmebaD image exceeds its partition: {name}")
            parsed[name] = (offset, path)

        if set(parsed) != set(expected):
            missing = sorted(set(expected) - set(parsed))
            raise PackageError("AmebaD flash plan is missing: " + ", ".join(missing))
        ranges = sorted(
            (offset, offset + path.stat().st_size, name) for name, (offset, path) in parsed.items()
        )
        if any(left[1] > right[0] for left, right in pairwise(ranges)):
            raise PackageError("AmebaD factory flash components overlap")
        images = [(name, expected[name], parsed[name][1]) for name in expected]
        return images, package.file(loader_name), speed, auto_reset

    @staticmethod
    def _validate_target(environment: Environment, package: FirmwarePackage) -> None:
        target = package.data.get("target")
        if not isinstance(target, dict):
            raise PackageError("AmebaD package has no target metadata")
        if (
            target.get("environment") != environment.name
            or target.get("chip") != environment.chip
            or target.get("sdk_target") != environment.sdk_target
        ):
            raise DeviceError("AmebaD package does not match the selected catalog environment")

    def flash(
        self,
        environment: Environment,
        package: FirmwarePackage,
        port: str,
        baud: int | None,
    ) -> None:
        self._validate_target(environment, package)

        images, loader, package_baud, auto_reset = self._images(package)
        try:
            upload(
                port,
                loader,
                images,
                baud or package_baud,
                auto_reset=auto_reset,
            )
        except AmebadUploadError as error:
            raise DeviceError(str(error)) from error
        except OSError as error:
            raise DeviceError(f"AmebaD upload failed: {error}") from error

    def flash_factory(
        self,
        environment: Environment,
        factory: FactoryPackage,
        port: str,
        baud: int | None,
    ) -> None:
        """Write a factory package from its description alone, as the web flasher does."""

        factory.steps((), "AmebaD")
        # The flashloader comes from the pinned SDK, as for update(); the package carries none.
        sdk = check_sdk_version("amebad")
        loader = sdk.source / "tools" / "AmebaD" / "Image_Tool" / "imgtool_flashloader_amebad.bin"
        if not loader.is_file():
            raise DeviceError(f"AmebaD flashloader is missing: {loader}")
        images = factory.write_images(WORK_DIR / environment.name / "flash" / "factory-package")
        try:
            upload(port, loader, images, baud or DEFAULT_UPLOAD_BAUD, auto_reset=False)
        except AmebadUploadError as error:
            raise DeviceError(str(error)) from error
        except OSError as error:
            raise DeviceError(f"AmebaD upload failed: {error}") from error

    def update(
        self,
        environment: Environment,
        update: ApplicationUpdate,
        port: str,
        baud: int | None,
    ) -> None:
        """Write and verify only the coupled application image in OTA2."""

        if environment.role != "application":
            raise DeviceError("AmebaD application update requires an application environment")
        if update.offset < 0 or update.offset % 0x1000:
            raise DeviceError("AmebaD application update offset is invalid")
        if update.offset + update.size_limit > environment.flash_kb * 1024:
            raise DeviceError("AmebaD application update partition exceeds declared flash")
        if not update.image.is_file():
            raise DeviceError(f"AmebaD application update image is missing: {update.image}")
        image_size = update.image.stat().st_size
        if image_size == 0 or image_size > update.size_limit:
            raise DeviceError("AmebaD application update image does not fit OTA2")

        sdk = check_sdk_version("amebad")
        loader = sdk.source / "tools" / "AmebaD" / "Image_Tool" / "imgtool_flashloader_amebad.bin"
        if not loader.is_file():
            raise DeviceError(f"AmebaD flashloader is missing: {loader}")
        plan_path = WORK_DIR / environment.name / "update-plan.json"
        plan_path.parent.mkdir(parents=True, exist_ok=True)
        plan_path.write_text(
            json.dumps(
                {
                    "environment": environment.name,
                    "protocol": "amebad-uart",
                    "partition": "ota2",
                    "offset": update.offset,
                    "address": FLASH_BASE + update.offset,
                    "size": image_size,
                    "size_limit": update.size_limit,
                    "image": str(update.image.relative_to(ROOT)),
                    "loader": str(loader.relative_to(ROOT)),
                    "preserves": ["boot-images", "ota1-safeboot", "littlefs"],
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"AmebaD update plan: {plan_path.relative_to(ROOT)}")
        print(
            "WARNING: AmebaD OTA2 update writes only the application slot; chip identification "
            "and flash backup are not implemented.",
            flush=True,
        )
        try:
            upload(
                port,
                loader,
                [("application-image2.bin", update.offset, update.image)],
                baud or DEFAULT_UPLOAD_BAUD,
                auto_reset=False,
            )
        except AmebadUploadError as error:
            raise DeviceError(str(error)) from error
        except OSError as error:
            raise DeviceError(f"AmebaD update failed: {error}") from error
