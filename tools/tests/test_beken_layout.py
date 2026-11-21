"""Host-side tests for Beken layout descriptors and filesystem placement."""

from __future__ import annotations

import tempfile
import unittest
from dataclasses import replace

from newmota.catalog import resolve_environment
from newmota.host import load_sdk_version
from newmota.model import ROOT
from newmota.platforms.beken.images import BekenLayoutError, resolve_layout


def setUpModule() -> None:
    (ROOT / "build" / "host").mkdir(parents=True, exist_ok=True)


def bounds(layout, name: str) -> tuple[int, int]:
    region = layout.region(name)
    return region.offset, region.size


class BekenLayoutTest(unittest.TestCase):
    def resolve(self, name: str, edit=None):
        environment = resolve_environment(name)
        sdk = load_sdk_version("beken")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            if edit is not None:
                descriptor = directory / "beken-variant.toml"
                original = environment.partition
                self.assertIsNotNone(original)
                text = edit(original.read_text(encoding="utf-8"))
                descriptor.write_text(text, encoding="utf-8")
                environment = replace(environment, partition=descriptor)
            return resolve_layout(environment, sdk, directory)

    def test_tuya_module_uses_the_tuya_map(self) -> None:
        layout = self.resolve("cb2s-2m")
        self.assertEqual(bounds(layout, "application"), (0x011000, 0x119000))
        self.assertEqual(bounds(layout, "ota"), (0x12A000, 0x0A6000))
        self.assertEqual(bounds(layout, "rf"), (0x1D0000, 0x001000))
        self.assertEqual(bounds(layout, "ble-bonding"), (0x1D1000, 0x001000))
        self.assertEqual(bounds(layout, "tuya-kvs"), (0x1D3000, 0x008000))
        self.assertEqual(bounds(layout, "net"), (0x1DB000, 0x001000))
        self.assertEqual(bounds(layout, "filesystem"), (0x1DC000, 0x011000))
        self.assertEqual(bounds(layout, "tuya-data"), (0x1ED000, 0x013000))
        self.assertEqual(layout.application_update_reach, ("ota",))
        self.assertTrue(layout.settings_filesystem_preserved)

    def test_bk7231t_keeps_settings_outside_ota_and_tuya_data(self) -> None:
        layout = self.resolve("wb2s")
        self.assertEqual(bounds(layout, "application"), (0x011000, 0x121000))
        self.assertEqual(bounds(layout, "ota"), (0x132000, 0x0A6000))
        self.assertEqual(bounds(layout, "tuya-kvs"), (0x1D8000, 0x008000))
        self.assertEqual(bounds(layout, "rf"), (0x1E0000, 0x001000))
        self.assertEqual(bounds(layout, "ble-bonding"), (0x1E1000, 0x001000))
        self.assertEqual(bounds(layout, "reserved"), (0x1E2000, 0x001000))
        self.assertEqual(bounds(layout, "net"), (0x1E3000, 0x001000))
        self.assertEqual(bounds(layout, "filesystem"), (0x1E4000, 0x009000))
        self.assertEqual(bounds(layout, "tuya-data"), (0x1ED000, 0x013000))
        self.assertEqual(layout.overlaps, ())
        self.assertTrue(layout.settings_filesystem_preserved)

    def test_sdk_module_is_discovered_from_the_pinned_sdk(self) -> None:
        layout = self.resolve("xh-cb3s")
        self.assertEqual(bounds(layout, "application"), (0x011000, 0x121000))
        self.assertEqual(bounds(layout, "ota"), (0x132000, 0x0AE000))
        self.assertEqual(bounds(layout, "rf"), (0x1E0000, 0x001000))
        self.assertEqual(bounds(layout, "net"), (0x1E1000, 0x001000))
        self.assertEqual(bounds(layout, "filesystem"), (0x1E2000, 0x00E000))
        self.assertEqual(bounds(layout, "ble-bonding"), (0x1F0000, 0x001000))
        self.assertEqual(layout.overlaps, ())

    def test_bk7252_uses_the_bk7221u_net_record(self) -> None:
        for name in ("generic-bk7252-2m", "generic-bk7252-4m"):
            with self.subTest(name=name):
                layout = self.resolve(name)
                self.assertEqual(bounds(layout, "application"), (0x011000, 0x121000))
                self.assertEqual(bounds(layout, "ota"), (0x132000, 0x0AE000))
                self.assertEqual(bounds(layout, "rf"), (0x1E0000, 0x001000))
                self.assertEqual(bounds(layout, "filesystem"), (0x1E2000, 0x00E000))
                self.assertEqual(bounds(layout, "ble-bonding"), (0x1F0000, 0x001000))
                self.assertEqual(bounds(layout, "net"), (0x1FF000, 0x001000))
                self.assertEqual(layout.overlaps, ())

    def test_bk7252n_shares_the_bk7231n_table(self) -> None:
        for name in ("generic-bk7252n-2m", "generic-bk7252n-4m"):
            with self.subTest(name=name):
                layout = self.resolve(name)
                self.assertEqual(bounds(layout, "ota"), (0x132000, 0x0AE000))
                self.assertEqual(bounds(layout, "net"), (0x1E1000, 0x001000))
                self.assertEqual(bounds(layout, "filesystem"), (0x1E2000, 0x00E000))
                self.assertEqual(layout.overlaps, ())

    def test_filesystem_inside_the_update_area_is_refused(self) -> None:
        with self.assertRaises(BekenLayoutError):
            self.resolve(
                "xh-cb3s", lambda text: text.replace("offset = 0x1E2000", "offset = 0x1D2000")
            )

    def test_filesystem_over_vendor_data_is_refused(self) -> None:
        with self.assertRaises(BekenLayoutError):
            self.resolve("cb2s-2m", lambda text: text.replace("size = 0x11000", "size = 0x12000"))

    def test_ota_overlay_descriptors_are_refused(self) -> None:
        with self.assertRaises(BekenLayoutError):
            overlay = '[filesystem]\noverlay = "ota"'
            self.resolve("xh-cb3s", lambda text: text.replace("[filesystem]", overlay))


if __name__ == "__main__":
    unittest.main()
