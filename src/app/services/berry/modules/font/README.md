# Berry fonts

`USE_DISPLAY_FONTS` adds the global `font` class, which draws UTF-8 text into a
[`pixmat`](../pixmat/README.md). Fonts are built into the firmware: the
[Spleen](../../../../../third_party/spleen/README.md) bitmap fonts by Frederic Cambus
(BSD-2-Clause), restricted to Latin-1, so umlauts and other Western European letters work.

```berry
var panel = display()
var frame = panel.framebuffer()
var screen = pixmat(frame, panel.width(), panel.height(), 0)
var big = font("8x16")
big.text(screen, "Küche 21 °C", 0, 0, 1)
panel.refresh()
```

On panels without `framebuffer()`, draw into a small matrix and copy it with `panel.blit`.

| Font | Selected by | RAM while the object exists |
| --- | --- | --- |
| `6x12`, `8x16` | `USE_DISPLAY_FONTS` | 2,292 and 3,056 bytes |
| `5x8` | `USE_DISPLAY_FONT_5X8` | 1,528 bytes |
| `12x24`, `16x32` | `USE_DISPLAY_FONT_12X24`, `USE_DISPLAY_FONT_16X32` | 1,536 and 2,048 bytes |

Small fonts are decompressed whole on first use. The two large fonts are compressed in blocks of
16 glyphs and keep the last two blocks, enough for lowercase ASCII. Text that needs more blocks
still draws correctly, but decompresses them again.

| Method | Behavior |
| --- | --- |
| `font(name)` | Selects a font of this build, or raises `value_error`. |
| `width()`, `height()` | Returns the glyph cell size in pixels. |
| `baseline()` | Returns the rows from the top of a cell to the baseline. |
| `measure(text)` | Returns the width of the text in pixels, for alignment. |
| `text(pixmat, text, x, y, color[, background])` | Draws with clipping and returns the x after the text. Without `background`, clear glyph pixels leave the matrix unchanged. |

A malformed UTF-8 byte draws `?` and advances one byte. A character outside Latin-1 draws one `?`.
