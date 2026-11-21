"""Host-side tests for Beken CRC-wrapped and RBL image packaging."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from newmota.catalog import resolve_environment
from newmota.host import load_sdk_version
from newmota.model import ROOT
from newmota.platforms.beken.codec import (
    BekenImageError,
    build_application_image,
    build_ota_image,
    load_image_config,
    validate_application_image,
    validate_ota_image,
)
from newmota.platforms.beken.images import resolve_layout


def setUpModule() -> None:
    (ROOT / "build" / "host").mkdir(parents=True, exist_ok=True)


class BekenImageTest(unittest.TestCase):
    def test_openbeken_images_are_deterministic_and_validate(self) -> None:
        environment = resolve_environment("cb2s-2m")
        sdk = load_sdk_version("beken")
        config = load_image_config(environment)
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            root = Path(temporary)
            layout = resolve_layout(environment, sdk, root / "layout-cache")
            raw = root / "raw.bin"
            raw.write_bytes(bytes(range(256)) * 64)
            application_a = root / "application-a.rbl"
            application_b = root / "application-b.rbl"
            ota_a = root / "application-a.ota.rbl"
            ota_b = root / "application-b.ota.rbl"

            build_application_image(raw, application_a, config, "0.0.1")
            build_application_image(raw, application_b, config, "0.0.1")
            build_ota_image(raw, ota_a, config, "0.0.1", layout)
            build_ota_image(raw, ota_b, config, "0.0.1", layout)

            self.assertEqual(application_a.read_bytes(), application_b.read_bytes())
            self.assertEqual(ota_a.read_bytes(), ota_b.read_bytes())
            application_metadata = validate_application_image(
                application_a, config, layout.region("application").size
            )
            ota_metadata = validate_ota_image(ota_a, config, layout.region("ota").size, layout)

        self.assertEqual(application_metadata["format"], "app-rbl")
        self.assertEqual(application_metadata["size"], config.application_physical_size)
        self.assertEqual(ota_metadata["format"], "rbl")
        self.assertEqual(ota_metadata["compression"], "gzip")
        self.assertEqual(ota_metadata["encryption"], "aes256")

    def test_raw_linker_output_is_not_an_ota_image(self) -> None:
        environment = resolve_environment("cb2s-2m")
        config = load_image_config(environment)
        with tempfile.TemporaryDirectory() as temporary:
            raw = Path(temporary) / "raw.bin"
            raw.write_bytes(b"\x0e\x00\x00\xea" * 16)
            sdk = load_sdk_version("beken")
            layout = resolve_layout(environment, sdk, Path(temporary) / "layout-cache")
            with self.assertRaisesRegex(BekenImageError, "RBL header"):
                validate_ota_image(raw, config, None, layout)

    def test_ota_rbl_is_bound_to_exact_chip_layout_and_image_policy(self) -> None:
        environment = resolve_environment("cb2s-2m")
        sdk = load_sdk_version("beken")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            layout = resolve_layout(environment, sdk, root / "cb2s-layout")
            raw = root / "raw.bin"
            raw.write_bytes(bytes(range(256)) * 64)
            image = root / "application.ota.rbl"
            config = load_image_config(environment)
            build_ota_image(raw, image, config, "0.0.1", layout)

            other_environment = resolve_environment("xh-cb3s")
            other_layout = resolve_layout(other_environment, sdk, root / "xh-layout")
            with self.assertRaisesRegex(BekenImageError, "compatibility tag"):
                validate_ota_image(
                    image,
                    load_image_config(other_environment),
                    other_layout.region("ota").size,
                    other_layout,
                )


if __name__ == "__main__":
    unittest.main()
