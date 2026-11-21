"""Compress the embedded Spleen fonts into a header for the Berry font class."""

from __future__ import annotations

import hashlib
import math
import shutil
import struct
import tempfile
from pathlib import Path

from newmota.host import copy_if_changed, cross_build_environment, run, write_if_changed
from newmota.model import ROOT, BuildError

SPLEEN = ROOT / "src/third_party/spleen"
TAMP = ROOT / "src/third_party/tamp"
COMPRESSOR = Path(__file__).with_name("font_compress.c")
HEADER = "newmota_fonts.h"
# The decompressor sources, staged as tamp/<name> for the one file that compiles them.
TAMP_FILES = ("common.c", "common.h", "decompressor.c", "decompressor.h")
GENERATED = (HEADER, *(f"tamp/{name}" for name in TAMP_FILES))

DEFAULT_FONTS = ("6x12", "8x16")
OPTIONAL_FONTS = {
    "USE_DISPLAY_FONT_5X8": "5x8",
    "USE_DISPLAY_FONT_12X24": "12x24",
    "USE_DISPLAY_FONT_16X32": "16x32",
}
# U+0020-U+007E and U+00A0-U+00FF, the glyph order font_module.cpp indexes.
CODE_POINTS = (*range(0x20, 0x7F), *range(0xA0, 0x100))
# Large fonts decompress in blocks, so only the glyphs being drawn take RAM.
BLOCK_GLYPHS = {"12x24": 16, "16x32": 16}


def selected_fonts(values: dict[str, bool]) -> tuple[str, ...]:
    """Return the fonts of this build, smallest first."""

    optional = (font for name, font in OPTIONAL_FONTS.items() if values[name])
    return tuple(sorted((*DEFAULT_FONTS, *optional), key=lambda font: int(font.split("x")[1])))


def _read_font(font: str) -> tuple[int, int, int, bytes]:
    """Return width, height, baseline and the row-aligned Latin-1 glyphs of one BDF file."""

    width, height = map(int, font.split("x"))
    stride = math.ceil(width / 8)
    glyph_bytes = stride * height
    baseline = None
    glyphs: dict[int, bytes] = {}
    code = -1
    rows: list[bytes] | None = None
    path = SPLEEN / f"spleen-{font}.bdf"
    for line in path.read_text(encoding="latin-1").splitlines():
        if line.startswith("FONT_ASCENT "):
            baseline = int(line.split()[1])
        elif line.startswith("ENCODING "):
            code = int(line.split()[1])
        elif line == "BITMAP":
            rows = []
        elif line == "ENDCHAR":
            glyphs[code] = b"".join(rows or ())
            rows = None
        elif rows is not None:
            rows.append(bytes.fromhex(line))
    if baseline is None:
        raise BuildError(f"{path.name} has no FONT_ASCENT")
    data = []
    for point in CODE_POINTS:
        glyph = glyphs.get(point)
        if glyph is None or len(glyph) != glyph_bytes:
            raise BuildError(f"{path.name} has no complete glyph for U+{point:04X}")
        data.append(glyph)
    return width, height, baseline, b"".join(data)


def _compile_compressor(directory: Path) -> Path:
    """Build the host compressor from the vendored tamp sources."""

    compiler = next(filter(None, map(shutil.which, ("cc", "clang", "gcc"))), None)
    if compiler is None:
        raise BuildError("USE_DISPLAY_FONTS needs a host C compiler (cc, clang, or gcc)")
    binary = directory / "font_compress"
    run(
        [
            compiler,
            "-std=c99",
            "-O2",
            "-w",
            "-DTAMP_LAZY_MATCHING=0",
            "-DTAMP_EXTENDED=1",
            "-DTAMP_STREAM=0",
            f"-I{TAMP}",
            str(COMPRESSOR),
            *(str(TAMP / name) for name in ("common.c", "compressor.c", "decompressor.c")),
            "-o",
            str(binary),
        ],
        environment=cross_build_environment(),
    )
    return binary


def _array(kind: str, name: str, values: tuple[int, ...] | bytes) -> str:
    """Format one constant array."""

    return f"static const std::{kind} {name}[] = {{{','.join(map(str, values))}}};"


def _render(fonts: tuple[str, ...], tool: Path, directory: Path) -> str:
    """Compress the fonts and return the C++ declarations."""

    arrays = []
    entries = []
    for font in fonts:
        width, height, baseline, glyphs = _read_font(font)
        glyph_bytes = math.ceil(width / 8) * height
        block_glyphs = BLOCK_GLYPHS.get(font, len(CODE_POINTS))
        raw = directory / f"{font}.raw"
        packed = directory / f"{font}.bin"
        raw.write_bytes(glyphs)
        run(
            [str(tool), str(raw), str(glyph_bytes), str(block_glyphs), str(packed)],
            environment=cross_build_environment(),
        )
        data = packed.read_bytes()
        blocks = math.ceil(len(CODE_POINTS) / block_glyphs)
        table = (blocks + 1) * 2
        offsets = struct.unpack(f"<{blocks + 1}H", data[:table])
        symbol = f"kFont{width}x{height}"
        arrays.append(_array("uint16_t", f"{symbol}Offsets", offsets))
        arrays.append(_array("uint8_t", f"{symbol}Stream", data[table:]))
        entries.append(
            f'    {{"{font}", {width}, {height}, {baseline}, {block_glyphs}, {blocks}, '
            f"{symbol}Offsets, {symbol}Stream}},"
        )
    return "\n".join((*arrays, "static const FontData kFonts[] = {", *entries, "};", ""))


def generate_fonts(values: dict[str, bool], directory: Path) -> None:
    """Write the font header and stage the decompressor beside the feature header."""

    fonts = selected_fonts(values)
    digest = hashlib.sha256(",".join(fonts).encode())
    inputs = [Path(__file__), COMPRESSOR, *(SPLEEN / f"spleen-{font}.bdf" for font in fonts)]
    inputs.extend(sorted(TAMP.glob("*.[ch]")))
    for path in inputs:
        digest.update(path.read_bytes())
    first_line = f"/* Generated by tools/newmota/fonts.py; do not edit. {digest.hexdigest()} */"

    for name in TAMP_FILES:
        (directory / "tamp").mkdir(parents=True, exist_ok=True)
        copy_if_changed(TAMP / name, directory / "tamp" / name)
    header = directory / HEADER
    if header.is_file():
        with header.open(encoding="utf-8") as existing:
            if existing.readline().rstrip("\n") == first_line:
                return
    with tempfile.TemporaryDirectory(prefix=".fonts-", dir=directory) as temporary:
        work = Path(temporary)
        body = _render(fonts, _compile_compressor(work), work)
    write_if_changed(header, f"{first_line}\n{body}")
