# Berry pixel matrices

The global `pixmat` class is available with Berry and `USE_BERRY_PIXMAT`. The build turns it on
with `USE_WS2812` or `USE_DISPLAY` (which `USE_LVGL` selects); setting `USE_BERRY_PIXMAT` explicitly
overrides that either way. The class works without the strip or display hardware.
It edits a retained framebuffer in native code. It does not transmit pixels or
claim hardware. Coordinates are zero based; serpentine matrices reverse odd rows
in storage while all drawing methods use logical coordinates.

```berry
frame = pixmat(16, 8, 3)
frame.clear()
# Eight rows of an MSB-first 5-pixel-wide A; unused low bits are ignored.
a = bytes("708888F888888800")
frame.blit(a, 5, 8, 2, 0, 0xFF8000)
```

Constructors:

- `pixmat(width,height,bpp[,serpentine])` allocates a zero-filled framebuffer.
- `pixmat(buffer,width,height,bpp[,serpentine])` wraps mutable Berry bytes without
  copying. The object retains the bytes; each operation resolves their current
  address and checks their length. Shrinking or closing the bytes makes operations
  fail until a sufficiently large valid buffer is restored.
- `pixmat(bitplane_bytes,bytes_per_line)` expands an MSB-first bitplane into a
  mutable grayscale matrix: width is `bytes_per_line*8`, height is the byte length
  divided by `bytes_per_line`, and pixels are 0 or 255.

Dimensions must be positive, `bpp` is bytes per pixel from 1 through 4 or 0 for one
bit per pixel, and `serpentine` is a boolean (default false). Allocations obey the
Berry VM bytes limit and available heap. Formats are 1-bit (0), grayscale (1), packed
two-channel (2), RGB `0xRRGGBB` (3), and RGBW `0xRRGGBBWW` (4). RGBW packing follows
Tasmota's pixmat API; `led_strip` color integers use `0xWWRRGGBB`.

The 1-bit format uses the layout of monochrome panels, so a 128x64 frame takes 1 KB and
a matrix can wrap `display.framebuffer()`. Each byte is a column of eight rows with the
top row in bit 0, and pages of `width` bytes follow each other. A nonzero color sets a
pixel and `get` returns 0 or 1. It cannot be serpentine. `clear(byte)` writes whole
storage bytes, so 255 sets every pixel.
Calling `init(...)` on an initialized matrix raises an error. Call `deinit()`
before initializing that instance again.

Methods:

| Method | Behavior |
| --- | --- |
| `buffer()` | Returns the retained Berry bytes object without copying, or nil after `deinit()`. It exposes physical storage order, including reversed odd rows for serpentine matrices. Changes to these bytes affect the matrix directly. |
| `width()`, `height()` | Returns the matrix size in pixels. |
| `get(x,y)` | Returns packed pixel integer; outside the matrix returns nil. Four-channel integers may be signed. |
| `set(x,y,color[,brightness])` | Writes a packed color; outside the matrix does nothing. |
| `set(x,y,h,s,v[,brightness])` | Writes HSV, with hue wrapped in degrees and saturation/value 0–255; white is zero for RGBW. One/two-channel matrices receive value in each channel. |
| `clear([byte])` | Fills every stored byte with 0–255, default 0. |
| `blit(source,dx,dy[,brightness[,tint]])` | Copies a matrix with clipping. Matching formats copy every pixel, including zero. Grayscale or 1-bit to a different format skips zero pixels and expands nonzero luminance into every destination channel before tint/brightness; a set 1-bit pixel counts as 255. Between 1-bit matrices, brightness 0 or tint 0 clears the copied pixels. Other conversions, including anything into 1-bit from another format, raise an error. |
| `blit(bits,width,height,dx,dy,color[,stride])` | Draws a packed MSB-first glyph with clipping; set bits write color and zero bits leave the destination unchanged. Default stride is `ceil(width/8)` bytes. Explicit stride must be at least this large. Only the bytes through the last used byte of the final row are required. No expanded glyph buffer is allocated. |
| `scroll(direction[,source])` | Moves by one pixel: 0 up, 1 left, 2 down, 3 right. Without source, wraps the outgoing edge. With a separate source, takes its opposite edge; dimensions and bpp must match. |
| `deinit()` | Releases the retained buffer reference. Repeated calls are harmless; drawing raises an error until reinitialized. |

Brightness and tint multiply each channel on a 0–255 scale; 255 preserves the
channel exactly. Matrix self-blits preserve the source pixels across overlapping
moves. Overlapping wrapped views require identical dimensions, format, storage
start, and serpentine layout; other overlaps raise an error. Packed glyph bytes
must not overlap the destination framebuffer. Read-only bytes cannot be wrapped
as mutable framebuffers. Native drawing loops allocate no temporary frame or
per-pixel Berry objects.
