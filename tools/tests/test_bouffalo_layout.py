"""Host-side tests for Bouffalo native-layout resolution."""

from __future__ import annotations

import binascii
import struct
import tempfile
import unittest
from dataclasses import replace

from newmota.catalog import resolve_environment
from newmota.host import load_sdk_version
from newmota.model import ROOT
from newmota.platforms.bouffalo.device import (
    PT_MAGIC,
    PT_TABLE_SIZE,
    BouffaloProgrammerError,
    grow_filesystem_table,
    parse_partition_table,
    validate_installed_layout,
)
from newmota.platforms.bouffalo.images import (
    BouffaloLayoutError,
    expected_partition_entries,
    resolve_layout,
)


def setUpModule() -> None:
    (ROOT / "build" / "host").mkdir(parents=True, exist_ok=True)


def _table(entries: dict[str, dict[str, int]], age: int = 3) -> bytes:
    """Encode one compact native table copy, as the SDK packages it, with valid CRCs."""

    header = struct.pack("<IHHI", PT_MAGIC, 0, len(entries), age)
    header += struct.pack("<I", binascii.crc32(header) & 0xFFFFFFFF)
    body = b"".join(
        struct.pack(
            "<BBB9sIIIIII",
            fields["type"],
            fields["device"],
            0,
            name.encode(),
            fields["address0"],
            fields["address1"],
            fields["size0"],
            fields["size1"],
            0,
            0,
        )
        for name, fields in entries.items()
    )
    return header + body + struct.pack("<I", binascii.crc32(body) & 0xFFFFFFFF)


def _parse(data: bytes) -> dict[str, object]:
    """Parse a compact copy as the device reads it: one erased 4 KiB sector."""

    return parse_partition_table(data + b"\xff" * (PT_TABLE_SIZE - len(data)))


