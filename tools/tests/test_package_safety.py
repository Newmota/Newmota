"""Safety regressions for flash package manifests and OTA bounds (no devices)."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from newmota.host import sha256
from newmota.package import PackageError, load_package


class PackageSafetyTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.image = self.root / "factory.bin"
        self.image.write_bytes(b"image")
        self.manifest = self.root / "manifest.json"
        self.data = {
            "schema": 1,
            "product": "newmota",
            "flashable": True,
            "qualification": {"factory": "development"},
            "target": {
                "environment": "test",
                "platform": "esp",
                "chip": "esp32c2",
                "role": "application",
                "flash_bytes": 2 * 1024 * 1024,
            },
            "artifacts": [
                {
                    "path": self.image.name,
                    "role": "factory",
                    "size": 5,
                    "sha256": sha256(self.image),
                }
            ],
            "flash": {"components": [{"path": self.image.name, "offset": 0}]},
        }

    def load(self):
        self.manifest.write_text(json.dumps(self.data))
        return load_package(self.manifest)

    def test_valid_manifest(self):
        self.assertEqual(self.load().file("factory.bin"), self.image)

    def test_corruption_is_rejected_before_device_access(self):
        self.image.write_bytes(b"other")
        with self.assertRaisesRegex(PackageError, "hash mismatch"):
            self.load()

    def test_path_escape_is_rejected(self):
        self.data["artifacts"][0]["path"] = "../outside.bin"
        with self.assertRaisesRegex(PackageError, "escapes"):
            self.load()

    def test_overlapping_components_are_rejected(self):
        self.data["flash"]["components"].append({"path": "factory.bin", "offset": 1})
        with self.assertRaisesRegex(PackageError, "overlap"):
            self.load()

    def test_out_of_flash_write_is_rejected(self):
        self.data["flash"]["components"][0]["offset"] = 2 * 1024 * 1024 - 1
        with self.assertRaisesRegex(PackageError, "capacity"):
            self.load()

    def test_unqualified_image_is_rejected(self):
        self.data["qualification"]["factory"] = "unqualified"
        with self.assertRaisesRegex(PackageError, "unqualified"):
            self.load()

    def add_ota_artifact(self):
        ota = self.root / "ota.bin"
        ota.write_bytes(b"native-app")
        self.data["artifacts"].append(
            {
                "path": ota.name,
                "role": "application-update",
                "size": ota.stat().st_size,
                "sha256": sha256(ota),
                "offset": 0x10000,
                "size_limit": 1024,
            }
        )
        self.data["ota"] = {
            "status": "wired-application",
            "artifacts": [
                {
                    "role": "application-update",
                    "path": ota.name,
                    "size": ota.stat().st_size,
                    "size_limit": 1024,
                    "offset": 0x10000,
                }
            ],
        }

    def test_native_ota_artifact_reference_and_bounds(self):
        self.add_ota_artifact()
        self.assertEqual(self.load().file("ota.bin"), self.root / "ota.bin")

    def test_ota_path_must_be_a_declared_application_artifact(self):
        self.add_ota_artifact()
        self.data["ota"]["artifacts"][0]["path"] = "factory.bin"
        with self.assertRaisesRegex(PackageError, "application artifact"):
            self.load()

    def test_ota_artifact_must_fit_its_native_limit(self):
        self.add_ota_artifact()
        self.data["ota"]["artifacts"][0]["size_limit"] = 2
        with self.assertRaisesRegex(PackageError, "native image limit"):
            self.load()

    def test_ota_descriptor_must_match_artifact_size(self):
        self.add_ota_artifact()
        self.data["ota"]["artifacts"][0]["size"] += 1
        with self.assertRaisesRegex(PackageError, "size does not match"):
            self.load()

    def test_factory_ota_requires_explicit_application_role_and_bounds(self):
        component = self.root / "littlefs.bin"
        component.write_bytes(b"filesystem")
        self.data["artifacts"].append(
            {
                "path": component.name,
                "role": "factory-component",
                "size": component.stat().st_size,
                "sha256": sha256(component),
                "offset": 0x10000,
                "update_role": "filesystem",
                "update_size_limit": 1024,
            }
        )
        self.data["flash"]["components"].append(
            {"path": component.name, "offset": 0x10000, "update_role": "filesystem"}
        )
        self.data["ota"] = {
            "status": "wired-application",
            "artifacts": [
                {
                    "role": "application-update",
                    "path": component.name,
                    "offset": 0x10000,
                    "size_limit": 1024,
                }
            ],
        }
        with self.assertRaisesRegex(PackageError, "not explicitly marked"):
            self.load()

    def test_factory_application_ota_must_match_native_bound(self):
        component = self.root / "application.bin"
        component.write_bytes(b"application")
        self.data["artifacts"].append(
            {
                "path": component.name,
                "role": "factory-component",
                "size": component.stat().st_size,
                "sha256": sha256(component),
                "offset": 0x10000,
                "update_role": "application",
                "update_size_limit": 2048,
            }
        )
        self.data["flash"]["components"].append(
            {
                "path": component.name,
                "offset": 0x10000,
                "update_role": "application",
                "update_size_limit": 2048,
            }
        )
        self.data["ota"] = {
            "status": "wired-application",
            "artifacts": [
                {
                    "role": "application-update",
                    "path": component.name,
                    "offset": 0x10000,
                    "size_limit": 2048,
                }
            ],
        }
        self.assertEqual(self.load().file("application.bin"), component)


if __name__ == "__main__":
    unittest.main()
