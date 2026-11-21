"""Checks of the files an environment ships in the factory filesystem with `filesystem`."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from littlefs import LittleFS
from newmota.factory_files import check_filesystem, filesystem_list
from newmota.model import ROOT, BuildError, FactoryFile
from newmota.package import BLOCK_SIZE, LittlefsImageError, create_filesystem_image

OV2640 = "berry/drivers/camera/ov2640.be"
ISP = "berry/drivers/camera/isp/ov5647.json"
SOURCE = "user_config/test.yaml"


class FactoryFilesTest(unittest.TestCase):
    def setUp(self):
        (ROOT / "build").mkdir(exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(dir=ROOT / "build")
        self.addCleanup(self.directory.cleanup)

    def file(self, name: str, data: bytes) -> str:
        path = Path(self.directory.name) / name
        path.write_bytes(data)
        return str(path.relative_to(ROOT))

    def test_short_and_long_forms(self):
        files = filesystem_list([OV2640, {"source": ISP, "target": "/isp.json"}], SOURCE)
        self.assertEqual(files, (FactoryFile(OV2640, "/ov2640.be"), FactoryFile(ISP, "/isp.json")))

    def test_entries_are_checked(self):
        for value, message in (
            (OV2640, "must be a list"),
            ([{"source": ISP}], "exactly source and target"),
            ([{"source": ISP, "target": "/a", "mode": 1}], "exactly source and target"),
            (["berry/drivers/camera/none.be"], "not an existing file"),
            (["../outside.be"], "outside the repository"),
            ([{"source": ISP, "target": "isp.json"}], "must be an absolute path"),
            ([{"source": ISP, "target": "/a/../isp.json"}], "without . or .."),
            ([{"source": ISP, "target": "/dir/"}], "without . or .."),
            ([{"source": ISP, "target": "/" + "a" * 191}], "longer than 191 bytes"),
            ([{"source": ISP, "target": "/.newmota/settings"}], "inside /.newmota"),
            ([{"source": ISP, "target": "/sd/isp.json"}], "inside /sd"),
            ([OV2640, {"source": ISP, "target": "/ov2640.be"}], "at /ov2640.be"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(BuildError, message):
                filesystem_list(value, SOURCE)

    def test_a_file_in_both_lists_is_refused(self):
        files = filesystem_list([OV2640], SOURCE)
        with self.assertRaisesRegex(BuildError, "in both solidify and filesystem"):
            check_filesystem(files, (OV2640,), {"USE_BERRY": True, "USE_WEBCAM": True})

    def test_requires_names_the_missing_feature(self):
        files = filesystem_list([OV2640], SOURCE)
        with self.assertRaisesRegex(BuildError, "requires USE_WEBCAM"):
            check_filesystem(files, (), {"USE_BERRY": True})
        check_filesystem(files, (), {"USE_BERRY": True, "USE_WEBCAM": True})

    def test_image_holds_the_files(self):
        script = self.file("autoexec.be", b"print(1)\n")
        image = Path(self.directory.name) / "littlefs.bin"
        files = (FactoryFile(script, "/autoexec.be"), FactoryFile(ISP, "/camera/isp.json"))
        create_filesystem_image(image, 8 * BLOCK_SIZE, files)
        filesystem = LittleFS(
            block_size=BLOCK_SIZE, block_count=8, read_size=128, prog_size=128, mount=False
        )
        filesystem.context.buffer[:] = image.read_bytes()
        filesystem.mount()
        with filesystem.open("/autoexec.be", "rb") as handle:
            self.assertEqual(handle.read(), b"print(1)\n")
        with filesystem.open("/camera/isp.json", "rb") as handle:
            self.assertEqual(handle.read(), (ROOT / ISP).read_bytes())

    def test_files_that_do_not_fit_are_refused(self):
        large = self.file("large.bin", bytes(16 * BLOCK_SIZE))
        image = Path(self.directory.name) / "littlefs.bin"
        with self.assertRaisesRegex(LittlefsImageError, "do not fit the 32 KB"):
            create_filesystem_image(image, 8 * BLOCK_SIZE, (FactoryFile(large, "/large.bin"),))


if __name__ == "__main__":
    unittest.main()
