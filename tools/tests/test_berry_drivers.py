"""Checks of the Berry driver files an environment builds in with `solidify`."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from newmota.berry_drivers import check_solidify, read_driver_file, solidify_list
from newmota.model import ROOT, BuildError

SEGMENT = "berry/drivers/segment_display/segment_display.be"
TM1637 = "berry/drivers/segment_display/tm1637.be"
OV2640 = "berry/drivers/camera/ov2640.be"
GPIO = {"USE_BERRY": True, "USE_BERRY_GPIO": True}


class BerryDriversTest(unittest.TestCase):
    def setUp(self):
        (ROOT / "build").mkdir(exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(dir=ROOT / "build")
        self.addCleanup(self.directory.cleanup)

    def driver(self, text: str) -> str:
        path = Path(self.directory.name) / "driver.be"
        path.write_text(text, encoding="utf-8")
        return str(path.relative_to(ROOT))

    def test_every_shipped_driver_parses(self):
        for path in sorted((ROOT / "berry/drivers").rglob("*.be")):
            with self.subTest(path=path.name):
                self.assertTrue(read_driver_file(str(path.relative_to(ROOT))).classes)

    def test_bases_first_passes_and_keeps_the_order(self):
        files = check_solidify((SEGMENT, TM1637), GPIO)
        self.assertEqual([item.classes for item in files], [("SegmentDisplay",), ("Tm1637",)])

    def test_missing_base_names_the_file_to_add(self):
        with self.assertRaisesRegex(BuildError, f"add {SEGMENT} before it"):
            check_solidify((TM1637,), GPIO)

    def test_base_listed_later_asks_for_the_order(self):
        with self.assertRaisesRegex(BuildError, f"list {SEGMENT} before {TM1637}"):
            check_solidify((TM1637, SEGMENT), GPIO)

    def test_requires_names_the_missing_feature(self):
        with self.assertRaisesRegex(BuildError, "requires USE_WEBCAM"):
            check_solidify((OV2640,), {"USE_BERRY": True})

    def test_solidify_needs_berry(self):
        with self.assertRaisesRegex(BuildError, "requires USE_BERRY"):
            check_solidify((SEGMENT,), {})

    def test_top_level_code_is_refused(self):
        entry = self.driver("#@ solidify:Foo\nclass Foo\nend\nFoo()\n")
        with self.assertRaisesRegex(BuildError, ":4: only classes may stand at the top level"):
            read_driver_file(entry)

    def test_weak_flag_is_not_a_class(self):
        entry = self.driver("#@ solidify:Foo,weak\nclass Foo\nend\n")
        self.assertEqual(read_driver_file(entry).classes, ("Foo",))

    def test_every_class_must_be_marked(self):
        entry = self.driver("#@ solidify:Foo\nclass Foo\nend\nclass Bar\nend\n")
        with self.assertRaisesRegex(BuildError, "class Bar is not marked"):
            read_driver_file(entry)

    def test_firmware_names_and_bases_are_refused(self):
        for text, message in (
            ("#@ solidify:draw\nclass draw\nend\n", "takes a firmware name"),
            ("#@ solidify:Foo\nclass Foo : Driver\nend\n", "extends the firmware class Driver"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(BuildError, message):
                check_solidify((self.driver(text),), {"USE_BERRY": True})

    def test_list_entries_are_checked(self):
        for value, message in (
            ("ov2640.be", "must be a list"),
            (["berry/drivers/camera/none.be"], "not an existing .be file"),
            (["../outside.be"], "outside the repository"),
            ([OV2640, OV2640], "twice"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(BuildError, message):
                solidify_list(value, "user_config/test.yaml")


if __name__ == "__main__":
    unittest.main()
