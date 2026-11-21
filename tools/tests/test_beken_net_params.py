"""Persistent-data safety checks for Beken factory NET initialization."""

from __future__ import annotations

import struct
import unittest

from newmota.platforms.beken.net_params import (
    CRITICAL_RECORD_SPACE,
    NET_SECTOR_SIZE,
    NEWMOTA_RECORD_TYPE,
    TABLE_TYPE,
    factory_net_sector,
)


def record(kind: int, payload: bytes) -> bytes:
    return struct.pack("<II", kind, len(payload)) + payload


def table(payload: bytes) -> bytes:
    return record(TABLE_TYPE, payload).ljust(NET_SECTOR_SIZE, b"\xff")


class BekenNetParamsTest(unittest.TestCase):
    def test_erased_and_foreign_configuration_become_empty_native_tables(self):
        for source in (b"\xff" * NET_SECTOR_SIZE, b"CFG\xdd\x05".ljust(NET_SECTOR_SIZE, b"\x00")):
            with self.subTest(header=source[:3]):
                self.assertEqual(factory_net_sector(source), table(b""))

    def test_native_hardware_records_survive_removal_of_all_newmota_records(self):
        mac = record(0x44444444, b"\x02\x11\x22\x33\x44\x55\x00\x00")
        rf = record(0x77777777, b"\x01\x02\x03\x04")
        unknown = record(0x12345678, b"vendor")
        owned = record(NEWMOTA_RECORD_TYPE, b"obsolete configuration")
        source = table(mac + owned + rf + owned + unknown)
        result = factory_net_sector(source)
        self.assertEqual(result, table(mac + rf + unknown))
        self.assertEqual(factory_net_sector(result), result)

    def test_undeclared_trailing_bytes_are_preserved(self):
        source = bytearray(table(record(NEWMOTA_RECORD_TYPE, b"old")))
        source[-4:] = b"tail"
        expected = bytearray(table(b""))
        expected[-4:] = source[-4:]
        self.assertEqual(factory_net_sector(bytes(source)), bytes(expected))

    def test_malformed_native_tables_and_incomplete_reads_are_rejected(self):
        cases = (
            b"\xff" * (NET_SECTOR_SIZE - 1),
            struct.pack("<II", TABLE_TYPE, NET_SECTOR_SIZE).ljust(NET_SECTOR_SIZE, b"\xff"),
            table(b"short"),
            table(struct.pack("<II", 0x44444444, 100)),
        )
        for source in cases:
            with self.subTest(length=len(source)), self.assertRaises(ValueError):
                factory_net_sector(source)

    def test_vendor_data_cannot_be_overwritten_to_make_credential_space(self):
        source = table(record(0x12345678, b"v" * (NET_SECTOR_SIZE - 16)))
        with self.assertRaisesRegex(ValueError, "no erased room"):
            factory_net_sector(source)
        source = bytearray(table(b""))
        source[8 + CRITICAL_RECORD_SPACE - 1] = 0
        with self.assertRaisesRegex(ValueError, "no erased room"):
            factory_net_sector(bytes(source))


if __name__ == "__main__":
    unittest.main()
