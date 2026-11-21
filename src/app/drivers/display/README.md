# Berry display primitives

`USE_DISPLAY` installs a global Berry `display` class over the retained panel
loaded from `/display.ini`. Construct one handle with `display()` after startup.

```berry
var panel = display()
panel.fill_rect(0, 0, panel.width(), panel.height(), 0x101820)
panel.pixel(8, 8, 0xffffff)
panel.refresh()
```

The class provides `width()`, `height()`, `pixel(x, y, rgb)`,
`fill_rect(x, y, width, height, rgb)`, `refresh([partial])`, `power(on)`,
`invert(enabled)`, `rotation(value)` and `brightness(value)`. Colors are
24-bit `0xRRGGBB`. Rectangles clip to the panel; pixel writes outside its bounds
are ignored. Buffered panels retain writes until `refresh()`; EPD refresh work
runs cooperatively through the application loop.

With `USE_BERRY_PIXMAT`, `blit(source, x, y)` copies a whole [`pixmat`](../../services/berry/modules/pixmat/README.md)
to the panel, clipped, without per-pixel Berry calls:

```berry
var line = pixmat(128, 16, 0)  # one bit per pixel, 256 bytes
line.blit(bytes("708888F888888800"), 5, 8, 0, 0, 1)
panel.blit(line, 0, 0)
panel.refresh()
```

Set 1-bit pixels are white and clear ones black. Grayscale, RGB and RGBW sources are
converted to RGB565 (white in RGBW is ignored), and two-channel sources are taken as
RGB565 values. Monochrome panels threshold the result as they do for every write.

On a monochrome panel at rotation 0, `framebuffer()` returns bytes mapped onto the panel's
own frame memory, so a matrix can draw there without a second buffer or a copy. It returns
`nil` on color panels and on rotated panels, which use `blit`. From the first call on,
every `refresh()` sends the frame. The mapping stays in the unrotated layout if the panel
is rotated afterwards.

```berry
var frame = panel.framebuffer()
if frame != nil
  var screen = pixmat(frame, panel.width(), panel.height(), 0)
  screen.set(10, 10, 1)
  panel.refresh()
end
```

The complete panel configuration is loaded from `/display.ini`. E-paper uses
the explicit `EPD` interface token in the `:H` header. When migrating a Tasmota
e-paper descriptor, change that header field from `SPI` to `EPD`; its pins,
command sequences and LUT data remain in the descriptor.

## Drawing helpers

`USE_DISPLAY_DRAW` adds the global `draw` class. Its methods clip to the matrix and take the
colors `pixmat.set` accepts.

| Method | Behavior |
| --- | --- |
| `draw.line(pm, x0, y0, x1, y1, color)` | Draws a line including both ends. |
| `draw.rect(pm, x, y, width, height, color)` | Draws an outline. |
| `draw.fill_rect(pm, x, y, width, height, color)` | Fills a rectangle. |
| `draw.circle(pm, cx, cy, radius, color)` | Draws an outline. |
| `draw.text(pm, font, text, x, y, width, color[, background[, align[, padding]]])` | Draws text in a field one font height tall, aligned with `draw.LEFT`, `draw.CENTER` or `draw.RIGHT` inside `padding`. A background clears the field first. Text wider than the field is not cut. |

The [clock](../../../../examples/display/display_clock.be) and
[sensor](../../../../examples/display/display_sensors.be) examples replace Tasmota's `DisplayMode`
pages. Both work with `framebuffer()` and with `blit`.

