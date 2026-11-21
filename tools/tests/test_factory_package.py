"""Factory package writing and the checks a flasher applies before device access."""

from __future__ import annotations

import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from newmota.factory_package import (
    DESCRIPTION,
    load_factory_package,
    write_factory_package,
)
from newmota.host import sha256
from newmota.package import FirmwarePackage, PackageError


class FactoryPackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "factory").mkdir()
        application = self.root / "factory" / "application.rbl"
        filesystem = self.root / "factory" / "littlefs.bin"
        application.write_bytes(b"\x01" * 8192)
        filesystem.write_bytes(b"\xff" * 4096)
        self.data = {
            "schema": 1,
            "product": "newmota",
            "version": "0.0.1",
            "source": {"revision": "0" * 40, "dirty": False},
            "target": {
                "environment": "cb2s-2m",
                "board": "cb2s-2m",
                "platform": "beken",
                "chip": "bk7231n",
                "flash_bytes": 2 * 1024 * 1024,
                "psram_bytes": None,
            },
            "artifacts": [
                {
                    "path": f"factory/{path.name}",
                    "role": "factory-component",
                    "size": path.stat().st_size,
                    "sha256": sha256(path),
                }
                for path in (application, filesystem)
            ],
            "layout": {"regions": [{"name": "rf", "offset": 0x1D0000, "size": 4096}]},
            "flash": {
                "preserves": ["bootloader", "rf"],
                "net_initialization": {
                    "policy": "reset-newmota-preserve-native-v1",
                    "partition": "net",
                    "offset": 0x1DB000,
                    "size": 4096,
                },
                "components": [
                    {
                        "name": "application.rbl",
                        "path": "factory/application.rbl",
                        "offset": 0x11000,
                    },
                    {"name": "littlefs.bin", "path": "factory/littlefs.bin", "offset": 0x1DC000},
                ],
            },
        }

    def write(self):
        package = FirmwarePackage(self.root / "manifest.json", self.root, self.data)
        return write_factory_package(package)

    def rewrite(self, path, change):
        with zipfile.ZipFile(path) as archive:
            contents = {name: archive.read(name) for name in archive.namelist()}
        description = json.loads(contents[DESCRIPTION])
        change(description, contents)
        contents[DESCRIPTION] = json.dumps(description).encode()
        with zipfile.ZipFile(path, "w") as archive:
            for name, content in contents.items():
                archive.writestr(name, content)

    def test_package_carries_writes_steps_and_preserves(self):
        path = self.write()
        self.assertEqual(path.name, "newmota-cb2s-2m-0.0.1-factory.zip")
        package = load_factory_package(path)
        description = package.description
        self.assertEqual(
            [(item["offset"], item["file"]) for item in description["write"]],
            [(0x11000, "images/application.rbl"), (0x1DC000, "images/littlefs.bin")],
        )
        self.assertEqual(
            [step["kind"] for step in description["steps"]], ["beken-rf-check", "beken-net-reset"]
        )
        self.assertEqual(description["steps"][0]["offset"], 0x1D0000)
        self.assertEqual(description["preserve"], ["bootloader", "rf"])
        self.assertEqual(description["target"]["chip"], "bk7231n")

    def test_same_images_give_the_same_archive(self):
        first = self.write().read_bytes()
        self.assertEqual(self.write().read_bytes(), first)

    def test_changed_image_is_rejected(self):
        path = self.write()
        self.rewrite(
            path, lambda _, contents: contents.update({"images/littlefs.bin": b"\0" * 4096})
        )
        with self.assertRaisesRegex(PackageError, "hash mismatch"):
            load_factory_package(path)

    def test_overlapping_writes_are_rejected(self):
        path = self.write()
        self.rewrite(path, lambda description, _: description["write"][1].update(offset=0x12000))
        with self.assertRaisesRegex(PackageError, "overlap"):
            load_factory_package(path)

    def test_unaligned_write_is_rejected(self):
        path = self.write()
        self.rewrite(path, lambda description, _: description["write"][1].update(offset=0x1DC001))
        with self.assertRaisesRegex(PackageError, "sector"):
            load_factory_package(path)

    def test_write_beyond_flash_is_rejected(self):
        path = self.write()
        self.rewrite(
            path, lambda description, _: description["target"].update(flash_bytes=0x1DC000)
        )
        with self.assertRaisesRegex(PackageError, "exceeds"):
            load_factory_package(path)

    def test_unknown_step_is_rejected(self):
        path = self.write()
        self.rewrite(path, lambda description, _: description["steps"].append({"kind": "other"}))
        with self.assertRaisesRegex(PackageError, "unknown"):
            load_factory_package(path)

    def test_net_reset_over_an_image_is_rejected(self):
        path = self.write()
        self.rewrite(path, lambda description, _: description["steps"][1].update(offset=0x11000))
        with self.assertRaisesRegex(PackageError, "overlaps"):
            load_factory_package(path)

    def test_rf_calibration_inside_an_image_is_rejected(self):
        path = self.write()
        self.rewrite(path, lambda description, _: description["steps"][0].update(offset=0x12000))
        with self.assertRaisesRegex(PackageError, "RF calibration lies inside"):
            load_factory_package(path)

    def test_unlisted_file_is_rejected(self):
        path = self.write()
        self.rewrite(path, lambda _, contents: contents.update({"images/extra.bin": b"x"}))
        with self.assertRaisesRegex(PackageError, "does not write"):
            load_factory_package(path)

    def test_other_format_is_rejected(self):
        path = self.write()
        self.rewrite(path, lambda description, _: description.update(format="other"))
        with self.assertRaisesRegex(PackageError, "not a Newmota factory package"):
            load_factory_package(path)


if __name__ == "__main__":
    unittest.main()
