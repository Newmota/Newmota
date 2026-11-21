"""Own the pinned BearSSL import identity and its selected source subsets."""

from __future__ import annotations

from newmota.model import ROOT, BuildError, Environment

# The import is pinned to one Tasmota commit and its tree. Builds never follow the moving
# branch; refreshing the pin is an explicit import change recorded in CUSTOMIZED.md.
BEARSSL_TASMOTA_COMMIT = "48da93dca8728c4c246b33a2d7f23c0ecaa056f0"
BEARSSL_TASMOTA_TREE = "d5c39303e1563137e6428365cb05f0f54b3b44c0"
BEARSSL_TASMOTA_PATH = "lib/lib_ssl/bearssl-esp8266"
BEARSSL_REVISION = BEARSSL_TASMOTA_COMMIT[:8]
BEARSSL_ROOT = ROOT / "src" / "third_party" / "bearssl"
BEARSSL_SOURCE_ROOT = BEARSSL_ROOT / "src"
BEARSSL_BLOB_MANIFEST = BEARSSL_ROOT / "IMPORT_BLOBS.txt"
BEARSSL_DELTA_MANIFEST = BEARSSL_ROOT / "DELTA_BLOBS.txt"

# Newmota files added beside the import. They are excluded from the blob comparison.
BEARSSL_METADATA = ("CUSTOMIZED.md", "IMPORT_BLOBS.txt", "DELTA_BLOBS.txt")

# The fork prefixed every BearSSL header with "t_" so that it would not collide with the
# Arduino ESP8266 core's own copies. Newmota never builds through Arduino, so the prefix is
# removed and every include follows. This is the only change most files receive.
HEADER_RENAMES: dict[str, str] = {
    "src/t_bearssl.h": "src/bearssl.h",
    "src/t_bearssl_aead.h": "src/bearssl_aead.h",
    "src/t_bearssl_block.h": "src/bearssl_block.h",
    "src/t_bearssl_ec.h": "src/bearssl_ec.h",
    "src/t_bearssl_hash.h": "src/bearssl_hash.h",
    "src/t_bearssl_hmac.h": "src/bearssl_hmac.h",
    "src/t_bearssl_kdf.h": "src/bearssl_kdf.h",
    "src/t_bearssl_pem.h": "src/bearssl_pem.h",
    "src/t_bearssl_prf.h": "src/bearssl_prf.h",
    "src/t_bearssl_rand.h": "src/bearssl_rand.h",
    "src/t_bearssl_rsa.h": "src/bearssl_rsa.h",
    "src/t_bearssl_ssl.h": "src/bearssl_ssl.h",
    "src/t_bearssl_x509.h": "src/bearssl_x509.h",
    "src/t_config.h": "src/config.h",
    "src/t_inner.h": "src/inner.h",
    "src/t_bearssl_tasmota_config.h": "src/bearssl_newmota_config.h",
}

# Files dropped from the pinned tree, with the reason.
REMOVED_SOURCES: dict[str, str] = {
    "bearssl_esp8266-customized.txt": "ESP8266 and Arduino fork record",
    "conf/esp8266.mk": "ESP8266 build configuration",
    "library.properties": "Arduino library metadata",
    "src/settings.c": "Arduino configuration table that only listed the removed ESP8266 macro",
}

# Files changed beyond the mechanical header rename, with the reason. Everything else that
# differs from the pinned tree differs only in an include line.
EDITED_SOURCES: dict[str, str] = {
    "src/bearssl.h": "point the configuration documentation at the renamed headers",
    "src/bearssl_newmota_config.h": "Newmota configuration header under the BEARSSL_NEWMOTA_CONFIG guard, keeping the "
    "same bounds and replacing the Arduino-IDE marker with a statement of purpose",
    "src/config.h": "drop the BR_USE_ESP8266_RAND documentation block",
    "src/inner.h": "restrict the low-multiplication and yield-hook detection to the targets that "
    "remain, drop the ESP8266 entropy macro and the unused debug marker, and name "
    "Newmota as the owner of the explicit time source",
    "src/ec/ec_all_m15.c": "state the BR_MAX_EC_SIZE curve-advertisement constraint without naming the fork "
    "that introduced it",
    "src/ec/ec_prime_i15.c": "compile and advertise P-521 only when BR_MAX_EC_SIZE >= 521, since the i15 "
    "buffers are sized from it",
    "src/ec/ec_secp521r1.c": "compile the P-521 curve definition only when BR_MAX_EC_SIZE >= 521",
    "src/ec/ecdsa_i15_sign_raw.c": "accept P-521 only when BR_MAX_EC_SIZE >= 521",
    "src/ec/ecdsa_i15_vrfy_raw.c": "accept P-521 only when BR_MAX_EC_SIZE >= 521",
    "src/pgmspace_bearssl.h": "keep the accessor shim the imported integer, hash, and curve code calls, without "
    "the ESP8266 include and the unused accessors",
    "src/rand/sysrng.c": "keep the ESP32 entropy seeder and drop the ESP8266 phy_get_rand one",
    "src/ec/_ec_c25519_m15.c": "drop the ESP8266 guard from the ESP32 ROM implementation",
    "src/ec/_ec_p256_m15.c": "drop the ESP8266 guard from the ESP32 ROM implementation",
    "src/hash/_sha_hal_idf5x.c": "drop the ESP8266 guard and comment from the ESP-IDF SHA implementation",
}

