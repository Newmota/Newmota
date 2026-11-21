"""Host-side tests for Pico native-layout resolution."""

from __future__ import annotations

import json
import tempfile
import unittest
from dataclasses import replace

from newmota.catalog import resolve_environment
from newmota.host import load_sdk_version
from newmota.model import ROOT
from newmota.platforms.pico.images import PicoLayoutError, resolve_layout


def setUpModule() -> None:
    (ROOT / "build" / "host").mkdir(parents=True, exist_ok=True)


class PicoLayoutTest(unittest.TestCase):
    def test_rp2040_layout_resolves_project_owned_physical_regions(self) -> None:
        environment = resolve_environment("pico-w")
        sdk = load_sdk_version("pico-sdk")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            layout = resolve_layout(environment, sdk, ROOT / temporary)

        self.assertEqual(layout.platform, "pico")
        self.assertEqual(layout.flash_bytes, 2 * 1024 * 1024)
        self.assertEqual(layout.erase_size, 0x1000)
        self.assertEqual(
            (layout.region("safeboot").offset, layout.region("safeboot").size),
            (0, 0xC0000),
        )
        self.assertEqual(
            (layout.region("application").offset, layout.region("application").size),
            (0xC0000, 0x12F000),
        )
        self.assertEqual(
            (layout.region("boot-marker").offset, layout.region("boot-marker").size),
            (0x1EF000, 0x1000),
        )
        self.assertEqual(
            (layout.region("littlefs").offset, layout.region("littlefs").size),
            (0x1F0000, 0x10000),
        )
        self.assertEqual(layout.application_update_reach, ("application",))
        self.assertTrue(layout.settings_filesystem_preserved)
        self.assertEqual(layout.overlaps, ())

    def test_rp2040_application_and_safeboot_share_layout_identity(self) -> None:
        application = resolve_environment("pico-w")
        safeboot = resolve_environment("pico-w-safeboot")
        sdk = load_sdk_version("pico-sdk")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            first = resolve_layout(application, sdk, directory / "app")
            second = resolve_layout(safeboot, sdk, directory / "safeboot")
        self.assertEqual(first.identity, second.identity)

    def test_rp2040_layout_rejects_gaps_and_missing_regions(self) -> None:
        environment = resolve_environment("pico-w")
        sdk = load_sdk_version("pico-sdk")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            source = json.loads(environment.partition.read_text(encoding="utf-8"))
            gap_path = directory / "gap.json"
            gap = json.loads(json.dumps(source))
            gap["regions"][1]["offset"] += 0x1000
            gap_path.write_text(json.dumps(gap), encoding="utf-8")
            with self.assertRaises(PicoLayoutError):
                resolve_layout(replace(environment, partition=gap_path), sdk, directory / "gap")

            missing_path = directory / "missing.json"
            missing = json.loads(json.dumps(source))
            missing["regions"].pop()
            missing_path.write_text(json.dumps(missing), encoding="utf-8")
            with self.assertRaises(PicoLayoutError):
                resolve_layout(
                    replace(environment, partition=missing_path), sdk, directory / "missing"
                )

    def test_layout_resolves_application_safeboot_and_filesystem(self) -> None:
        environment = resolve_environment("pico2-w")
        sdk = load_sdk_version("pico-sdk")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            layout = resolve_layout(environment, sdk, ROOT / temporary)

        self.assertEqual(layout.platform, "pico")
        self.assertEqual(layout.flash_bytes, 4 * 1024 * 1024)
        self.assertEqual(layout.erase_size, 0x1000)
        application = layout.region("application")
        self.assertEqual((application.offset, application.size), (0x0D0000, 0x2D2000))
        self.assertEqual(application.image_budget, 0x2D2000)
        safeboot = layout.region("safeboot")
        self.assertEqual((safeboot.offset, safeboot.size), (0x0000, 0x0D0000))
        self.assertEqual(safeboot.image_budget, 0xD0000)
        filesystem = layout.region("littlefs")
        self.assertEqual((filesystem.offset, filesystem.size), (0x3A3000, 0x5A000))
        self.assertEqual(filesystem.image_budget, 0x5A000)
        # The sticky-boot marker owns the first sector of `data`, outside the
        # filesystem geometry.
        self.assertEqual(
            (layout.region("boot-marker").offset, layout.region("boot-marker").size),
            (0x3A2000, 0x1000),
        )
        # Safeboot owns the embedded partition table at flash zero so application
        # updates can replace the table-free image in its physical XIP slot.
        self.assertEqual(safeboot.offset, 0x0000)
        self.assertEqual(application.offset, 0x0D0000)
        self.assertEqual(
            (layout.region("e10-reserve").offset, layout.region("e10-reserve").size),
            (0x3FF000, 0x1000),
        )
        self.assertEqual(
            (layout.region("btstack-reserve").offset, layout.region("btstack-reserve").size),
            (0x3FD000, 0x2000),
        )
        self.assertEqual(layout.application_update_reach, ("application",))
        self.assertTrue(layout.settings_filesystem_preserved)
        self.assertEqual(layout.overlaps, ())

    def test_safeboot_environment_resolves_the_same_layout(self) -> None:
        application = resolve_environment("pico2-w")
        safeboot = resolve_environment("pico2-w-safeboot")
        sdk = load_sdk_version("pico-sdk")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            first = resolve_layout(application, sdk, directory / "app")
            second = resolve_layout(safeboot, sdk, directory / "safeboot")
        self.assertEqual(first.identity, second.identity)

    def test_missing_data_partition_fails_closed(self) -> None:
        environment = resolve_environment("pico2-w")
        sdk = load_sdk_version("pico-sdk")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            variant = directory / "no-data.json"
            document = json.loads(environment.partition.read_text(encoding="utf-8"))
            document["partitions"] = [
                item for item in document["partitions"] if item["name"] != "data"
            ]
            variant.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaises(PicoLayoutError):
                resolve_layout(
                    replace(environment, partition=variant),
                    sdk,
                    directory,
                )

    def test_unknown_partition_fails_closed(self) -> None:
        environment = resolve_environment("pico2-w")
        sdk = load_sdk_version("pico-sdk")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            variant = directory / "unknown.json"
            document = json.loads(environment.partition.read_text(encoding="utf-8"))
            document["partitions"].append(
                {
                    "name": "mystery",
                    "start": "4092K",
                    "size": "4K",
                    "families": ["data"],
                    "permissions": {"secure": "rw", "nonsecure": "", "bootloader": "rw"},
                }
            )
            variant.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaises(PicoLayoutError):
                resolve_layout(
                    replace(environment, partition=variant),
                    sdk,
                    directory,
                )

    def test_misplaced_application_slot_fails_closed(self) -> None:
        environment = resolve_environment("pico2-w")
        sdk = load_sdk_version("pico-sdk")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            variant = directory / "moved-app.json"
            document = json.loads(environment.partition.read_text(encoding="utf-8"))
            for item in document["partitions"]:
                if item["name"] == "application":
                    item["start"] = "12K"
            variant.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaises(PicoLayoutError):
                resolve_layout(
                    replace(environment, partition=variant),
                    sdk,
                    directory,
                )

    def test_ab_link_fails_closed(self) -> None:
        environment = resolve_environment("pico2-w")
        sdk = load_sdk_version("pico-sdk")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            variant = directory / "linked.json"
            document = json.loads(environment.partition.read_text(encoding="utf-8"))
            for item in document["partitions"]:
                if item["name"] == "safeboot":
                    item["link"] = ["a", 0]
            variant.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaises(PicoLayoutError):
                resolve_layout(
                    replace(environment, partition=variant),
                    sdk,
                    directory,
                )


if __name__ == "__main__":
    unittest.main()
