"""Beken factory-package flashing against an in-memory bk7231tools device."""

from __future__ import annotations

import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from newmota.catalog import resolve_environment
from newmota.device import DeviceError
from newmota.factory_package import load_factory_package, write_factory_package
from newmota.host import sha256
from newmota.package import FirmwarePackage, PackageError
from newmota.platforms.beken import device as beken_device
from newmota.platforms.beken.device import RF_CALIBRATION_HEADER, BekenFlasher

FLASH_BYTES = 2 * 1024 * 1024
APPLICATION = 0x11000
RF = 0x1D0000
NET = 0x1DB000
FILESYSTEM = 0x1DC000


class FakeBeken:
    """The bk7231tools calls the Beken flasher makes, over a bytearray flash."""

    def __init__(self, flash: bytearray):
        self.flash = flash
        self.chip_type = SimpleNamespace(name="BK7231N")
        self.flash_size = len(flash)
        self.flash_id = b"\x85\x20\x15"
        self.protocol_type = SimpleNamespace(name="FULL")
        self.bootloader_type = None
        self.rebooted = False

    def flash_read_bytes(self, offset, size, crc_check):
        return bytes(self.flash[offset : offset + size])

    def flash_read(self, offset, size, crc_check):
        yield self.flash_read_bytes(offset, size, crc_check)

    def program_flash(self, io, io_size, start, crc_check):
        data = io.read(io_size)
        self.flash[start : start + len(data)] = data
        yield len(data)

    def reboot_chip(self):
        self.rebooted = True


class BekenFactoryFlashTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "factory").mkdir()
        self.application = b"\x01" * 8192
        self.filesystem = b"\x02" * 69632
        (self.root / "factory" / "application.rbl").write_bytes(self.application)
        (self.root / "factory" / "littlefs.bin").write_bytes(self.filesystem)
        self.flash = bytearray(b"\xaa" * FLASH_BYTES)
        self.flash[RF : RF + 4] = RF_CALIBRATION_HEADER
        self.device = FakeBeken(self.flash)
        self.environment = resolve_environment("cb2s-2m")

        @contextmanager
        def connected(port, baud):
            yield self.device

        for name, value in (
            ("_connected", connected),
            ("_probe_directory", lambda environment, flash_bytes: self.root),
        ):
            patcher = mock.patch.object(beken_device, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def package(self, **changes):
        components = [
            ("application.rbl", APPLICATION),
            ("littlefs.bin", FILESYSTEM),
        ]
        data = {
            "schema": 1,
            "product": "newmota",
            "version": "0.0.1",
            "source": {"revision": "0" * 40, "dirty": False},
            "target": {
                "environment": "cb2s-2m",
                "board": "cb2s-2m",
                "platform": "beken",
                "chip": "bk7231n",
                "flash_bytes": FLASH_BYTES,
                "psram_bytes": None,
            },
            "layout": {"regions": [{"name": "rf", "offset": RF, "size": 4096}]},
            "artifacts": [
                {
                    "path": f"factory/{name}",
                    "role": "factory-component",
                    "size": (self.root / "factory" / name).stat().st_size,
                    "sha256": sha256(self.root / "factory" / name),
                }
                for name, _ in components
            ],
            "flash": {
                "preserves": ["bootloader", "rf", "ble-bonding", "tuya-kvs", "tuya-data"],
                "net_initialization": {
                    "policy": "reset-newmota-preserve-native-v1",
                    "partition": "net",
                    "offset": NET,
                    "size": 4096,
                },
                "components": [
                    {"name": name, "path": f"factory/{name}", "offset": offset}
                    for name, offset in components
                ],
            },
        }
        data["flash"].update(changes)
        path = write_factory_package(FirmwarePackage(self.root / "manifest.json", self.root, data))
        return load_factory_package(path)

    def flash_package(self, factory):
        BekenFlasher().flash_factory(self.environment, factory, "/dev/null", None)

    def test_writes_images_resets_net_and_keeps_the_rest(self):
        self.flash_package(self.package())
        self.assertEqual(self.flash[APPLICATION : APPLICATION + 8192], self.application)
        self.assertEqual(self.flash[FILESYSTEM : FILESYSTEM + 69632], self.filesystem)
        # A foreign NET sector becomes an empty native table.
        self.assertEqual(self.flash[NET : NET + 8], b"TLV\x00\x00\x00\x00\x00")
        self.assertEqual(self.flash[:APPLICATION], b"\xaa" * APPLICATION)
        self.assertEqual(self.flash[RF : RF + 4], RF_CALIBRATION_HEADER)
        self.assertTrue(self.device.rebooted)

    def test_missing_rf_calibration_writes_nothing(self):
        factory = self.package()
        self.flash[RF : RF + 4] = b"\xff" * 4
        before = bytes(self.flash)
        with self.assertRaisesRegex(DeviceError, "no RF calibration"):
            self.flash_package(factory)
        self.assertEqual(bytes(self.flash), before)
        self.assertFalse(self.device.rebooted)

    def test_foreign_calibration_inside_a_write_writes_nothing(self):
        factory = self.package()
        self.flash[0x1E0000 : 0x1E0000 + 4] = RF_CALIBRATION_HEADER
        factory.description["write"][1]["offset"] = 0x1E0000
        before = bytes(self.flash)
        with self.assertRaisesRegex(DeviceError, "would overwrite"):
            self.flash_package(factory)
        self.assertEqual(bytes(self.flash), before)

    def test_package_for_another_family_step_is_refused(self):
        factory = self.package()
        factory.description["steps"].append({"kind": "esp-partition-table"})
        with self.assertRaisesRegex(PackageError, "cannot carry out"):
            self.flash_package(factory)

    def test_unknown_net_policy_is_refused(self):
        factory = self.package()
        factory.description["steps"][1]["policy"] = "other"
        with self.assertRaisesRegex(PackageError, "NET reset"):
            self.flash_package(factory)


if __name__ == "__main__":
    unittest.main()