# Identifiers that must not come back into the import. The ESP32 ROM accelerator uses the
# ets_bigint_* family, which is an ESP32 API and stays.
FORBIDDEN_IMPORT_PATTERNS: tuple[str, ...] = (
    "ESP8266",
    "__ets__",
    "ICACHE_FLASH",
    "esp8266",
    "phy_get_rand",
    "sys/pgmspace.h",
    "tasmota",
    "Tasmota",
    "TASMOTA",
)

# One portable software primitive set per selection unit. Paths are relative to the
# imported src/ directory. ESP32 ROM/HAL replacements and the x86-only implementations
# are never part of a portable selection: they stay unselected until their own gate.
PRIMITIVE_SOURCES: dict[str, tuple[str, ...]] = {
    "CODEC": (
        "codec/ccopy.c",
        "codec/dec16be.c",
        "codec/dec16le.c",
        "codec/dec32be.c",
        "codec/dec32le.c",
        "codec/dec64be.c",
        "codec/dec64le.c",
        "codec/enc16be.c",
        "codec/enc16le.c",
        "codec/enc32be.c",
        "codec/enc32le.c",
        "codec/enc64be.c",
        "codec/enc64le.c",
    ),
    "MD5": ("hash/md5.c",),
    "SHA1": ("hash/sha1.c",),
    "SHA224_SHA256": ("hash/sha2small.c",),
    "SHA384_SHA512": ("hash/sha2big.c",),
    "GHASH": ("hash/ghash_ctmul.c", "hash/ghash_ctmul32.c"),
    "MULTIHASH": ("hash/dig_oid.c", "hash/dig_size.c", "hash/multihash.c"),
    "HMAC": ("mac/hmac.c", "mac/hmac_ct.c"),
    "HKDF_PBKDF2": ("kdf/hkdf.c",),
    "AES": (
        "symcipher/aes_big_cbcdec.c",
        "symcipher/aes_big_cbcenc.c",
        "symcipher/aes_big_ctr.c",
        "symcipher/aes_big_ctrcbc.c",
        "symcipher/aes_big_dec.c",
        "symcipher/aes_big_enc.c",
        "symcipher/aes_common.c",
        "symcipher/aes_ct.c",
        "symcipher/aes_ct64.c",
        "symcipher/aes_ct64_cbcdec.c",
        "symcipher/aes_ct64_cbcenc.c",
        "symcipher/aes_ct64_ctr.c",
        "symcipher/aes_ct64_ctrcbc.c",
        "symcipher/aes_ct64_dec.c",
        "symcipher/aes_ct64_enc.c",
        "symcipher/aes_ct_cbcdec.c",
        "symcipher/aes_ct_cbcenc.c",
        "symcipher/aes_ct_ctr.c",
        "symcipher/aes_ct_ctrcbc.c",
        "symcipher/aes_ct_dec.c",
        "symcipher/aes_ct_enc.c",
        "symcipher/aes_small_cbcdec.c",
        "symcipher/aes_small_cbcenc.c",
        "symcipher/aes_small_ctr.c",
        "symcipher/aes_small_ctrcbc.c",
        "symcipher/aes_small_dec.c",
        "symcipher/aes_small_enc.c",
    ),
    "CHACHA20": ("symcipher/chacha20_ct.c",),
    "POLY1305": (
        "symcipher/poly1305_ctmul.c",
        "symcipher/poly1305_ctmul32.c",
        "symcipher/poly1305_ctmulq.c",
        "symcipher/poly1305_i15.c",
    ),
    "GCM": ("aead/gcm.c",),
    "CCM": ("aead/ccm.c",),
    "EAX": ("aead/eax.c",),
    "INT_I15": (
        "int/i15_add.c",
        "int/i15_bitlen.c",
        "int/i15_decmod.c",
        "int/i15_decode.c",
        "int/i15_decred.c",
        "int/i15_encode.c",
        "int/i15_fmont.c",
        "int/i15_iszero.c",
        "int/i15_moddiv.c",
        "int/i15_modpow.c",
        "int/i15_modpow2.c",
        "int/i15_montmul.c",
        "int/i15_mulacc.c",
        "int/i15_muladd.c",
        "int/i15_ninv15.c",
        "int/i15_reduce.c",
        "int/i15_rshift.c",
        "int/i15_sub.c",
        "int/i15_tmont.c",
    ),
    "EC": (
        "ec/ec_all_m15.c",
        "ec/ec_c25519_m15.c",
        "ec/ec_curve25519.c",
        "ec/ec_default.c",
        "ec/ec_keygen.c",
        "ec/ec_p256_m15.c",
        "ec/ec_prime_i15.c",
        "ec/ec_secp256r1.c",
        "ec/ec_secp384r1.c",
        "ec/ec_secp521r1.c",
    ),
    "ECDSA_VERIFY": (
        "ec/ec_pubkey.c",
        "ec/ecdsa_atr.c",
        "ec/ecdsa_i15_bits.c",
        "ec/ecdsa_i15_vrfy_asn1.c",
        "ec/ecdsa_i15_vrfy_raw.c",
        "ec/ecdsa_default_vrfy_asn1.c",
        "ec/ecdsa_default_vrfy_raw.c",
        "ec/ecdsa_rta.c",
    ),
    "ECDSA_SIGN": (
        "ec/ecdsa_i15_sign_asn1.c",
        "ec/ecdsa_i15_sign_raw.c",
        "ec/ecdsa_default_sign_asn1.c",
        "ec/ecdsa_default_sign_raw.c",
        "rand/hmac_drbg.c",
    ),
    # The TLS 1.2 PRF alone, for protocols that derive keys like TLS without its engine.
    "TLS12_PRF": ("ssl/prf.c", "ssl/prf_sha256.c"),
    "RSA_SIGN": (
        "rsa/rsa_i15_pkcs1_sign.c",
        "rsa/rsa_pkcs1_sig_pad.c",
        "rsa/rsa_i15_priv.c",
        "x509/skey_decoder.c",
    ),
}

