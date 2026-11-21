"""Generate or check the P-256 generator comb table of the ESP MPI provider.

Entry i - 1 of COMB is the affine point sum over teeth t of bit t of i times 2^(64 t) G,
for i in 1..15, as little-endian 32-bit limbs. Exact integer arithmetic, no dependencies.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROVIDER = ROOT / "src/platform/esp-idf/crypto_p256_mpi.c"

P = 2**256 - 2**224 + 2**192 + 2**96 - 1
A = P - 3
GX = 0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296
GY = 0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5

Point = tuple[int, int] | None


def add(p1: Point, p2: Point) -> Point:
    """Affine addition, including doubling and infinity (None)."""
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2:
        if (y1 + y2) % P == 0:
            return None
        slope = (3 * x1 * x1 + A) * pow(2 * y1, -1, P) % P
    else:
        slope = (y2 - y1) * pow(x2 - x1, -1, P) % P
    x3 = (slope * slope - x1 - x2) % P
    return x3, (slope * (x1 - x3) - y1) % P


def mul(k: int, point: Point) -> Point:
    result: Point = None
    while k:
        if k & 1:
            result = add(result, point)
        point = add(point, point)
        k >>= 1
    return result


def table() -> list[tuple[int, int]]:
    teeth = [mul(2 ** (64 * t), (GX, GY)) for t in range(4)]
    entries = []
    for i in range(1, 16):
        point: Point = None
        for t in range(4):
            if i >> t & 1:
                point = add(point, teeth[t])
        assert point is not None
        entries.append(point)
    return entries


def limbs(value: int) -> list[int]:
    return [(value >> (32 * i)) & 0xFFFFFFFF for i in range(8)]


def render() -> str:
    def words(value: int) -> str:
        return ", ".join(f"0x{limb:08X}" for limb in limbs(value))

    lines = ["static const struct {", "    fe x, y;", "} COMB[15] = {"]
    for x, y in table():
        lines.append(f"    {{{{{words(x)}}},")
        lines.append(f"        {{{words(y)}}}}},")
    lines.append("};")
    return "\n".join(lines) + "\n"


def check(path: Path) -> bool:
    """Compare the limbs of the COMB block in the provider with freshly computed ones."""
    match = re.search(r"COMB\[15\] = \{(.*?)\n\};", path.read_text(), re.DOTALL)
    if not match:
        print(f"{path}: no COMB table found", file=sys.stderr)
        return False
    found = [int(word, 16) for word in re.findall(r"0x([0-9A-Fa-f]{8})", match.group(1))]
    expected = [limb for x, y in table() for limb in limbs(x) + limbs(y)]
    if found != expected:
        print(f"{path}: COMB table differs from the computed one", file=sys.stderr)
        return False
    print(f"{path}: COMB table matches")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="verify the table in the provider")
    args = parser.parse_args()
    if args.check:
        return 0 if check(PROVIDER) else 1
    sys.stdout.write(render())
    return 0


if __name__ == "__main__":
    sys.exit(main())
