# LVGL and Berry

`USE_LVGL: true` selects LVGL 9.6.0 and its Berry `lv` binding in application
environments. It defaults off in every profile and requires Berry. It also
selects `USE_DISPLAY` on every platform, which loads the panel described by
`/display.ini` and attaches it as LVGL's default display.

When selected, the Berry service registers the native call mapper and imports `lv`.
Call `lv.start()` before creating widgets. Startup is lazy and idempotent: it initializes
LVGL, attaches the panel buffer, registers callbacks, and loads the custom Berry widgets
once. Repeated calls preserve the existing display and widgets.

`SetOption135` defaults to `0`: when a panel is already open, the Berry service starts
LVGL automatically and shows a Newmota text splash for five seconds. `SetOption135 1`
suppresses this automatic startup and splash on the next Berry initialization; LVGL's
core, callbacks and panel buffer then remain unallocated until `lv.start()`. An explicit
`lv.start()` also removes an active splash immediately. UDisplay's panel startup remains
independent of LVGL.

The cooperative application loop runs LVGL timers inside the Berry execution deadline.
LVGL uses the platform millisecond clock, libc allocation and the shared logger. Teardown
disables callback dispatch and releases LVGL before destroying the Berry VM.

The binding provides standard LVGL widgets, styles, events, timers, animations, constants
and ctypes wrappers. `lv.clock_icon(parent)` and `lv.wifi_arcs_icon(parent)` follow Tasmota's
status-icon layout. Their base classes `clock`, `signal_arcs` and `wifi_arcs` are also
available after startup. They refresh through LVGL timers, released when the widget is
deleted. The clock reads Newmota local time and remains unset until time is valid; the
Wi-Fi indicator reads current station status and RSSI without starting a scan.

Embedded font lookup follows Tasmota's exact family/size matching:

| Family | Sizes |
| --- | --- |
| `montserrat` | 10, 14, 20, 28 (Tasmota glyph and symbol set) |
| `seg7` | 8, 10, 12, 14, 16, 18, 20, 24, 28, 36, 48 (DSEG7) |
| `unscii` | 8, 16 |
| `icons` | 10, 12, 14, 16, 18, 20, 22, 24, 28 |

Use `lv.font_embedded("montserrat", 20)`, `lv.montserrat_font(20)` /
`lv.font_montserrat(20)`, or `lv.seg7_font(16)` / `lv.font_seg7(16)`.
Unsupported families or sizes raise `value_error`; sizes are never substituted. Icons
reuse the Tasmota Montserrat face at its available sizes and use separate icon faces
for the remaining sizes. The bundled LVGL default font remains standard Montserrat 14.

Logo images, HASPmota themes and Roboto Condensed, FreeType integration, and the
`lv_tasmota` module are omitted. No LittleFS driver is registered with LVGL, so
filesystem font/image loading is not connected.

Display adapters accept SPI, I2C, explicit `EPD` and I80 descriptors subject to
board pin availability. ESP additionally supports native RGB and DSI on capable
chips; see the [transport table](../../../../drivers/display/README.md#transport-support). LVGL uses an
RGB565 partial buffer. Flushes complete synchronously for direct panels; EPD
copies each tile into retained panel storage and advances refresh work in the
cooperative application loop. E-paper descriptors must use the `EPD` interface
token so their command, waveform and LUT sections are parsed as e-paper.

Optional `USE_UNIVERSAL_TOUCH` attaches a pointer input device for the descriptor's
universal I2C touch controller. It reads cached logical coordinates; touch I2C work
runs in the application display poll. Failed transfers release the pointer. See the
[touch format and limits](../../../../drivers/display/README.md#universal-i2c-touch).

When no `/display.ini` is present, automatic startup is skipped. An explicit
`lv.start()` still initializes LVGL without an attached display.

Panel rotation queues a geometry change, applied before LVGL timers run on the next
application-loop poll. This keeps resolution-change callbacks inside Berry's execution
deadline. A Berry script that rotates the panel must allow that poll before explicitly
forcing an LVGL refresh. The retained partial buffer can fit at least one row in either
orientation.

The imported ctypes tables require 32-bit pointers, `size_t`, Berry integers
and Berry reals. They also assume the matched LVGL configuration and normal
32-bit C enums. The MCU runtime rejects incompatible pointer/value widths at
compile time. These bindings are not suitable for a native 64-bit host VM.

Source provenance and binding generation inputs are recorded in
[the import notes](../../../../../third_party/lvgl/IMPORT.md).