# Imported sources that no portable selection uses, matched by path prefix, with the
# reason the selection cannot include them yet. The ESP32 replacements and the TLS,
# X.509, RSA, and random-generator layers are separate work with their own gates.
UNSELECTED_REASONS: dict[str, str] = {
    "ec/_ec_c25519_m15.c": "ESP32 ROM EC implementation behind USE_SHA_ROM",
    "ec/_ec_p256_m15.c": "ESP32 ROM EC implementation behind USE_SHA_ROM",
    "hash/_sha_hal_idf5x.c": "ESP-IDF SHA HAL implementation behind USE_SHA_ROM",
    "hash/ghash_ctmul64.c": "SSE2-only GHASH, not a portable selection",
    "hash/ghash_pclmul.c": "x86 PCLMULQDQ-only GHASH, not a portable selection",
    "hash/md5sha1.c": "only the TLS PRF needs it; MD5 and SHA-1 are selected directly",
    "hash/mgf1.c": "only the RSA key generation path needs it",
    "kdf/shake.c": "SHA-3, deferred",
    "symcipher/chacha20_sse2.c": "SSE2-only ChaCha20, not a portable selection",
    "symcipher/des_": "DES is outside the required primitive surface",
    "codec/pemdec.c": "PEM decoding belongs to the later application-TLS work",
    "codec/pemenc.c": "PEM encoding belongs to the later application-TLS work",
    "rsa/": "the Berry RSA surface is Phase 1 work with its own gate",
    "rand/": "needs the platform entropy contract",
    "ssl/": "the TLS engine is later application-TLS work",
    "x509/": "certificate validation is later application-TLS work",
    # RFC 6979 signers derive deterministic nonces from the private key and
    # reduced message hash without external entropy. Signing is Phase 1 scope.
    "ec/ecdsa_default_sign_asn1.c": "deterministic RFC 6979 signing is Phase 1 work",
    "ec/ecdsa_default_sign_raw.c": "deterministic RFC 6979 signing is Phase 1 work",
    "ec/ecdsa_i15_sign_asn1.c": "deterministic RFC 6979 signing is Phase 1 work",
    "ec/ecdsa_i15_sign_raw.c": "deterministic RFC 6979 signing is Phase 1 work",
}