[`berry/drivers/display/display_text.be`](../../../../berry/drivers/display/display_text.be) adds Tasmota's `DisplayText`
command for existing scripts and automations: `load("display_text.be")  DisplayText().add_command()`
in `autoexec.be`, or only `DisplayText().add_command()` when a build lists it under
[`solidify`](../../../../user_config/README.md#built-in-berry-drivers). It supports the
escapes `z x y l c C B p s f h v L r R k K t T d D a o O` and `~HH` in text, and answers
`Unknown Escape` to `P S G g w b F m i I u U`. Since glyphs are not scaled, `f` and `s` select the
closest embedded font.

Seven-segment and LED matrix chips are not panels and have no native driver.
[`berry/drivers/segment_display`](../../../../berry/drivers/segment_display) holds Berry drivers for the
TM1637, TM1640 and MAX7219 over free GPIOs and the TM1650 on I2C, with text, numbers, raw segments,
scrolling and a clock. `autoexec.be` loads `segment_display.be`, the driver and then the script
that uses it, each with `load()`. The GPIOs must stay unassigned in the device configuration,
because Berry cannot drive assigned pins.

## High-level UI

LVGL is the high-level UI consumer when selected. These primitives provide a
small direct drawing surface; `USE_DISPLAY_FONTS` adds text through the Berry
[`font` class](../../services/berry/modules/font/README.md). Optional
`USE_UNIVERSAL_TOUCH` supplies pointer samples.

The same panel exposes `Display`, `DisplayDimmer`, `DisplayRotate`, `DisplayInvert`,
`DisplayPower` and `DisplayRefreshNow` through the shared
[command interface](../../core/command/README.md#display-controls). Runtime controls are
retained until restart. Panel-specific unsupported controls report an error.

Rotation updates panel dimensions immediately. LVGL adopts the new dimensions at the start
of its next application-loop poll so resolution-change callbacks run inside the Berry
execution deadline. After rotating from Berry, allow that poll before explicitly forcing
an LVGL refresh in the same script.

## Transport support

All platform families accept SPI monochrome and RGB565 panels, explicit SPI EPD,
and monochrome I2C panels. A descriptor must select an implemented transport and
available board pins; unsupported bus selectors, routes, framing and controllers
are rejected before hardware configuration. EPD mode 1, five-table mode 2 and
mode 3 share the same cooperative engine.

| Family | Native SPI selectors | Other display transports |
| --- | --- | --- |
| ESP-IDF | 1/2 where the chip supplies them; 8-bit or embedded 9-bit D/C | Software SPI 3/4, I2C, EPD; native I80, RGB and DSI on capable chips |
| Beken BK7231N/BK7238 | Unavailable in the current adapter | Software SPI 3/4, I2C, EPD, GPIO I80 |
| Bouffalo BL602, BL616, BL618DG, BL702 | 1 = SPI0; separate D/C required | Software SPI 3/4, I2C, EPD, GPIO I80 |
| AmebaD | 1 = SSI0, 2 = SSI1; 8-bit or embedded 9-bit D/C | Software SPI 3/4, I2C, EPD, GPIO I80 |
| Pico SDK | 1 = SPI0, 2 = SPI1; 8-bit or embedded 9-bit D/C | Software SPI 3/4, I2C, EPD, GPIO I80 |

SPI selectors 3 and 4 use the native GPIO boundary with arbitrary available output
pads. Selector 4 adds a microsecond to each clock half-cycle; the descriptor MHz
field is not a guaranteed software SPI rate. Omit D/C (`-1`) for embedded 9-bit
command/data framing. Native SPI uses mode 3 with a manually controlled descriptor
CS. Chip select may be omitted for a permanently selected panel.

On Bouffalo, AmebaD and Pico the native controller comes from the platform's shared SPI
controller owner (on ESP-IDF from its SPI host owner). An SD card whose `SPI CLK`, `MOSI`
and `MISO` match the descriptor's clock, MOSI and MISO pins shares that controller with its
own CS; declare MISO in the descriptor when the bus is shared.

Bouffalo SPI0 signals repeat every four pads, in a chip-specific order (GPIO number
modulo 4):

| Chip | MOSI | Clock | MISO |
| --- | --- | --- | --- |
| BL602 (unswapped) | 0 | 1 | 3 |
| BL702 | 0 | 2 | 3 |
| BL616, BL618DG | 3 | 1 | 2 |

The BL618DG order follows the SDK's bl618dgdk example and is unconfirmed on hardware.
Native embedded 9-bit D/C and selector 2 are rejected; use software SPI when needed.
The rate ceiling is 40 MHz. On BL602 the SPI0 resource is exclusive with the native LED
strip transport.

AmebaD SSI0 routes use MOSI/clock PA16/PA18 or PB18/PB20; optional MISO is PA17
or PB19 from the same route. SSI1 uses PA12/PA14 or PB4/PB6, with optional MISO
PA13 or PB5. The current BW16 exposes only the PA12–PA15 SSI1 route, so use
selector 2 or software SPI there. Native clock ceilings are 50 MHz for SSI0 and
25 MHz for SSI1. The divider can reduce the actual clock.

Pico native routes require the SDK's primary SPI pin function: SCK has GPIO number
modulo 4 equal to 2, MOSI equal to 3, and optional MISO equal to 0; GPIO bit 3
selects SPI0 (0) or SPI1 (1). Board exposure and reserved-pad checks still apply.
The requested rate cannot exceed half the peripheral clock and can be quantized.

Beken's pinned BDK master path changes additional pin muxes and waits indefinitely
for an interrupt semaphore. It is not used by this adapter; native selectors 1/2
are rejected. Software SPI supplies complete commands, pixels and EPD transfers.

Non-ESP I80 is an 8- or 16-bit write-only GPIO bus. CS and RD idle HIGH; WR pulses
LOW, D/C selects command or data, and every write has at least one microsecond
of setup, LOW pulse and hold. Commands and parameters occupy the low data lane;
16-bit pixel writes drive both lanes. The descriptor MHz field is not a sustained
GPIO write rate. Every data/control pad is reserved by the display owner before
initialization. Actual feasibility depends on available pads: for example CB2S
has insufficient exposed pads for I80. Continuous RGB and DSI are unavailable on
the current non-ESP environments. BW16 lacks the complete fixed LCDC RGB565 pad
route; the other current non-ESP chips have no RGB/DSI display controller.

I2C uses the same owned software bus as sensors and Berry Wire, with external
pull-ups. Explicit descriptor pins require an unused logical bus; `-1,-1` borrows
an already configured bus. ESP reset-reason conditions (`69`/`6A`) in EPD command
sequences are rejected on other families because their numeric reset reasons
cannot be interpreted portably.

EPD initialization, BUSY waits and refresh delays advance from the application
loop. Initial power/dim commands run after reset and initialization; power changes
during EPD initialization or an active refresh report busy. Power on/off requires
a command for that direction, a descriptor dim command, or a backlight output.
Panels without such a path report unsupported. A dim command powers off at zero
and restores the retained brightness on power on. RGB uses its backlight output
for power and brightness. None of these transports has display hardware qualification.

## Four-line QSPI

Select `features.USE_DISPLAY_QSPI: true` in personal environment YAML. This selects
`USE_DISPLAY`; selecting `USE_DISPLAY` or `USE_LVGL` alone leaves QSPI disabled.
The paired Safeboot keeps its own feature selection. QSPI is available on ESP-IDF,
Bouffalo BL616/BL618DG and Pico RP2040/RP2350. BL602, Beken and AmebaD reject the
feature at build selection.

The header matches Tasmota UDisplay at revision
`fb629eddb5047764781851ee059366328814a552`:

```text
:H,name,width,height,16,QSPI,bus,cs,clk,d0,d1,d2,d3,backlight,reset,mhz[,lines]
```

Omit `lines` or use `4`. One-line pixels are rejected. Supply explicit physical
pins for CS, clock and all four data outputs; the owner reserves each pin before
initialization and rejects duplicates or unavailable outputs. Backlight/reset may
be `-1` or use `*` to resolve the configured display function. Bus wildcards are
unsupported. QSPI is write-only RGB565 with 16-bit address windows (`:A`), explicit
X/Y/RAMWR commands and ordinary initialization, rotation, offsets and control
sections. `:P,18`, monochrome and all-command framing are rejected. Touch sections require the independent `USE_UNIVERSAL_TOUCH` opt-in.

| Target | Bus and route |
| --- | --- |
| ESP-IDF | Native SPI selector 1/2 where the chip provides it; available output pads through the GPIO matrix |
| BL616 | Selector 1, DBI QSPI: clock GPIO10, CS GPIO11, D0–D3 GPIO12–15 |
| BL618DG | Selector 1, provisional DBI route: clock GPIO0, CS GPIO1, D0–D3 GPIO2–5. This order is inferred from the SDK GPIO0–5 QSPI group; per-signal mux confirmation remains open |
| RP2040/RP2350 | Selector 1, one dynamically claimed PIO state machine and DMA channel; D0–D3 must be consecutive increasing GPIOs, all transport pins below GPIO32 |

QSPI uses mode 0. Commands and parameters use one lane with transport prefix
`02` and address bytes `00,command,00`; pixel frames use prefix `32`, the same
single-lane address and four-lane RGB565 bytes, most significant byte first.
The first pixel frame uses the descriptor RAMWR command; subsequent frames use
DCS RAMWRC (`3C`). Panels must accept that continuation across CS frames. Transfers
return after the wire drains, so Berry and LVGL can reuse their source buffers.
DBI uses at most 256 data bytes per frame and bounded FIFO polling; Pico uses
PIO/DMA with a bounded terminal-shifter drain. Rates may be reduced by native
clock dividers. These transports are compile-tested only; panel wiring, actual
clocks, continuation behavior and hardware timing require board qualification.

## Universal I2C touch

`features.USE_UNIVERSAL_TOUCH: true` selects the portable universal touch runtime
and `USE_DISPLAY`. It defaults off, including with `USE_DISPLAY`, `USE_LVGL` or
`USE_DISPLAY_QSPI` alone. Without it, touch sections remain ignored metadata.
Panels without universal touch sections work normally with this feature enabled.

The supported descriptor language follows Tasmota UDisplay at revision
`fb629eddb5047764781851ee059366328814a552`. Add these sections to `/display.ini`:

```text
:UTI,FT6236,I1,38,-1,-1
RD A8
CP 11
RTF
RD A3
CP 36
RTF
RT
:UTT
RDM 00 16
MV 2 1
RT
:UTX
MV 3 2
RT
:UTY
MV 5 2
RT
```

`:UTI,name,I1|I2,address,reset,irq` chooses configured shared I2C bus 1 or 2 and
a hexadecimal seven-bit address. Configure its SCL/SDA GPIO functions first;
external pull-ups are required. The driver leases the address against other
participating I2C drivers. Reset and IRQ accept explicit physical pins, `-1`, or
`*` resolved through `TS_RST` and `TS_IRQ`. Pins are reserved with the display
before initialization. Reset uses HIGH 10 ms, LOW 10 ms, then HIGH 50 ms. IRQ is
reserved as an input; sampling uses cooperative polling, without an interrupt
handler or an IRQ-level gate that could miss a release.

All four programs must be present and nonempty; initialization must return a
nonzero identification result. Each program has a strict 63-byte compiled limit
in a 64-byte buffer. Overflow, unknown instructions, invalid numeric operands and
out-of-range buffer indices reject the descriptor before hardware changes.

| Instructions | Operands and behavior |
| --- | --- |
| `RD`, `RDW`, `RDL` | Read one byte using an 8-, 16- or 32-bit hexadecimal register address |
| `RDM`, `RDWM`, `RDLM` | Same address widths, then a decimal count of 1–16 bytes |
| `WR`, `WRW` | Write one hexadecimal byte to an 8- or 16-bit hexadecimal register |
| `CP`, `CPR`, `CPM` | Compare buffer byte 0, compare the result, or compare byte 0 against up to eight alternatives; operands and CPM count are hexadecimal |
| `MV`, `MVB` | Decimal buffer index and byte-order mode (1=byte, 2=big endian, 3=little endian), retaining MV's 12-bit mask; MVB takes high/low selector 0/1 followed by a decimal index |
| `AND`, `SCL`, `LIM` | Hexadecimal 16-bit mask; signed decimal offset plus finite float multiplier; decimal upper limit. Scaled results saturate to signed 16-bit range |
| `RT`, `RTF`, `RTT`, `DBG` | Return result, stop on false, return zero on positive result, or log with a decimal identifier |

Register bytes are transmitted most significant byte first, including unsigned
32-bit addresses above `7FFFFFFF`. Reads use one combined repeated-start I2C
transaction and require every requested byte. Any NACK, short/incomplete read,
write failure or timeout clears the 16-byte VM buffer and releases the pointer;
no later instruction acts on stale data. Samples retry on later polls.

The application samples at most once per 20 ms. Detection, X and Y programs share
the current sample buffer. LVGL receives a pointer input device when touch is
initialized; its callback reads only the cached sample. Berry `display().touch()`
returns logical `[x,y]` while pressed and `nil` on release or transfer failure.

The optional fourth hexadecimal field of `:0`–`:3` selects touch orientation,
with defaults 0,1,2,3. Bit `80` swaps axes before optional `:M,xmin,xmax,ymin,ymax`
calibration; low-nibble modes 0–5 select identity, quarter turns and axis mirrors.
Coordinates use zero-based `extent - 1` edges and clamp inside the logical panel,
correcting upstream's inclusive out-of-range edge. Rotation releases the cached
press until a new sample is acquired.

SPI touch, XPT2046, direct resistive `GSRT`, and Tasmota's older `:TI`/`:TS` controller
formats are not implemented. Universal SPI/resistive headers or unsupported
instructions reject explicitly when this feature is selected. This integration
is native compile-tested; controller identification, calibration, physical touch,
reset timing and LVGL interaction still require hardware qualification.
