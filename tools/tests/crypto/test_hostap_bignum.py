"""Randomized comparison of the hostap bignum backend with Python integers (no devices)."""

from __future__ import annotations

import random
import subprocess
import unittest
from pathlib import Path

from newmota.model import ROOT

FUZZ = ROOT / "build" / "host" / "hostap-bignum-fuzz"
P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
CAP = 1 << 1024  # NM_BN_LIMBS * 32 bits


def _hex(x: int) -> str:
    s = format(x, "x")
    return "0" + s if len(s) % 2 else s


def _operand(rng: random.Random, bits: int) -> int:
    k = min(rng.choice([1, 2, 31, 32, 33, 63, 64, 65, 96, 128, 255, 256, 257, 384, 511, 512, 513, 1000, bits]), bits)
    r = rng.random()
    if r < 0.05:
        return 0
    if r < 0.10:
        return (1 << k) - 1
    if r < 0.15:
        return 1
    return rng.getrandbits(k)


def _modulus(rng: random.Random) -> int:
    r = rng.random()
    if r < 0.3:
        return P
    if r < 0.5:
        return N
    return _operand(rng, 521)


def _case(rng: random.Random) -> tuple[str, str]:
    op = rng.choice(["add", "sub", "mod", "div", "mulmod", "addmod", "sqrmod", "exptmod",
                     "inverse", "rshift", "legendre", "cmp", "bits"])
    a, b, m = _operand(rng, 1024), _operand(rng, 1024), _modulus(rng)
    if op in ("mulmod", "sqrmod", "exptmod", "inverse", "legendre", "addmod"):
        a, b = _operand(rng, 600), _operand(rng, 600)
    if op == "add":
        e = f"0 {_hex(a + b)}" if a + b < CAP else "-1"
    elif op == "sub":
        e = f"0 {_hex(a - b)}" if a >= b else "-1"
    elif op == "mod":
        e = f"0 {_hex(a % m)}" if m else "-1"
    elif op == "div":
        e = f"0 {_hex(a // m)}" if m else "-1"
    elif op == "mulmod":
        e = f"0 {_hex(a * b % m)}" if m else "-1"
    elif op == "addmod":
        e = f"0 {_hex((a + b) % m)}" if m and a + b < CAP else "-1"
    elif op == "sqrmod":
        e = f"0 {_hex(a * a % m)}" if m else "-1"
    elif op == "exptmod":
        b = _operand(rng, 300)
        # As mbedtls_mpi_exp_mod(): an odd modulus above 1 only.
        e = f"0 {_hex(pow(a, b, m))}" if m % 2 == 1 and m != 1 else "-1"
    elif op == "inverse":
        if rng.random() < 0.5:
            m = rng.choice([P, N])
        inv = None
        if m % 2 == 1 and m != 1:
            try:
                inv = pow(a, -1, m)
            except ValueError:
                inv = None
        e = f"0 {_hex(inv)}" if inv is not None else "-1"
    elif op == "rshift":
        b = rng.randrange(0, 1100)
        e = f"0 {_hex(a >> b)}"
    elif op == "legendre":
        m = rng.choice([P, N])
        t = pow(a, (m - 1) // 2, m)
        e = str(1 if t == 1 else 0 if t == 0 else -1)
    elif op == "cmp":
        if rng.random() < 0.2:
            b = a
        e = str((a > b) - (a < b))
    else:
        e = str(a.bit_length())
    return f"{op} {_hex(a)} {_hex(b)} {_hex(m)}", e


@unittest.skipUnless(FUZZ.is_file(), "build/host/hostap-bignum-fuzz not built (needs the ESP-IDF 5.5 source)")
class HostapBignumTest(unittest.TestCase):
    def test_operations_match_python_integers(self):
        for seed in (1, 2, 3):
            rng = random.Random(seed)
            cases = [_case(rng) for _ in range(20000)]
            out = subprocess.run(
                [str(FUZZ)], input="\n".join(c for c, _ in cases) + "\n",
                capture_output=True, text=True, check=True,
            ).stdout.split("\n")
            self.assertGreaterEqual(len(out), len(cases))
            bad = [(c, e, o) for (c, e), o in zip(cases, out) if e.strip() != o.strip()]
            self.assertEqual(bad[:3], [], f"seed {seed}: {len(bad)} mismatches")


if __name__ == "__main__":
    unittest.main()
