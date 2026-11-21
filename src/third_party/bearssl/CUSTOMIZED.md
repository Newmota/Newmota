# BearSSL import record

The `src/` directory contains the Tasmota BearSSL import used for Newmota's crypto work.
`IMPORT_BLOBS.txt` records the complete pinned tree and `DELTA_BLOBS.txt` records the blob
name every retained file has after the local delta. The delta itself is declared once in
[`tools/newmota/crypto.py`](../../../tools/newmota/crypto.py): the removals, the header
renames, and the reason for each edited file.

## Pinned source

| Field | Value |
| --- | --- |
| Upstream repository | `https://github.com/arendst/Tasmota`, `development` |
| Tasmota commit | `48da93dca8728c4c246b33a2d7f23c0ecaa056f0` |
| Tasmota tree | `d5c39303e1563137e6428365cb05f0f54b3b44c0` |
| Imported path | `lib/lib_ssl/bearssl-esp8266/` |
| Recorded files | 215, of which 211 are retained |
| Local source directory | `src/third_party/bearssl/src/` |

The upstream project is `bearssl-esp8266`
(`https://github.com/earlephilhower/bearssl-esp8266`), a BearSSL fork by Earle F.
Philhower, III that avoids header collisions with the Arduino ESP8266 core. BearSSL
itself is by Thomas Pornin, author of BearSSL and maintainer of `library.properties` in
the pinned tree. The fork renamed headers `bearssl*` to `t_bearssl*`, `inner.h` to
`t_inner.h`, and `config.h` to `t_config.h`, and added a Tasmota configuration header plus
`pgmspace_bearssl.h`, so that the tree could sit next to the Arduino ESP8266 core's own
BearSSL headers. Newmota does not build through Arduino, so the local delta restores the
upstream header names. Only the file names ever changed; the public `br_*` symbols are
unchanged.

## License

BearSSL and `bearssl-esp8266` are MIT licensed. Every imported `src/*.c` and `*.h` file
carries the MIT permission notice in its own header, except the files that upstream
BearSSL marks as automatically generated (`src/codec/pemdec.c`, `src/ssl/ssl_hs_client.c`,
`src/ssl/ssl_hs_server.c`, `src/x509/pkey_decoder.c`, `src/x509/skey_decoder.c`,
`src/x509/x509_decoder.c`, `src/x509/x509_minimal.c`), which carry the generated-code
notice instead, and the three ESP32 ROM replacements below, which carry an author
attribution. The pinned tree contains no separate `LICENSE` file, so these header
notices are the license record for this import.

## Tasmota and ESP32 customizations already present in the import

These files differ from a stock `bearssl-esp8266` snapshot and are part of the pinned
content, not Newmota changes:

- `src/bearssl_newmota_config.h` sets `BR_SLOW_MUL15`, `BR_MAX_RSA_SIZE` (2048, or 4096
  with `USE_4K_RSA`), and `BR_MAX_EC_SIZE` (384). It arrived as `t_bearssl_tasmota_config.h`
  with a `BEARSSL_TASMOTA_CONFIG` guard and also defined the ESP8266 `__ets__` and
  `ICACHE_FLASH` markers; the local delta renames and trims it.
- The pinned tree includes `src/pgmspace_bearssl.h` for flash-memory compatibility. The
  Newmota delta removes its ESP8266 branch and unused accessors; the remaining small shim
  supplies the flat-memory definitions still used by the imported fork's integer, hash,
  curve, and cipher code. The i15 code calls `pgm_read_byte`, `pgm_read_word`, `memcpy_P`,
  and `memcmp_P`, so those definitions remain necessary.
- `src/hash/_sha_hal_idf5x.c`, `src/ec/_ec_p256_m15.c`, and `src/ec/_ec_c25519_m15.c`
  are ESP32 ROM/HAL replacements behind `USE_SHA_ROM`. They are never selected by the host
  baseline and must not be enabled for a platform before that backend's own gate passes.
- `src/inner.h` and `src/hash/sha1.c`, `src/hash/sha2big.c`, `src/hash/sha2small.c`
  carry the ESP32 stack-thunk hook, the `PROGMEM` handling, the NTP time source, and the
  ESP-IDF 5.5 SHA interface change.

## Newmota-added files in this directory

| File | Purpose |
| --- | --- |
| `CUSTOMIZED.md` | This provenance, license, and delta record |
| `IMPORT_BLOBS.txt` | Git blob SHA-1 of every file of the pinned tree, the provenance record |
| `DELTA_BLOBS.txt` | Git blob SHA-1 every retained file has after the local delta |

## Local delta

Two changes make the tree Newmota's to carry. First, the fork prefixed every BearSSL header
with `t_` so the tree could sit next to the Arduino ESP8266 core's own BearSSL headers.
Newmota does not build through Arduino, so the prefix is removed, the upstream header names
come back, and every include follows. Second, the ESP8266 and Arduino-only parts of the fork
are removed rather than carried as dead code, and the remaining Tasmota branding becomes
Newmota branding.

