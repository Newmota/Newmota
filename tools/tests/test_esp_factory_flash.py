"""ESP factory-package flashing with esptool and the probe replaced (no devices)."""

from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from newmota.catalog import resolve_environment
from newmota.device import DeviceError, ProbeResult
from newmota.factory_package import load_factory_package, write_factory_package
from newmota.host import sha256
from newmota.package import FirmwarePackage, PackageError
from newmota.platforms.esp.device import (
    PARTITION_ENTRY,
    PARTITION_MAGIC,
    PARTITION_MD5,
    EspIdfFlasher,
    _grow_partition_table,
)

MB = 1024 * 1024
SPIFFS = 0x1F0000


def partition_table(final_size: int) -> bytes:
    rows = (
        (1, 2, 0x9000, 0x5000, b"nvs", 0),
        (1, 0, 0xE000, 0x2000, b"otadata", 0),
        (0, 0, 0x10000, 0xB0000, b"safeboot", 0),
        (0, 0x10, 0xC0000, 0x130000, b"app0", 0),
        (1, 0x82, SPIFFS, final_size, b"spiffs", 1),
    )
    body = b"".join(
        PARTITION_ENTRY.pack(PARTITION_MAGIC, kind, subtype, offset, size, name, flags)
        for kind, subtype, offset, size, name, flags in rows
    )
    table = body + PARTITION_MD5 + hashlib.md5(body).digest()
    return table + b"\xff" * (0xC00 - len(table))


class EspFactoryFlashTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "factory").mkdir()
        self.files = {
            "bootloader.bin": (0x0, "bootloader", b"\x01" * 4096),
            "partition-table.bin": (0x8000, "partition-table", partition_table(0x10000)),
            "application.bin": (0xC0000, "app", b"\x03" * 8192),
            "littlefs.bin": (SPIFFS, "filesystem", b"\xff" * 0x10000),
        }
        for name, (_, _, content) in self.files.items():
            (self.root / "factory" / name).write_bytes(content)
        self.environment = resolve_environment("esp32c2-2m")
        self.calls: list[tuple[str, ...]] = []
        self.security = {
            "secure_boot": False,
            "flash_encryption": False,
            "secure_download_mode": False,
        }

    def package(self):
        data = {
            "schema": 1,
            "product": "newmota",
            "version": "0.0.1",
            "source": {"revision": "0" * 40, "dirty": False},
            "target": {
                "environment": "esp32c2-2m",
                "board": "esp32c2-2m",
                "platform": "esp",
                "chip": "esp32c2",
                "flash_bytes": 2 * MB,
                "psram_bytes": None,
            },
            "artifacts": [
                {
                    "path": f"factory/{name}",
                    "role": role,
                    "size": len(content),
                    "sha256": sha256(self.root / "factory" / name),
                }
                for name, (_, role, content) in self.files.items()
            ],
            "flash": {
                "write_args": ["--flash_mode", "dio", "--flash_size", "2MB", "--flash_freq", "60m"],
                "partition_table": {
                    "offset": 0x8000,
                    "partitions": [],
                    "expandable_final": {"name": "spiffs", "offset": SPIFFS},
                },
                "components": [
                    {"role": role, "path": f"factory/{name}", "offset": offset}
                    for name, (offset, role, _) in self.files.items()
                ],
            },
        }
        package = FirmwarePackage(self.root / "manifest.json", self.root, data)
        return load_factory_package(write_factory_package(package))

    def flash_package(self, factory, flash_bytes=2 * MB):
        probe = ProbeResult(
            "esp", "esp32c2", "/dev/null", flash_bytes, None, self.security, self.root
        )

        def esptool(flasher, chip, port, baud, before, after, command, *arguments, **_):
            self.calls.append((command, *arguments))

        with (
            mock.patch.object(EspIdfFlasher, "probe", lambda *_: probe),
            mock.patch.object(EspIdfFlasher, "_esptool", esptool),
        ):
            EspIdfFlasher().flash_factory(self.environment, factory, "/dev/null", None)

    def written(self):
        command, *arguments = self.calls[-1]
        self.assertEqual(command, "write-flash")
        pairs = arguments[6:]
        return arguments[:6], {
            int(pairs[index], 16): Path(pairs[index + 1]).read_bytes()
            for index in range(0, len(pairs), 2)
        }

    def test_writes_every_image_at_its_offset(self):
        self.flash_package(self.package())
        arguments, images = self.written()
        self.assertEqual(
            arguments, ["--flash_mode", "dio", "--flash_freq", "60m", "--flash_size", "2MB"]
        )
        self.assertEqual(images, {offset: content for offset, _, content in self.files.values()})

    def test_larger_flash_grows_only_the_final_filesystem(self):
        self.flash_package(self.package(), flash_bytes=4 * MB)
        arguments, images = self.written()
        self.assertEqual(arguments[-1], "4MB")
        self.assertEqual(images[0x8000], partition_table(4 * MB - SPIFFS))
        self.assertEqual(images[0xC0000], self.files["application.bin"][2])

    def test_smaller_flash_writes_nothing(self):
        factory = self.package()
        with self.assertRaisesRegex(DeviceError, "package requires"):
            self.flash_package(factory, flash_bytes=MB)
        self.assertEqual(self.calls, [])

    def test_encrypted_flash_writes_nothing(self):
        factory = self.package()
        self.security["flash_encryption"] = True
        with self.assertRaisesRegex(DeviceError, "flash encryption"):
            self.flash_package(factory)
        self.assertEqual(self.calls, [])

    def test_step_of_another_family_is_refused(self):
        factory = self.package()
        factory.description["steps"].append({"kind": "beken-net-reset"})
        with self.assertRaisesRegex(PackageError, "cannot carry out"):
            self.flash_package(factory)

    def test_table_without_its_checksum_is_refused(self):
        table = bytearray(partition_table(0x10000))
        table[5 * 32 + 20] ^= 0xFF
        with self.assertRaisesRegex(DeviceError, "checksummed"):
            _grow_partition_table(bytes(table), "spiffs", SPIFFS, 4 * MB)


if __name__ == "__main__":
    unittest.main()
