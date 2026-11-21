"""Matter setup codes of a build that fixes them, printed as a terminal QR code."""

from __future__ import annotations

import re
from functools import cache

from newmota.model import ROOT, BuildContext, BuildError

DEVICE_SOURCE = ROOT / "src/app/services/matter/berry/embedded/matter_device.be"
SIZE = 21  # Version 1: the 22-character payload fits error correction L in alphanumeric mode.
ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"
BASE38 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-."
GENERATOR = (127, 122, 154, 164, 11, 68, 117)
FORMAT = 0x77C4  # Error correction L, mask 0
VERHOEFF_D = "0123456789123406789523401789563401289567401239567859876043216598710432765982104387659321049876543210"
VERHOEFF_P = "01234567891576283094580379614289160435279453126870428657390127938064157046913258"


@cache
def device_ids() -> tuple[int, int]:
    """Read the vendor and product ID from the Berry device class, their single source."""

    source = DEVICE_SOURCE.read_text(encoding="utf-8")
    ids = [
        re.search(rf"static var {name} = (0x[0-9A-Fa-f]+)", source)
        for name in ("VENDOR_ID", "PRODUCT_ID")
    ]
    if not all(ids):
        raise BuildError(
            f"{DEVICE_SOURCE.relative_to(ROOT)} no longer declares VENDOR_ID and PRODUCT_ID"
        )
    return int(ids[0].group(1), 16), int(ids[1].group(1), 16)


def setup_payload(passcode: int, discriminator: int, ble: bool) -> str:
    """Return the QR payload text: version 0, standard flow, on-network discovery and BLE if built."""

    vendor, product = device_ids()
    discovery = 0x06 if ble else 0x04
    value = (
        (vendor << 3)
        | (product << 19)
        | (discovery << 37)
        | (discriminator << 45)
        | (passcode << 57)
    )
    raw = value.to_bytes(11, "little")
    text = ""
    for index in range(0, len(raw), 3):
        chunk = raw[index : index + 3]
        number = int.from_bytes(chunk, "little")
        for _ in range((2, 4, 5)[len(chunk) - 1]):
            number, digit = divmod(number, 38)
            text += BASE38[digit]
    return "MT:" + text


def pairing_code(passcode: int, discriminator: int) -> str:
    """Return the 11-digit manual pairing code with its Verhoeff check digit."""

    digits = (
        f"{discriminator >> 10}{((discriminator & 0x300) << 6) | (passcode & 0x3FFF):05d}"
        f"{passcode >> 14:04d}"
    )
    check = 0
    for index, digit in enumerate(reversed(digits)):
        check = int(VERHOEFF_D[check * 10 + int(VERHOEFF_P[((index + 1) % 8) * 10 + int(digit)])])
    return digits + "0432156789"[check]


def _gf(x: int, y: int) -> int:
    z = 0
    for bit in range(7, -1, -1):
        z = ((z << 1) ^ ((z >> 7) * 0x11D)) ^ (((y >> bit) & 1) * x)
    return z & 0xFF


def _codewords(text: str) -> list[int]:
    bits: list[int] = []

    def put(value: int, count: int) -> None:
        bits.extend((value >> bit) & 1 for bit in range(count - 1, -1, -1))

    put(2, 4)
    put(len(text), 9)
    for index in range(0, len(text) - 1, 2):
        put(ALPHABET.index(text[index]) * 45 + ALPHABET.index(text[index + 1]), 11)
    if len(text) % 2:
        put(ALPHABET.index(text[-1]), 6)
    put(0, min(4, 152 - len(bits)))  # The terminator shortens when the data fills the code
    bits += [0] * (-len(bits) % 8)
    data = [int("".join(map(str, bits[index : index + 8])), 2) for index in range(0, len(bits), 8)]
    pad = 0xEC
    while len(data) < 19:
        data.append(pad)
        pad ^= 0xFD
    ec = [0] * len(GENERATOR)
    for value in data:
        factor = value ^ ec[0]
        ec = [e ^ _gf(g, factor) for e, g in zip([*ec[1:], 0], GENERATOR, strict=True)]
    return data + ec


def qr_rows(text: str) -> list[int]:
    """Encode up to 25 QR alphanumeric characters as rows whose top bit is the leftmost module."""

    if len(text) > 25 or any(character not in ALPHABET for character in text):
        raise ValueError(f"not a version 1-L alphanumeric QR text: {text!r}")
    n = SIZE
    dark = [[False] * n for _ in range(n)]
    fixed = [[False] * n for _ in range(n)]

    def put(x: int, y: int, on: bool | int) -> None:
        dark[y][x] = bool(on)
        fixed[y][x] = True

    for index in range(n):
        put(6, index, index % 2 == 0)
        put(index, 6, index % 2 == 0)
    for cx, cy in ((3, 3), (n - 4, 3), (3, n - 4)):
        for dy in range(-4, 5):
            for dx in range(-4, 5):
                x, y = cx + dx, cy + dy
                if 0 <= x < n and 0 <= y < n:
                    put(x, y, max(abs(dx), abs(dy)) not in (2, 4))
    for index in range(15):
        on = (FORMAT >> index) & 1
        put(
            *(
                (8, index)
                if index < 6
                else (8, index + 1)
                if index < 8
                else (7, 8)
                if index < 9
                else (14 - index, 8)
            ),
            on,
        )
        put(*((n - 1 - index, 8) if index < 8 else (8, n - 15 + index)), on)
    put(8, n - 8, True)
    words = _codewords(text)
    index = 0
    right = n - 1
    while right >= 1:
        if right == 6:
            right = 5
        for vertical in range(n):
            for column in range(2):
                x = right - column
                y = vertical if (right + 1) & 2 else n - 1 - vertical
                if fixed[y][x]:
                    continue
                bit = (words[index >> 3] >> (7 - (index & 7))) & 1
                index += 1
                dark[y][x] = not bit if (x + y) % 2 == 0 else bool(bit)
        right -= 2
    return [sum(1 << (n - 1 - x) for x in range(n) if dark[y][x]) for y in range(n)]


def terminal_lines(rows: list[int], quiet: int = 4) -> list[str]:
    """Draw two modules per character, black on white in any terminal theme so a camera reads it."""

    n = SIZE + 2 * quiet

    def dark(x: int, y: int) -> bool:
        x, y = x - quiet, y - quiet
        return 0 <= x < SIZE and 0 <= y < SIZE and bool((rows[y] >> (SIZE - 1 - x)) & 1)

    lines = []
    for y in range(0, n, 2):
        # Half blocks draw dark modules in the black text colour over the white background.
        cells = "".join(" ▄▀█"[dark(x, y) * 2 + dark(x, y + 1)] for x in range(n))
        lines.append(f"\x1b[30;47m{cells}\x1b[0m")
    return lines


def print_setup_codes(context: BuildContext) -> None:
    """Print the codes when the build fixes them; random codes exist only on the device."""

    settings = context.environment.features.get("USE_MATTER")
    if not isinstance(settings, dict) or not context.features.enabled("USE_MATTER"):
        return
    passcode, discriminator = settings["passcode"], settings["discriminator"]
    payload = setup_payload(passcode, discriminator, context.features.enabled("USE_MATTER_BLE"))
    print("Matter setup code fixed by this build:")
    print("\n".join(terminal_lines(qr_rows(payload))))
    print(f"QR code {payload}, pairing code {pairing_code(passcode, discriminator)}", flush=True)