class BouffaloLayoutTest(unittest.TestCase):
    def test_layout_resolves_application_safeboot_and_filesystem(self) -> None:
        environment = resolve_environment("bl602-2m")
        sdk = load_sdk_version("bouffalo")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            layout = resolve_layout(environment, sdk, ROOT / temporary)

        self.assertEqual(layout.platform, "bouffalo")
        self.assertEqual(layout.flash_bytes, 2 * 1024 * 1024)
        self.assertEqual(layout.erase_size, 0x1000)
        application = layout.region("application")
        self.assertEqual((application.offset, application.size), (0x10000, 0x14C000))
        self.assertEqual(application.image_budget, 0x14C000)
        safeboot = layout.region("safeboot")
        self.assertEqual((safeboot.offset, safeboot.size), (0x15C000, 0x94000))
        self.assertEqual(safeboot.image_budget, 0x94000)
        filesystem = layout.region("littlefs")
        self.assertEqual((filesystem.offset, filesystem.size), (0x1F0000, 0x10000))
        self.assertEqual(
            {region.name for region in layout.regions},
            {"pt-table0", "pt-table1", "application", "safeboot", "littlefs"},
        )
        self.assertEqual(layout.application_update_reach, ("application",))
        self.assertTrue(layout.settings_filesystem_preserved)
        self.assertEqual(layout.overlaps, ())
        self.assertEqual(set(expected_partition_entries(environment)), {"Boot2", "FW", "lfs"})

    def test_safeboot_environment_resolves_the_same_layout(self) -> None:
        application = resolve_environment("bl602-2m")
        safeboot = resolve_environment("bl602-2m-safeboot")
        sdk = load_sdk_version("bouffalo")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            first = resolve_layout(application, sdk, directory / "app")
            second = resolve_layout(safeboot, sdk, directory / "safeboot")
        self.assertEqual(first.identity, second.identity)

    def test_four_mib_target_layouts_and_exact_recovery_pairs(self) -> None:
        sdk = load_sdk_version("bouffalo")
        for name, target in (("bl602-4m", "bl602"), ("bl616dk", "bl616"), ("bl618dgdk", "bl618dg")):
            with self.subTest(environment=name):
                application = resolve_environment(name)
                safeboot = resolve_environment(f"{name}-safeboot")
                self.assertEqual(application.sdk_target, target)
                self.assertEqual(application.partition.name, f"4m-{target}.toml")
                self.assertEqual(application.safeboot, safeboot.name)
                with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
                    directory = ROOT / temporary
                    layout = resolve_layout(application, sdk, directory / "app")
                    recovery = resolve_layout(safeboot, sdk, directory / "recovery")
                self.assertEqual(layout.identity, recovery.identity)
                self.assertEqual(layout.flash_bytes, 0x400000)
                self.assertEqual(layout.overlaps, ())
                app_size = 0x2A0000 if target == "bl602" else 0x260000
                recovery_offset = 0x2B0000 if target == "bl602" else 0x270000
                recovery_size = 0x100000 if target == "bl602" else 0x140000
                self.assertEqual(
                    (layout.region("application").offset, layout.region("application").size),
                    (0x10000, app_size),
                )
                self.assertEqual(
                    (layout.region("safeboot").offset, layout.region("safeboot").size),
                    (recovery_offset, recovery_size),
                )
                self.assertEqual(
                    (layout.region("littlefs").offset, layout.region("littlefs").size),
                    (0x3B0000, 0x50000),
                )
                self.assertEqual(
                    set(expected_partition_entries(application)), {"Boot2", "FW", "lfs"}
                )

    def test_previous_four_mib_layout_requires_factory_migration(self) -> None:
        for name in ("bl602-4m", "bl616dk", "bl618dgdk"):
            with self.subTest(environment=name):
                environment = resolve_environment(name)
                entries = expected_partition_entries(environment)
                table = {"entries": [dict(name=name, **fields) for name, fields in entries.items()]}
                validate_installed_layout(table, environment)
                if environment.sdk_target in {"bl616", "bl618dg"}:
                    previous_entries = {key: dict(fields) for key, fields in entries.items()}
                    previous_entries["FW"].update(size0=0x2A0000, address1=0x2B0000, size1=0x100000)
                    previous = {
                        "entries": [
                            dict(name=key, **fields) for key, fields in previous_entries.items()
                        ]
                    }
                    with self.assertRaisesRegex(BouffaloProgrammerError, "geometry differs"):
                        validate_installed_layout(previous, environment)
                entries["FW"].update(size0=0x2E0000, address1=0x2F0000)
                entries["lfs"].update(address0=0x3F0000, size0=0x10000)
                previous = {
                    "entries": [dict(name=name, **fields) for name, fields in entries.items()]
                }
                with self.assertRaisesRegex(BouffaloProgrammerError, "geometry differs"):
                    validate_installed_layout(previous, environment)

    def test_missing_littlefs_entry_fails_closed(self) -> None:
        environment = resolve_environment("bl602-2m")
        sdk = load_sdk_version("bouffalo")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            variant = directory / "no-lfs.toml"
            content = environment.partition.read_text(encoding="utf-8")
            start = content.index('name = "lfs"')
            entry_start = content.rindex("[[pt_entry]]", 0, start)
            entry_end = content.find("[[pt_entry]]", start)
            if entry_end < 0:
                entry_end = len(content)
            variant.write_text(content[:entry_start] + content[entry_end:], encoding="utf-8")
            with self.assertRaises(BouffaloLayoutError):
                resolve_layout(
                    replace(environment, partition=variant),
                    sdk,
                    directory,
                )

    def test_unknown_entry_fails_closed(self) -> None:
        environment = resolve_environment("bl602-2m")
        sdk = load_sdk_version("bouffalo")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            variant = directory / "unknown.toml"
            variant.write_text(
                environment.partition.read_text(encoding="utf-8")
                + '\n[[pt_entry]]\ntype = 11\nname = "mystery"\ndevice = 0\n'
                "address0 = 0x1FF000\nsize0 = 0x1000\naddress1 = 0\nsize1 = 0\nlen = 0\n",
                encoding="utf-8",
            )
            with self.assertRaises(BouffaloLayoutError):
                resolve_layout(
                    replace(environment, partition=variant),
                    sdk,
                    directory,
                )

    def test_misplaced_boot2_entry_fails_closed(self) -> None:
        environment = resolve_environment("bl602-2m")
        sdk = load_sdk_version("bouffalo")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            variant = directory / "moved-boot2.toml"
            variant.write_text(
                environment.partition.read_text(encoding="utf-8").replace(
                    'name = "Boot2"\ndevice = 0\naddress0 = 0\n',
                    'name = "Boot2"\ndevice = 0\naddress0 = 0x1000\n',
                    1,
                ),
                encoding="utf-8",
            )
            with self.assertRaises(BouffaloLayoutError):
                resolve_layout(
                    replace(environment, partition=variant),
                    sdk,
                    directory,
                )

    def test_misplaced_application_slot_fails_closed(self) -> None:
        environment = resolve_environment("bl602-2m")
        sdk = load_sdk_version("bouffalo")
        with tempfile.TemporaryDirectory(dir=ROOT / "build" / "host") as temporary:
            directory = ROOT / temporary
            variant = directory / "moved-fw.toml"
            variant.write_text(
                environment.partition.read_text(encoding="utf-8").replace(
                    "address0 = 0x10000", "address0 = 0x11000", 1
                ),
                encoding="utf-8",
            )
            with self.assertRaises(BouffaloLayoutError):
                resolve_layout(
                    replace(environment, partition=variant),
                    sdk,
                    directory,
                )


