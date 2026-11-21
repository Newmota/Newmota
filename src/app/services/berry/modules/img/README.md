# Berry images

The global `img` class is Tasmota's: one image in a native buffer, outside the Berry heap.
It is available with Berry and the optional `USE_BERRY_IMAGE` on ESP-IDF boards with PSRAM.
JPEG is decoded by the vendored [JPEGDEC](../../../../../third_party/jpegdec/README.md), with
its SIMD colour conversion on the ESP32-S3 and P4.

```berry
import cam
var frame = img()
cam.get_image(frame, img.GRAYSCALE)   # decode the newest camera frame into the buffer
var roi = frame.get_buffer(dsc)       # a 32x32 region for a model, see below
```

The buffer only grows. A frame of the same or a smaller size reuses it, so a loop that
decodes into the same `img` allocates nothing and `info()['buf_addr']` stays the same. A
larger frame moves the buffer; a pointer taken before then is stale, and using it is the
script's responsibility, as in C.

## Formats

| Constant | Value | Bytes per pixel | Layout |
| --- | --- | --- | --- |
| `img.RGB565` | 0 | 2 | Big endian |
| `img.RGB565LE` | 1 | 2 | Little endian, the order of ESP display framebuffers and LVGL |
| `img.GRAYSCALE` | 3 | 1 | Luma |
| `img.JPEG` | 4 | — | The JPEG file as it is |
| `img.RGB888` | 5 | 3 | R, G, B |

The values are Tasmota's, which follow esp32-camera's `pixformat_t`. `RGB888` is R, G, B in
every path; Tasmota's own conversions disagree on that order. Grayscale from RGB uses the
BT.601 luma, where Tasmota averages the channels.

## Methods

| Method | Behaviour |
| --- | --- |
| `img()` | An empty image; the buffer comes with the first data |
| `from_jpg(jpeg [, format])` | Without a format, or with `img.JPEG`, stores the JPEG and reads its size from the frame header. With a raw format, decodes it; only baseline JPEG decodes |
| `from_buffer(data, width, height, format)` | Copies raw pixels; the length must be `width * height` times the bytes per pixel. With `img.JPEG`, copies the bytes with the given size |
| `get_buffer([descriptor])` | Copies the whole image into `bytes`, raising `value_error` above the 32 KiB `bytes` limit, or samples a region of interest (below) |
| `convert_to(format)` | Converts in place. JPEG decodes into the raw format; raw formats convert pixel by pixel. The same format does nothing; `img.JPEG` raises, as there is no encoder |
| `info()` | `buf_addr`, `size`, `width`, `height`, `bpp` (0 for JPEG) and `format` |

A decode that fails while reading the header leaves the image as it was. One that fails part
way, on damaged data or the Berry execution deadline, leaves the new size and format with
partial pixels, unless the frame needed a new buffer. Data inside the buffer itself, such as
`bytes(info()['buf_addr'], size)`, moves in place.

## Regions of interest

The descriptor is Tasmota's 24 bytes, as written by the
[ROI editor](https://tasmota.github.io/docs/ROI-editor/): floats `scale_x`, `shear_x`,
`shear_y`, `scale_y`, then `uint16` `translation_x`, `translation_y`, `width`, `height`, all
little endian.

```berry
def roi(m)
  var d = bytes(-24)
  d.setfloat(0, m['scaleX'])  d.setfloat(4, m['shearX'])
  d.setfloat(8, m['shearY'])  d.setfloat(12, m['scaleY'])
  d.seti(16, m['transX'], 2)  d.seti(18, m['transY'], 2)
  d.seti(20, m['width'], 2)   d.seti(22, m['height'], 2)
  return d
end
```

Output pixel (x, y) takes source pixel
(`x * scale_x + y * shear_y + translation_x`, `x * shear_x + y * scale_y + translation_y`),
truncated, with the field names as Tasmota has them. Samples outside the image are 0, where
Tasmota reads past the buffer. The region must fit the 32 KiB `bytes` limit and the image
must be raw.

## Camera

With `USE_WEBCAM` or `USE_CSI_WEBCAM`, `cam.get_image(img [, format])` stores the newest JPEG
frame in the `img`, or decodes it there, and returns `true`, or `false` when no frame arrives
within 2 s. See the [camera](../../../../drivers/camera/README.md#berry).