`HEADER_RENAMES`, `REMOVED_SOURCES`, and `EDITED_SOURCES` in
[`tools/newmota/crypto.py`](../../../../tools/newmota/crypto.py) declare this. For a file
that is not listed in `EDITED_SOURCES`, the include line is the entire diff.

Renamed files:

| Pinned path | Our path |
| --- | --- |
| `src/t_bearssl.h`, `src/t_bearssl_aead.h`, `src/t_bearssl_block.h`, `src/t_bearssl_ec.h`, `src/t_bearssl_hash.h`, `src/t_bearssl_hmac.h`, `src/t_bearssl_kdf.h`, `src/t_bearssl_pem.h`, `src/t_bearssl_prf.h`, `src/t_bearssl_rand.h`, `src/t_bearssl_rsa.h`, `src/t_bearssl_ssl.h`, `src/t_bearssl_x509.h` | the same names without the `t_` prefix |
| `src/t_config.h`, `src/t_inner.h` | `src/config.h`, `src/inner.h` |
| `src/t_bearssl_tasmota_config.h` | `src/bearssl_newmota_config.h` |

The include guards were already unprefixed in the pinned tree, so no guard changed.

Removed files:

| File | Reason |
| --- | --- |
| `bearssl_esp8266-customized.txt` | the fork's ESP8266 and Arduino renaming record |
| `conf/esp8266.mk` | the ESP8266 build configuration; the directory held nothing else |
| `library.properties` | Arduino library metadata, including its `esp8266,esp32` architectures |
| `src/settings.c` | the Arduino configuration table, which only listed the removed ESP8266 macro |

Edited files:

| File | Change |
| --- | --- |
| `src/bearssl.h` | the configuration documentation points at the renamed headers |
| `src/bearssl_newmota_config.h` | the `BEARSSL_NEWMOTA_CONFIG` guard, the same size bounds, and a comment that states its purpose instead of the Arduino-IDE "do not delete" marker |
| `src/config.h` | includes the renamed header, and the `BR_USE_ESP8266_RAND` documentation block is gone |
| `src/inner.h` | the low-multiplication and stack-yield detection now follow the targets that remain, the ESP8266 entropy macro and `_debugBearSSL` marker are gone, and the time-source comment names Newmota |
| `src/pgmspace_bearssl.h` | kept the markers and accessors the imported code calls; removed the ESP8266 `sys/pgmspace.h` include and the accessors nothing calls |
| `src/rand/sysrng.c` | kept the ESP32 `esp_random` seeder and removed the ESP8266 `phy_get_rand` one, renaming it accordingly |
| `src/ec/ec_all_m15.c` | the `BR_MAX_EC_SIZE` curve-advertisement comment states the constraint without naming the fork that introduced it |
| `src/ec/ec_prime_i15.c`, `src/ec/ec_secp521r1.c`, `src/ec/ecdsa_i15_sign_raw.c`, `src/ec/ecdsa_i15_vrfy_raw.c` | P-521 is compiled, advertised, and accepted only when `BR_MAX_EC_SIZE` is at least 521, as upstream Tasmota did later (`479e99612`); with the configured 384, `br_ec_prime_i15` no longer offers a curve its i15 buffers cannot hold |
| `src/ec/_ec_p256_m15.c`, `src/ec/_ec_c25519_m15.c`, `src/hash/_sha_hal_idf5x.c` | dropped the `!defined(ESP8266)` guard and comment, which were inert on every target that compiles them |

Every other edit is inert for the targets that compile the file: Newmota never defines `ESP8266`,
and `BR_LOMUL=1` already selects the only implementation set the tree ships. The P-521 guard
is the one behavioural edit: it removes a curve the configured `BR_MAX_EC_SIZE` cannot hold,
which no Newmota caller selects. No attribution
is rewritten; a comment that credited the fork is restated without an owner instead. Any
later source change is a separate reviewed delta declared in the same place. Host build
shims, source selection, and portable wrappers live outside this directory.

## Reproducing the import

```bash
# From the NewmotaNG root, with a Tasmota clone in TASMOTA_ROOT.
git -C "$TASMOTA_ROOT" archive --format=tar 48da93dca8728c4c246b33a2d7f23c0ecaa056f0 \
    lib/lib_ssl/bearssl-esp8266 | tar -x -C src/third_party/bearssl --strip-components=3
```

`tools/tests/crypto/test_bearssl_import.py` (run by `make test-crypto`) checks that `IMPORT_BLOBS.txt` still describes the
pinned tree, that `DELTA_BLOBS.txt` matches the working tree byte for byte, that every
recorded file is either present or declared removed or renamed, that no `t_` header or
include is left, and that no ESP8266, Arduino, or Tasmota identifier appears anywhere in the
import. Re-applying the delta after a fresh extraction is a mechanical edit of the files
listed above; the test is what proves the result.
