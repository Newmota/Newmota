# WS2812 RGB and RGBW strips

Optional `USE_WS2812` builds select a portable strip owner and native waveform
adapter. It defaults off. Enable it in a personal environment, for example:

```yaml
name: strip-pico
inherit: pico2-w
features:
  USE_WS2812: true
```

Build with `make build ENV=strip-pico`. Assign the `WS2812` GPIO function to
an eligible output pad to create the configured Light. It shares power, color,
brightness, gamma, fade, palette, wakeup, MQTT, browser, and Berry `light.get()` /
`light.set()` state with the Light service. A configured strip takes precedence
over PWM Light; configured PWM outputs then remain available to raw PWM commands.
SetOption37 bit 128 selects separate RGB/white endpoints for RGBW after restart.
SetOption68 selects independent component endpoints after restart and takes
precedence over split grouping. Endpoint power gates steady, manual, and
animated strip output.
RGB strips have no physical color-temperature channels.

The configured Light accepts `Pixels 1..512[,reverse,height,alternate]`,
`PixelType`, `Led<n>`, `Rotation`, `Width` / `Width1..4`, and `StepPixels`.
`PixelType` selects GRB (0/default or 1), RGB (2), RBG (3), BRG (4), BGR (5), or
GBR (6); add 8 for RGBW and 16 for white first. Changing RGB/RGBW channel count
requires restart. Buffers follow the configured pixel count, on ESP-IDF RMT about
2.4 KB of internal RAM plus 148 bytes per pixel. Raising `Pixels` above the highest count since the start
restarts the strip and reallocates, and the command fails without changing anything if the memory
is not there. Other changes keep the buffers, including during animation. `Led<n>` uses one-based pixel indices and accepts
successive blank-separated colors. `Pixels` count changes clear retained manual
pixels. Pixel buffers and effect samples are not persisted.

Schemes 0–4 retain the common Light behavior. Strip schemes are clock (5),
incandescent (6), RGB (7), Christmas (8), Hanukkah (9), Kwanzaa (10), rainbow (11),
fire (12), stairs (13), and clear/manual (14). Width, Fade, Speed and StepPixels
control the pattern. The clock uses Color3–Color6 for seconds, minutes, hours,
and markers, and SetOption16 for direction; it stays black until time is valid.
SetOption17 selects decimal color responses. Clear/manual clears on entry and
retains subsequent pixel edits through power off/on. Network schemes and
L1MusicSync are deferred.

With Berry enabled, the global class is
`led_strip(pin, count[, order, reverse])`. Pins are physical GPIO numbers, counts
are 1–512, order defaults to `"GRB"`, and reverse defaults to `false`. Accepted
orders are `"RGB"`, `"GRB"`, `"RBG"`, `"GBR"`, `"BRG"`, and `"BGR"`.
Append `W` to a color order for RGBW, or prepend it for white-first wire order
(for example `"GRBW"` or `"WGRB"`). Colors are integers in `0xWWRRGGBB` order
regardless of wire order; existing `0xRRGGBB` colors have white zero.

```berry
strip = led_strip(4, 16)
strip.fill(0x001020)
strip.set(0, 0xFF0000)  # zero-based pixel index
strip.show()
```

`set(index, rgb)` and `fill(rgb)` edit retained pixels; `show()` starts native
transmission and returns immediately. `busy()` includes the final bit and reset
interval. While busy, edits, another show, and `close()` raise an error; retry
after a timer or a later application iteration. Standalone strips have no queued frame or implicit refresh. Initial pixels are black; construction does not transmit.
`close()` is idempotent after success. Closed methods return nil. Collection and
VM teardown defer cleanup until the native transfer drains. Uncertain hardware
failures raise a hardware error from `busy()`, edits, `show()`, and `close()`, and
retain buffers, peripheral ownership, and pad reservations until restart.

An available, unassigned pad can be opened directly. Opening the configured `WS2812` pad borrows its existing Light owner; count,
order and reverse must match that owner. Pixel edits select its manual mode and
update the same power state. Closing or collecting a borrowed object and VM
teardown leave the configured Light active. Light frames coalesce while hardware
is busy: the latest complete frame is sent after the current transfer drains.
Editable intent is separate from the immutable DMA buffer. Every unrelated
configured pad, inactive/invalid binding, console pad, and active script GPIO or
serial pad remains reserved. Only one strip may own a physical pad. The portable
owner has four handle slots; native resource availability may impose a smaller
limit. Generation handles reject access after a close and reopen.

| Platform | Native waveform and GPIO routes |
| --- | --- |
| ESP-IDF | RMT on chips that expose RMT; DMA SPI2 on chips such as ESP32-C2 with no RMT. One strip; any available output pad. An occupied peripheral is rejected. |
| Pico SDK | Claimed PIO state machine, program storage, and DMA channel; available output GPIOs below 32. Multiple strips require available PIO/DMA resources. |
| BL602 | Continuous SPI0 with DMA channel 0; unswapped MOSI GPIO0/4/8/12/16/20, subject to board exposure and reservations. One strip. |
| AmebaD | SSI/GDMA on SPI0 MOSI PA16/PB18 (GPIO16/50) or SPI1 MOSI PA12/PB4 (GPIO12/36), subject to board exposure. One strip per SSI controller. Only MOSI is muxed. |
| Beken | `USE_WS2812` is rejected by feature resolution pending a verified continuous waveform. The pinned BDK DMA drain calculation includes an inter-word interval that could exceed the selected WS2812B-V6 LOW limit; no board waveform measurement confirms otherwise. CB2S also lacks the exposed GPIO16 MOSI route. |

Waveforms target WS2812B-V6: 4-bit `1000`/`1100` symbols at approximately
3.2 MHz, with an adapter rate between 3.0 and 3.25 MHz where an integer native
divider is used. Leading and trailing zero bytes provide at least 300 µs LOW
reset. SPI adapters must keep the pixel frame continuous; BL602 explicitly
enables continuous master mode and clears the SDK's interframe delay. RMT and
PIO emit the same symbol timing. Buffers are bounded and retained until hardware
drains; no waveform uses application bit-banging.

Host checks validate encoding, bounds, pin ownership, stale handles, and Berry
lifecycle. Native compilation establishes SDK compatibility. Actual pulse timing,
DMA behavior under network load, electrical operation, and the selected strip
revision still require board qualification. The configured Light supports RGB and
RGBW wire formats. The standalone Berry constructor also accepts RGBW color
orders and keeps existing RGB integer calls.