class BouffaloFilesystemGrowthTest(unittest.TestCase):
    MIB = 1024 * 1024

    def test_factory_table_grows_final_littlefs_to_physical_flash(self) -> None:
        environment = resolve_environment("bl602-2m")
        entries = expected_partition_entries(environment)
        original = _table(entries)
        grown = grow_filesystem_table(original, 0x1F0000, 2 * self.MIB, 4 * self.MIB)

        self.assertEqual((len(grown), grown[:16]), (len(original), original[:16]))
        table = _parse(grown)
        actual = {entry["name"]: entry for entry in table["entries"]}
        self.assertEqual(
            (actual["lfs"]["address0"], actual["lfs"]["size0"]), (0x1F0000, 4 * self.MIB - 0x1F0000)
        )
        for name in ("Boot2", "FW"):
            self.assertEqual(actual[name]["size0"], entries[name]["size0"])
        self.assertEqual(
            grow_filesystem_table(original, 0x1F0000, 2 * self.MIB, 2 * self.MIB), original
        )
        capped = _parse(grow_filesystem_table(original, 0x1F0000, 2 * self.MIB, 32 * self.MIB))
        lfs = next(entry for entry in capped["entries"] if entry["name"] == "lfs")
        self.assertEqual(lfs["address0"] + lfs["size0"], 16 * self.MIB)

    def test_growth_requires_littlefs_to_end_the_declared_layout(self) -> None:
        entries = expected_partition_entries(resolve_environment("bl602-2m"))
        entries["lfs"].update(size0=0x8000)
        with self.assertRaisesRegex(BouffaloProgrammerError, "final region"):
            grow_filesystem_table(_table(entries), 0x1F0000, 2 * self.MIB, 4 * self.MIB)

    def test_update_accepts_only_a_littlefs_grown_within_probed_flash(self) -> None:
        environment = resolve_environment("bl602-2m")
        entries = expected_partition_entries(environment)
        entries["lfs"].update(size0=4 * self.MIB - 0x1F0000)
        table = {"entries": [dict(name=name, **fields) for name, fields in entries.items()]}
        validate_installed_layout(table, environment, 4 * self.MIB)
        for flash_bytes in (None, 2 * self.MIB):
            with self.assertRaisesRegex(BouffaloProgrammerError, "geometry differs"):
                validate_installed_layout(table, environment, flash_bytes)


if __name__ == "__main__":
    unittest.main()