# The host baseline compiles the portable subset with the 15-bit low-multiplication
# selection, which is the same m15 implementation Tasmota links. The remaining
# directories of the import are TLS, X.509, RSA, and the random generators.
HOST_PRIMITIVES = tuple(PRIMITIVE_SOURCES)

# Defines every host build of the imported sources needs: this tree carries only the
# 15-bit objects, while its default-dispatch code references the 31-bit ones.
HOST_DEFINES = ("BR_LOMUL=1",)

# Local extensions live beside the pinned import, never inside its blob manifest.
EXTENSION_SOURCES = {
    "SUPPORT": ("support.c",),
    "PBKDF2_SHA256": ("pbkdf2.c",),
    "BLAKE2S": ("blake2s.c",),
    "RAW_POLY1305": ("poly1305.c",),
    "XCHACHA20_POLY1305": ("xchacha20.c",),
    "ED25519": ("ed25519.c",),
}


def extension_sources(names: tuple[str, ...]) -> tuple[str, ...]:
    """Resolve explicit local extension units independently of the pinned import."""

    return tuple(
        sorted(
            {
                f"src/third_party/bearssl/extensions/{source}"
                for name in names
                for source in EXTENSION_SOURCES[name]
            }
        )
    )


def unselected_reason(source: str) -> str | None:
    """Return why an imported source stays unselected, or None when it may be used."""

    if any(source in sources for sources in PRIMITIVE_SOURCES.values()):
        return None
    reason = None
    for prefix, candidate in UNSELECTED_REASONS.items():
        if source.startswith(prefix):
            reason = candidate
    return reason


def imported_sources() -> tuple[str, ...]:
    """Return every imported C source, relative to the imported src directory."""

    return tuple(
        sorted(
            str(path.relative_to(BEARSSL_SOURCE_ROOT)) for path in BEARSSL_SOURCE_ROOT.rglob("*.c")
        )
    )


def read_blob_manifest(path) -> dict[str, str]:
    """Return the paths and git blob names recorded in one manifest."""

    entries = {}
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        blob, _, name = line.partition("  ")
        entries[name] = blob
    return entries


def recorded_blobs() -> dict[str, str]:
    """Return the pinned tree's recorded paths and their git blob names."""

    return read_blob_manifest(BEARSSL_BLOB_MANIFEST)


def delta_blobs() -> dict[str, str]:
    """Return the blob names every file of the import must have after the delta."""

    return read_blob_manifest(BEARSSL_DELTA_MANIFEST)


def import_identity() -> dict[str, str]:
    """Return the recorded provenance of the pinned import."""

    return {
        "tasmota_commit": BEARSSL_TASMOTA_COMMIT,
        "tasmota_tree": BEARSSL_TASMOTA_TREE,
        "tasmota_path": BEARSSL_TASMOTA_PATH,
        "revision": BEARSSL_REVISION,
    }


def resolve_primitive_sources(names: tuple[str, ...]) -> tuple[str, ...]:
    """Return the imported source list for a primitive selection, in a stable order."""

    selected: set[str] = set()
    for name in names:
        try:
            sources = PRIMITIVE_SOURCES[name]
        except KeyError:
            raise ValueError(f"unknown crypto primitive: {name}") from None
        selected.update(sources)
    return tuple(sorted(selected))


_HARDWARE_HASH_UNITS = {
    "USE_CRYPTO_HW_SHA1": ("SHA1", "sha1"),
    "USE_CRYPTO_HW_SHA256": ("SHA224_SHA256", "sha2small"),
    "USE_CRYPTO_HW_SHA512": ("SHA384_SHA512", "sha2big"),
}


