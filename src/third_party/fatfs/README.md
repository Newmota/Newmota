# FatFs

ChaN's FatFs R0.16 with patch 1 and patch 2 from
<http://elm-chan.org/fsw/ff/patches.html>. Patch 2 fixes CVE-2026-6682, CVE-2026-6683
and CVE-2026-6687. `USE_SDCARD` compiles this copy on every family; the FatFs copies in
the vendor SDKs are not compiled or linked.

| Input | SHA-256 |
| --- | --- |
| `ff16.zip` | `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e` |
| `ff16p1.diff` | `7996ddc3135f3d534a8153f7d75d587030e5c8551b003c3b7ee823ffb7fc64bf` |
| `ff16p2.diff` | `5cd39f1fc299f0f1dec9fa1ce544dc0bc048b2f6eb3b8161476ed20f9cf1e290` |

The archive's `source/` files are kept with LF line endings; the patches apply to them
unchanged. The sample `diskio.c` is omitted: Newmota's disk glue is
`src/app/core/storage/sd_volume.cpp`.

`ffconf.h` differs from the archive in these options:

| Option | Value | Reason |
| --- | --- | --- |
| `FF_CODE_PAGE` | 437 | Short names only; long names carry all characters |
| `FF_USE_LFN` | 3 | Long names with heap work buffers (`ffsystem.c`) |
| `FF_LFN_UNICODE` | 2 | UTF-8 paths, the same as LittleFS |
| `FF_NORTC_YEAR` | 2026 | Year of the fixed timestamp `get_fattime()` uses before the clock is set |

exFAT stays off.
