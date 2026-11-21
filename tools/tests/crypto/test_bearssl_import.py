"""Integrity and provenance regressions for the pinned BearSSL import (no devices)."""

from __future__ import annotations

import hashlib
import unittest
from pathlib import Path

from newmota.crypto import (
    BEARSSL_METADATA,
    BEARSSL_ROOT,
    BEARSSL_SOURCE_ROOT,
    HEADER_RENAMES,
    HOST_DEFINES,
    HOST_PRIMITIVES,
    REMOVED_SOURCES,
    delta_blobs,
    host_sources,
    imported_sources,
    recorded_blobs,
    resolve_primitive_sources,
    unselected_reason,
)
from newmota.model import ROOT


def git_blob_sha1(path: Path) -> str:
    """Return the git blob object name of one file."""

    data = path.read_bytes()
    header = b"blob %d\0" % len(data)
    return hashlib.sha1(header + data).hexdigest()


def present_import_paths() -> set[str]:
    """Return imported source paths relative to the BearSSL directory."""

    return {
        f"src/{path.relative_to(BEARSSL_SOURCE_ROOT)}"
        for path in BEARSSL_SOURCE_ROOT.rglob("*")
        if path.is_file()
    }


class BearSSLImportTest(unittest.TestCase):
    def test_declared_import_paths_match_the_tree(self):
        expected = set(recorded_blobs()) - set(REMOVED_SOURCES) - set(HEADER_RENAMES)
        expected.update(HEADER_RENAMES.values())
        self.assertEqual(present_import_paths(), expected)
        for name in BEARSSL_METADATA:
            self.assertTrue((BEARSSL_ROOT / name).is_file(), name)

    def test_delta_manifest_matches_paths_and_blob_hashes(self):
        recorded = delta_blobs()
        self.assertEqual(set(recorded), present_import_paths())
        for name, blob in recorded.items():
            with self.subTest(path=name):
                self.assertEqual(git_blob_sha1(BEARSSL_ROOT / name), blob)

    def test_selected_sources_exist_and_unselected_sources_have_reasons(self):
        selected = resolve_primitive_sources(HOST_PRIMITIVES)
        self.assertEqual(len(selected), len(set(selected)))
        for source in host_sources():
            self.assertTrue((ROOT / source).is_file(), source)
        for source in imported_sources():
            if source not in selected:
                self.assertTrue(unselected_reason(source), source)

    def test_host_selection_excludes_accelerated_and_platform_sources(self):
        for source in (
            "ec/_ec_c25519_m15.c",
            "ec/_ec_p256_m15.c",
            "hash/_sha_hal_idf5x.c",
        ):
            self.assertNotIn(source, resolve_primitive_sources(HOST_PRIMITIVES))

    def test_host_defines_select_the_low_multiplication_portable_path(self):
        self.assertIn("BR_LOMUL=1", HOST_DEFINES)
        selected = resolve_primitive_sources(HOST_PRIMITIVES)
        self.assertIn("ec/ec_all_m15.c", selected)
        self.assertNotIn("ec/ec_all_m31.c", selected)


if __name__ == "__main__":
    unittest.main()
