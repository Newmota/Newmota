"""Host-side checks for ESP image compatibility metadata."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from newmota.model import ROOT
from newmota.package import PackageError
from newmota.platforms.esp.images import (
    ESP_CUSTOM_DESCRIPTOR,
    ESP_CUSTOM_DESCRIPTOR_APPLICATION,
    ESP_CUSTOM_DESCRIPTOR_MAGIC,
    ESP_CUSTOM_DESCRIPTOR_OFFSET,
    ESP_CUSTOM_DESCRIPTOR_SCHEMA,
    esp_partition_compatibility,
    validate_esp_custom_descriptor,
    validate_esp_image_info,
)

PARTITIONS = """\
nvs,data,nvs,0x9000,0x5000,
otadata,data,ota,0xe000,0x2000,
safeboot,app,factory,0x10000,0xB0000,
app0,app,ota_0,0xC0000,0x130000,
spiffs,data,spiffs,0x1F0000,0x10000,
"""


def setUpModule() -> None:
    (ROOT / "build" / "host").mkdir(parents=True, exist_ok=True)


class EspImageCompatibilityTest(unittest.TestCase):
    def test_compatibility_identity_ignores_filesystem_size_and_csv_comments(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            root = Path(temporary)
            small = root / "small.csv"
            larger_filesystem = root / "larger-filesystem.csv"
            small.write_text("# board A\n" + PARTITIONS, encoding="utf-8")
            larger_filesystem.write_text(
                PARTITIONS.replace("0x10000,\n", "0x20000,\n") + "# board B\n",
                encoding="utf-8",
            )
            first = esp_partition_compatibility(small, 2 * 1024 * 1024, "esp32c2-2m", "esp32c2", 0)
            second = esp_partition_compatibility(
                larger_filesystem,
                2 * 1024 * 1024 + 0x10000,
                "esp32c2-2m",
                "esp32c2",
                0,
            )
        self.assertEqual(first["layout_id"], second["layout_id"])

    def test_layout_offset_change_changes_compatibility_identity(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            path = Path(temporary) / "layout.csv"
            path.write_text(PARTITIONS, encoding="utf-8")
            first = esp_partition_compatibility(path, 2 * 1024 * 1024, "esp32c2-2m", "esp32c2", 0)
            changed = PARTITIONS.replace("0xC0000,0x130000", "0xD0000,0x120000")
            path.write_text(changed, encoding="utf-8")
            second = esp_partition_compatibility(path, 2 * 1024 * 1024, "esp32c2-2m", "esp32c2", 0)
        self.assertNotEqual(first["layout_id"], second["layout_id"])

    def test_custom_descriptor_is_checked_against_resolved_target(self):
        compatibility = {
            "environment": "esp32c2-2m",
            "sdk_target": "esp32c2",
            "layout_id": "a" * 64,
            "app0_offset": 0xC0000,
            "safeboot_offset": 0x10000,
            "otadata_offset": 0xE000,
            "nvs_offset": 0x9000,
            "psram_bytes": 0,
        }
        payload = ESP_CUSTOM_DESCRIPTOR.pack(
            ESP_CUSTOM_DESCRIPTOR_MAGIC,
            ESP_CUSTOM_DESCRIPTOR_SCHEMA,
            ESP_CUSTOM_DESCRIPTOR.size,
            ESP_CUSTOM_DESCRIPTOR_APPLICATION,
            compatibility["app0_offset"],
            compatibility["safeboot_offset"],
            compatibility["otadata_offset"],
            compatibility["nvs_offset"],
            compatibility["environment"].encode().ljust(32, b"\0"),
            compatibility["sdk_target"].encode().ljust(16, b"\0"),
            compatibility["layout_id"].encode().ljust(65, b"\0"),
            compatibility["psram_bytes"],
        )
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            image = Path(temporary) / "application.bin"
            image.write_bytes(b"\xff" * ESP_CUSTOM_DESCRIPTOR_OFFSET + payload)
            validate_esp_custom_descriptor(image, compatibility)
            with self.assertRaisesRegex(PackageError, "mismatched"):
                validate_esp_custom_descriptor(
                    image, {**compatibility, "environment": "esp32s3-4m"}
                )

    def test_esptool_report_requires_matching_chip_and_valid_checksums(self):
        valid = """\
Detected image type: ESP32-S3
6 segments
Checksum: 14 (valid)
Validation Hash: abcdef (valid)
"""
        validate_esp_image_info(valid, "esp32s3")
        cases = (
            (valid.replace("ESP32-S3", "ESP32-C2"), "chip mismatch"),
            (valid.replace("(valid)", "(invalid)", 1), "checksum"),
            (
                valid.replace(
                    "Validation Hash: abcdef (valid)", "Validation Hash: abcdef (invalid)"
                ),
                "SHA-256 digest",
            ),
            (valid.replace("6 segments", "0 segments"), "no segments"),
        )
        for output, expected_error in cases:
            with (
                self.subTest(expected_error=expected_error),
                self.assertRaisesRegex(PackageError, expected_error),
            ):
                validate_esp_image_info(output, "esp32s3")


if __name__ == "__main__":
    unittest.main()
