# Spleen

Frederic Cambus's Spleen 2.1.0 bitmap fonts (BSD-2-Clause, `LICENSE`) from
<https://github.com/fcambus/spleen>, release `2.1.0`.

| Input | SHA-256 |
| --- | --- |
| `spleen-2.1.0.tar.gz` (`releases/download/2.1.0/spleen-2.1.0.tar.gz`) | `8b47c56f1a6eb858fbcf9e34530557404b02fbb3455e38e64fb84473fd0c372f` |

The `.bdf` files for 5x8, 6x12, 8x16, 12x24 and 16x32 are the archive's, unchanged. The 32x64
size, the other font formats and the rest of the archive are omitted.

## Consumers

| Consumer | Uses |
| --- | --- |
| The Berry [`font` class](../../app/services/berry/modules/font/README.md) with `USE_DISPLAY_FONTS` | The Latin-1 glyphs, U+0020–U+007E and U+00A0–U+00FF, compressed at build time by `tools/newmota/fonts.py` |
