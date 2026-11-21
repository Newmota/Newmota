"""Host-side tests for AmebaD OTA layout resolution."""

from __future__ import annotations

import tempfile
import unittest

from newmota.catalog import resolve_environment
from newmota.host import load_sdk_version
from newmota.model import ROOT
from newmota.platforms.amebad.images import resolve_layout


def setUpModule() -> None:
    (ROOT / "build" / "host").mkdir(parents=True, exist_ok=True)


class AmebadLayoutTest(unittest.TestCase):
    def test_application_and_safeboot_share_resolved_ota_geometry(self) -> None:
        application = resolve_environment("bw16-2m")
        safeboot = resolve_environment("bw16-2m-safeboot")
        sdk = load_sdk_version("amebad")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            app_layout = resolve_layout(application, sdk, directory / "application")
            safe_layout = resolve_layout(safeboot, sdk, directory / "safeboot")

        self.assertEqual(app_layout.platform, "amebad")
        self.assertEqual(app_layout.flash_bytes, 2 * 1024 * 1024)
        self.assertEqual(app_layout.erase_size, 0x1000)
        self.assertEqual(
            (app_layout.region("boot-system").offset, app_layout.region("boot-system").size),
            (0, 0x10000),
        )
        self.assertEqual(
            (app_layout.region("safeboot").offset, app_layout.region("safeboot").size),
            (0x10000, 0xC8000),
        )
        self.assertEqual(
            (app_layout.region("application").offset, app_layout.region("application").size),
            (0xD8000, 0x118000),
        )
        self.assertEqual(
            (app_layout.region("littlefs").offset, app_layout.region("littlefs").size),
            (0x1F0000, 0x10000),
        )
        self.assertEqual(app_layout.application_update_reach, ("application",))
        self.assertTrue(app_layout.settings_filesystem_preserved)
        self.assertEqual(app_layout.overlaps, ())
        self.assertEqual(app_layout.identity, safe_layout.identity)

    def test_four_mebibyte_pair_has_a_distinct_update_identity(self) -> None:
        sdk = load_sdk_version("amebad")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            layouts = {
                name: resolve_layout(resolve_environment(name), sdk, directory / name)
                for name in ("bw16-2m", "bw16-4m", "bw16-4m-safeboot")
            }
        application = layouts["bw16-4m"]
        self.assertEqual(application.identity, layouts["bw16-4m-safeboot"].identity)
        self.assertNotEqual(application.identity, layouts["bw16-2m"].identity)
        self.assertEqual(application.flash_bytes, 4 * 1024 * 1024)
        self.assertEqual(application.application_update_reach, ("application",))
        self.assertTrue(application.settings_filesystem_preserved)
        self.assertEqual(application.overlaps, ())
        safeboot = application.region("safeboot")
        app = application.region("application")
        filesystem = application.region("littlefs")
        self.assertEqual((safeboot.offset, safeboot.size), (0x10000, 0xD0000))
        self.assertEqual(safeboot.offset + safeboot.size, app.offset)
        self.assertEqual(app.offset + app.size, filesystem.offset)
        self.assertEqual(filesystem.offset + filesystem.size, application.flash_bytes)


if __name__ == "__main__":
    unittest.main()
