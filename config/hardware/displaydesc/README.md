# Display descriptors

These panel descriptors were imported from Tasmota's
`tasmota/displaydesc/` at revision
`c00ece86819fce87a4ad7c440b1995623ec6a5e0`. The upstream descriptors are
part of the Tasmota project (GPL-3.0-or-later); their file names and any embedded
notices are retained. The four e-paper descriptors have one Newmota change: their
`:H` interface is `EPD` instead of `SPI`.

To use one, enable `USE_DISPLAY`, choose the descriptor for the panel and board,
and upload it through Newmota's browser file manager as `/display.ini` in the
device's LittleFS, or list it under `filesystem` in a
[user configuration](../../../user_config/README.md#files-in-the-factory-filesystem) to put it
into the factory image. Configure the required GPIO functions in the device's hardware
template and keep `*` in the descriptor's pin fields. A physical pin in the descriptor is
accepted only on a GPIO the template leaves unassigned; on an assigned GPIO the display does not
start and the log names the pin. Restart the device to load the descriptor.

QSPI descriptors from Tasmota revision
`fb629eddb5047764781851ee059366328814a552` can be supplied separately with the
explicit `USE_DISPLAY_QSPI` feature. See the [four-line QSPI format and routes](../../../src/app/drivers/display/README.md#four-line-qspi);
only RGB565 and four data lines are supported.

The `:H` header selects the transport. `SPI` means an ordinary SPI display;
`EPD` means an SPI e-paper display with BUSY on the header's former MISO field.
The e-paper command and LUT sections require `EPD`. See the
[display driver documentation](../../../src/app/drivers/display/README.md)
for transport and platform limits. Renderer and legacy touch sections remain metadata. Optional
`USE_UNIVERSAL_TOUCH` executes the `:UTI`/`:UTT`/`:UTX`/`:UTY` I2C programs; see
the [supported instructions and limits](../../../src/app/drivers/display/README.md#universal-i2c-touch). Parsing a descriptor
does not qualify its wiring or panel behavior on hardware.

## Descriptors needing adaptation

These `.ini` files are not included in `make validate-catalog`; the compatibility
notes below come from a separate check with the runtime display parser.

The following imported files are retained for reference but the current Newmota
parser rejects them. The listed diagnostics came from parsing the imported files
with the Newmota parser; the `SPI,*` bus selector was normalized to software SPI
selector `3`, as the runtime does before parsing.

| Descriptor | Current parser issue |
| --- | --- |
| `GC9A01_display.ini`, `M5Stack_Dial_display.ini` | Unsupported `:A` address width `01` |
| `M5Stack_S3R.ini` | `:B` flush buffer section lacks a supported line count and flags pair |
| `M5StickCPlus2_ST7789v2_display.ini`, `ST7789_135x240_display.ini`, `esp32s3-esp32-1732S019_display.ini` | Unsupported `:r` startup rotation section |
| `Panlee_ZX4D30NE01S-UR_SC-02_no_touch.display.ini` | Incomplete RGB header |
| `SH1106_display.ini`, `SH1107_128x128_display.ini` | Initialization bytes contain a token outside the accepted byte format |
| `SSD1306_128x32_display.ini`, `SSD1306_70x42_display.ini` | Leading `:` title line appears before `:H` |
| `ST7701_480x480_WS_4inch.ini` | RGB SPI initialization records have invalid framing |
| `ST7735S_AtomS3_SPI2_display.ini`, `ST7735S_AtomS3_display.ini` | Initialization sequence has invalid framing |

Other descriptors pass syntax parsing, subject to the selected platform's
transport, pin route, memory, and panel support.