def _drop_selected(selected: set[str], sources: tuple[str, ...], feature: str) -> None:
    """Remove the software units a hardware provider replaces; all of them must be selected."""

    paths = set(_repository_sources(sources))
    if not paths <= selected:
        raise BuildError(
            f"{feature} replaces BearSSL sources that are not selected: {sorted(paths - selected)}"
        )
    selected -= paths


def _repository_sources(sources: tuple[str, ...]) -> tuple[str, ...]:
    """Return repository-relative paths for selected imported sources."""

    resolved = []
    for source in sources:
        path = BEARSSL_SOURCE_ROOT / source
        if not path.is_file():
            raise FileNotFoundError(f"selected BearSSL source is missing: {path}")
        resolved.append(str(path.relative_to(ROOT)))
    return tuple(resolved)


def host_sources() -> tuple[str, ...]:
    """Return the repository-relative sources of the host crypto baseline."""

    return _repository_sources(resolve_primitive_sources(HOST_PRIMITIVES))


# SDK targets whose hardware implements a complete BearSSL primitive.
# Each C provider repeats these limits as SoC capability #error guards.
_SHA_RESUME_TARGETS = frozenset(
    {"esp32s2", "esp32s3", "esp32s31", "esp32c2", "esp32c3", "esp32c5", "esp32c6", "esp32c61"}
    | {"esp32h2", "esp32p4", "bl602", "bl616", "bl618dg"}
)
# P-256 on a complete ECC engine, else field multiplication on the MPI engine. ESP32-H2's ECC
# engine is constant-time only from revision 1.2, which a build cannot know, so H2 uses MPI.
_P256_ECC_TARGETS = frozenset({"esp32c5", "esp32c61", "esp32p4", "esp32s31"})
_P256_MPI_TARGETS = frozenset({"esp32", "esp32s2", "esp32s3", "esp32c3", "esp32c6", "esp32h2"})
HARDWARE_CRYPTO_TARGETS: dict[str, frozenset[str]] = {
    "USE_CRYPTO_HW_SHA1": _SHA_RESUME_TARGETS,
    "USE_CRYPTO_HW_SHA256": _SHA_RESUME_TARGETS,
    "USE_CRYPTO_HW_SHA512": frozenset(
        {"esp32s2", "esp32s3", "esp32s31", "esp32c5", "esp32p4", "bl616", "bl618dg"}
    ),
    "USE_CRYPTO_HW_P256": _P256_ECC_TARGETS | _P256_MPI_TARGETS,
    "USE_CRYPTO_HW_AES": frozenset({"esp32", "esp32s2", "bl602", "bl616", "bl618dg", "rtl8721d"}),
}
_HARDWARE_PROVIDER_DIRS = {"esp": "esp-idf", "bouffalo": "bouffalo", "amebad": "amebad"}


def hardware_crypto_sources(
    environment: Environment, values: dict[str, bool], sources: tuple[str, ...]
) -> tuple[str, ...]:
    """Replace complete selected primitive providers; never retain software fallback."""
    selected = set(sources)
    for feature, targets in HARDWARE_CRYPTO_TARGETS.items():
        if not values.get(feature):
            continue
        if environment.sdk_target not in targets:
            raise BuildError(f"{feature} has no complete hardware provider for {environment.name}")
        platform = _HARDWARE_PROVIDER_DIRS[environment.platform]
        if feature == "USE_CRYPTO_HW_P256":
            _drop_selected(selected, ("ec/ec_p256_m15.c",), feature)
            ecc = environment.sdk_target in _P256_ECC_TARGETS
            selected.add(f"src/platform/esp-idf/crypto_p256{'' if ecc else '_mpi'}.c")
        elif feature == "USE_CRYPTO_HW_AES":
            _drop_selected(selected, PRIMITIVE_SOURCES["AES"], feature)
            selected.add("src/platform/crypto/bearssl_aes.c")
            selected.add(f"src/platform/{platform}/crypto_aes.c")
        else:
            primitive, unit = _HARDWARE_HASH_UNITS[feature]
            _drop_selected(selected, PRIMITIVE_SOURCES[primitive], feature)
            selected.add(f"src/platform/crypto/bearssl_{unit}.c")
            selected.add(f"src/platform/{platform}/crypto_hash.c")
    return tuple(sorted(selected))


def host_build_inputs() -> tuple[str, ...]:
    """Return the defines and sources of the host crypto baseline, in build order."""

    return (*HOST_DEFINES, *host_sources(), *extension_sources(tuple(EXTENSION_SOURCES)))
