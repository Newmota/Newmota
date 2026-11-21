"""Bouffalo, AmebaD and Pico factory-package flashing with their tools replaced (no devices)."""

from __future__ import annotations

import binascii
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from newmota.catalog import resolve_environment
from newmota.device import ProbeResult
from newmota.factory_package import load_factory_package, write_factory_package
from newmota.host import sha256
from newmota.package import FirmwarePackage, PackageError
from newmota.platforms.amebad import device as amebad_device
from newmota.platforms.bouffalo import device as bouffalo_device
from newmota.platforms.bouffalo.device import PT_MAGIC, BouffaloFlasher, parse_partition_table
from newmota.platforms.pico import device as pico_device

MB = 1024 * 1024
BOUFFALO_LITTLEFS = 0x1F0000


def bouffalo_table() -> bytes:
    entries = [
        (0, 0, 0, b"FW", 0x10000, 0, 0xD0000, 0, 0, 0),
        (2, 0, 0, b"lfs", BOUFFALO_LITTLEFS, 0, 2 * MB - BOUFFALO_LITTLEFS, 0, 0, 0),
    ]
    header = struct.pack("<IHHI", PT_MAGIC, 0, len(entries), 0)
    header += struct.pack("<I", binascii.crc32(header) & 0xFFFFFFFF)
    body = b"".join(struct.pack("<BBB9sIIIIII", *entry) for entry in entries)
    return header + body + struct.pack("<I", binascii.crc32(body) & 0xFFFFFFFF)


class FamilyFactoryFlashTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "factory").mkdir()
        for module in (bouffalo_device, amebad_device, pico_device):
            for name, value in (("WORK_DIR", self.root / "work"), ("ROOT", self.root)):
                patcher = mock.patch.object(module, name, value)
                patcher.start()
                self.addCleanup(patcher.stop)

    def package(self, environment, platform, chip, flash_bytes, files, regions=()):
        for name, (_, content) in files.items():
            (self.root / "factory" / name).write_bytes(content)
        data = {
            "schema": 1,
            "product": "newmota",
            "version": "0.0.1",
            "source": {"revision": "0" * 40, "dirty": False},
            "target": {
                "environment": environment,
                "board": environment,
                "platform": platform,
                "chip": chip,
                "flash_bytes": flash_bytes,
                "psram_bytes": None,
            },
            "layout": {"regions": list(regions)},
            "artifacts": [
                {
                    "path": f"factory/{name}",
                    "role": "factory-component",
                    "size": len(content),
                    "sha256": sha256(self.root / "factory" / name),
                }
                for name, (_, content) in files.items()
            ],
            "flash": {
                "components": [
                    {
                        "name": name,
                        "role": "factory-component",
                        "path": f"factory/{name}",
                        "offset": offset,
                    }
                    for name, (offset, _) in files.items()
                ]
            },
        }
        package = FirmwarePackage(self.root / "manifest.json", self.root, data)
        return load_factory_package(write_factory_package(package))

    def bouffalo_package(self):
        return self.package(
            "bl602-2m",
            "bouffalo",
            "bl602",
            2 * MB,
            {
                "boot2.bin": (0x0, b"\x01" * 4096),
                "partition-table0.bin": (0xE000, bouffalo_table()),
                "partition-table1.bin": (0xF000, bouffalo_table()),
                "application.bin": (0x10000, b"\x02" * 8192),
                "littlefs.bin": (BOUFFALO_LITTLEFS, b"\xff" * 0x10000),
            },
            [{"name": "littlefs", "offset": BOUFFALO_LITTLEFS, "size": 0x10000}],
        )

    def test_bouffalo_writes_images_and_grown_tables_per_capacity(self):
        factory = self.bouffalo_package()
        self.assertEqual(
            factory.description["steps"],
            [
                {
                    "kind": "bouffalo-partition-table",
                    "tables": [0xE000, 0xF000],
                    "filesystem_offset": BOUFFALO_LITTLEFS,
                }
            ],
        )
        calls = []
        api = SimpleNamespace(FlashImage=lambda *fields: fields, flash_uart=None)
        with (
            mock.patch.object(BouffaloFlasher, "_api", staticmethod(lambda: api)),
            mock.patch.object(
                BouffaloFlasher,
                "_invoke",
                staticmethod(
                    lambda action, function, flash_images, **options: (
                        calls.append((flash_images, options)) or SimpleNamespace(log="")
                    )
                ),
            ),
        ):
            BouffaloFlasher().flash_factory(
                resolve_environment("bl602-2m"), factory, "/dev/null", None
            )
        images, options = calls[0]
        self.assertEqual(
            [offset for _, offset, _ in images], [0x0, 0xE000, 0xF000, 0x10000, BOUFFALO_LITTLEFS]
        )
        self.assertEqual(options["minimum_flash_bytes"], 2 * MB)
        _, _, variants = images[1]
        self.assertEqual(sorted(variants), [4 * MB, 8 * MB, 16 * MB])
        grown = parse_partition_table(variants[4 * MB].read_bytes() + b"\xff" * 4096)
        self.assertEqual(grown["entries"][1]["size0"], 4 * MB - BOUFFALO_LITTLEFS)
        self.assertEqual(images[3][0].read_bytes(), b"\x02" * 8192)

    def test_amebad_uploads_package_images_with_the_sdk_loader(self):
        loader = self.root / "sdk" / "tools" / "AmebaD" / "Image_Tool"
        loader.mkdir(parents=True)
        (loader / "imgtool_flashloader_amebad.bin").write_bytes(b"loader")
        factory = self.package(
            "bw16-2m",
            "amebad",
            "rtl8720dn",
            2 * MB,
            {
                "km0_boot_all.bin": (0x0, b"\x01" * 2880),
                "application-image2.bin": (0xD8000, b"\x02" * 4096),
            },
        )
        calls = []
        with (
            mock.patch.object(
                amebad_device,
                "check_sdk_version",
                lambda name: SimpleNamespace(source=self.root / "sdk"),
            ),
            mock.patch.object(
                amebad_device, "upload", lambda *arguments, **options: calls.append(arguments)
            ),
        ):
            amebad_device.AmebadFlasher().flash_factory(
                resolve_environment("bw16-2m"), factory, "/dev/null", None
            )
        _, used_loader, images, baud = calls[0]
        self.assertEqual(used_loader.name, "imgtool_flashloader_amebad.bin")
        self.assertEqual(baud, 230_400)
        self.assertEqual(
            [(name, offset) for name, offset, _ in images],
            [
                ("km0_boot_all.bin", 0x0),
                ("application-image2.bin", 0xD8000),
            ],
        )
        self.assertEqual(images[1][2].read_bytes(), b"\x02" * 4096)

    def test_pico_writes_each_image_and_reboots(self):
        factory = self.package(
            "pico2-w",
            "pico",
            "rp2350",
            4 * MB,
            {
                "safeboot.bin": (0x0, b"\x01" * 4096),
                "application.bin": (0xD0000, b"\x02" * 4096),
            },
        )
        writes, commands = [], []
        probe = ProbeResult("pico", "rp2350", "/dev/null", 4 * MB, None, {}, self.root)
        with (
            mock.patch.object(pico_device.PicoFlasher, "_probe_target", lambda *_, **__: probe),
            mock.patch.object(
                pico_device.PicoFlasher,
                "_write_component",
                lambda self, name, offset, path, **_: writes.append(
                    (name, offset, path.read_bytes())
                ),
            ),
            mock.patch.object(
                pico_device.PicoFlasher,
                "_run_picotool",
                lambda self, arguments, **_: commands.append(arguments),
            ),
        ):
            pico_device.PicoFlasher().flash_factory(
                resolve_environment("pico2-w"), factory, "/dev/null", None
            )
        self.assertEqual(
            writes,
            [("safeboot.bin", 0x0, b"\x01" * 4096), ("application.bin", 0xD0000, b"\x02" * 4096)],
        )
        self.assertEqual(commands, [["reboot", "-a"]])

    def test_pico_with_other_flash_size_writes_nothing(self):
        factory = self.package(
            "pico2-w", "pico", "rp2350", 4 * MB, {"safeboot.bin": (0x0, b"\x01" * 4096)}
        )
        writes = []
        probe = ProbeResult("pico", "rp2350", "/dev/null", 2 * MB, None, {}, self.root)
        with (
            mock.patch.object(pico_device.PicoFlasher, "_probe_target", lambda *_, **__: probe),
            mock.patch.object(
                pico_device.PicoFlasher,
                "_write_component",
                lambda *arguments, **_: writes.append(arguments),
            ),
            self.assertRaisesRegex(pico_device.PicoProgrammerError, "package requires"),
        ):
            pico_device.PicoFlasher().flash_factory(
                resolve_environment("pico2-w"), factory, "/dev/null", None
            )
        self.assertEqual(writes, [])

    def test_step_of_another_family_is_refused(self):
        factory = self.package(
            "pico2-w", "pico", "rp2350", 4 * MB, {"safeboot.bin": (0x0, b"\x01" * 4096)}
        )
        factory.description["steps"].append({"kind": "beken-net-reset"})
        with self.assertRaisesRegex(PackageError, "Pico cannot carry out"):
            pico_device.PicoFlasher().flash_factory(
                resolve_environment("pico2-w"), factory, "/dev/null", None
            )


if __name__ == "__main__":
    unittest.main()
