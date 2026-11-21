# Optional Berry crypto tests

These files are optional, user-run tests. They are excluded from firmware generation,
solidification and its cache inputs. The host Berry VM runs all the same files unchanged in
strict mode, matching the firmware Berry service. `random.be` explicitly reports
unavailable nonempty entropy from the host adapter.

Run a separate file for each group: `inventory.be`, `hashes_kdf.be`, `aes.be`, `curves.be`,
`spake.be`, `chacha_blake.be`, `rsa.be` and `random.be`. Each selected API prints `PASS`; an API
absent from the firmware prints `SKIP`. Assertion errors and runtime errors are failures,
except the explicit `UNAVAILABLE random` result when the platform adapter supplies no entropy.
A completely absent `crypto`
module cannot run this suite. Build with `crypto: all` to select every API.

Once a script exists on the device's LittleFS, run it through the Berry console, for example:

```berry
Br compile('/crypto_test.be', 'file')()
```

Use the actual path of the script on the filesystem. The current browser filesystem page
is read-only and provides no script upload operation; this repository does not yet provide
a supported general file-placement workflow. Preparing files on-device remains a prerequisite
for these tests. Do not paste whole test files into the console: Berry source input is limited
to 2 KiB, and execution has a four-second guard. Run files separately; a timeout is a
failure, not a skipped vector. PBKDF2's 4096-iteration vector may require device qualification
against that guard.

Vectors include RFC 8032 Ed25519, RFC 7748 X25519, RFC 5869 HKDF, RFC 7693 BLAKE2s,
RFC 8439 Poly1305, NIST AES examples, PBKDF2 vectors retained from the pinned Tasmota
source, and the SPAKE2+ draft's published P-256 transcript. SPAKE also checks both parties
with Matter's truncated confirmation-key mode and rejects invalid peers. RSA uses a
publicly committed fixture identity: never deploy that key as an identity.

The SHA-256 checks interleave independent contexts, cross a compression boundary and
continue after reading a digest; HMAC also exercises a multi-block key and message.
Run these with each hardware option selected as well as the software configuration. No
Berry class takes an AES-192 key, so AES-192 is not reachable from these scripts. They
do not cover BearSSL's C-level context copy and state import APIs or prove that the
hardware engine executed.

`inventory.be` prints `IN` or `OUT` for every Berry crypto class and for `crypto.random`,
checks that each selected class has all of its methods, and ends with
`PASS inventory: <selected> of <total> classes`. For timings, see the
[`examples/crypto/crypto_bench.be`](../../../../../../../examples/crypto/crypto_bench.be) runtime example.

`random.be` checks the API and lengths, and reports nonempty adapter availability. The
supported ESP-IDF target must return bytes after the station driver starts; other current
platform adapters intentionally report `UNAVAILABLE`. This cannot establish entropy quality. Record the firmware revision, environment, exact board, operation and results for
hardware qualification. Host-side BearSSL known-answer and unavailable-entropy checks live
under `tools/tests/crypto/` and run with `make test-crypto`.
