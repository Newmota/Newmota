# GPIO Templates

Newmota uses Tasmota-style GPIO templates to describe how a product connects to a supported
chip. A template is runtime device configuration: it assigns functions such as LEDs, buttons,
and relays to pins. It does not select the native SDK build recipe; board recipes live under
[`config/boards/`](config/boards/).

The examples cover ESP and non-ESP targets. Each `GPIO` array follows that target
architecture's slot map. `1` means `User`, leaving the pin available for a later `GPIO<n>`
assignment; `GPIO<n>` uses the physical pin number. The board topology still rejects pins that
are absent, reserved, or not exposed by the selected board.

A personal build can start with a template, so it needs no commands. Paste the JSON into
`user_config/<id>.yaml`; the build checks it against the board:

```yaml
template: {"NAME":"ESP32 GPIO2 LED","GPIO":[1,1,288,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]}
```

It is the device's default: a device with no saved settings starts with it, and `Reset 1` or
`Reset 4` returns to it. `Reset 4` keeps Wi-Fi. A device that already has saved settings keeps
its template until then, and `Template` still replaces it at any time.

## ESP-IDF: classic ESP32 development board

For a classic ESP32 development board with a discrete LED on physical GPIO2, use the `esp32`
architecture's 40-slot array:

```text
Module 0
Template {"NAME":"ESP32 GPIO2 LED","ARCH":"esp32","GPIO":[1,1,288,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],"FLAG":0,"BASE":0}
Restart 1
```

Zero-based slot 2 maps to physical GPIO2 on this architecture. Use `320` instead of `288`
for an active-low LED. Check the board pinout first: GPIO2 is a strapping pin on the classic
ESP32, and development boards differ.

## ESP-IDF: Waveshare ESP32-S3-Zero

[Board pinout](https://docs.waveshare.com/ESP32-S3-Zero): BOOT on GPIO0, WS2812 RGB LED on
GPIO21. Requires `USE_WS2812`.

```text
Module 0
Template {"NAME":"Waveshare ESP32-S3-Zero","ARCH":"esp32s3","GPIO":[32,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1376,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],"FLAG":0,"BASE":0}
Restart 1
```

After restart, use `Pixels 1` for the single onboard LED.

## ESP-IDF: Waveshare ESP32-C5-LCD-1.47

The [board pinout](https://docs.waveshare.com/ESP32-C5-LCD-1.47) connects its ST7789
display to GPIO6 (MOSI), GPIO7 (clock), GPIO23 (CS), GPIO24 (D/C), GPIO26 (reset),
and GPIO10 (backlight); its WS2812 LED is on GPIO8. Build the `esp32c5` environment
with `USE_DISPLAY` and `USE_WS2812` enabled. Upload
[`ST7789_172x320_Waveshare_esp32c6_lcd_1_47.ini`](config/hardware/displaydesc/ST7789_172x320_Waveshare_esp32c6_lcd_1_47.ini)
as `/display.ini`: it describes the same panel dimensions and offsets. Then apply:

```text
Module 0
Template {"NAME":"Waveshare C5 LCD 1.47","ARCH":"esp32c5","GPIO":[1,1,1,1,1,1,704,736,1376,1,992,1,1,1,1,1,1,1,1,1,1,1,1,768,800,1,1024,1,1],"FLAG":0,"BASE":0}
Restart 1
```

Use `Pixels 1` for the onboard LED. The microSD slot shares the display's SPI clock
and MOSI; this template does not configure the card. The display descriptor and
wiring have not been qualified on this C5 board.

## ESP-IDF: ESP32-S3-EYE camera

The ESP32-S3-EYE's OV2640 is wired to GPIO4 (SIOD), GPIO5 (SIOC), GPIO6 (VSYNC), GPIO7 (HREF),
GPIO13 (PCLK), GPIO15 (XCLK) and D0…D7 on GPIO11, 9, 8, 10, 12, 18, 17, 16; it has no PWDN or
RESET line. Requires `USE_WEBCAM` and an 8 MiB flash, 8 MiB octal PSRAM build such as
`esp32s3-qio-opi-8m-8m`; see the [camera guide](src/app/drivers/camera/README.md).

```text
Module 0
Template {"NAME":"ESP32-S3-EYE","ARCH":"esp32s3","GPIO":[1,1,1,1,5024,5056,5120,5152,5090,5089,5091,5088,5092,5184,1,4992,5095,5094,5093,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],"FLAG":0,"BASE":0}
Restart 1
```

Then upload [`ov2640.be`](berry/drivers/camera/ov2640.be), add
`load("ov2640.be")  cam.register(OV2640())` to `autoexec.be`, and use `WcStream 1` for `http://<device>:81/stream`.

## Beken: CB2S-2M carrier

For a CB2S-2M carrier with a discrete LED wired to exposed GPIO26, use the `bk7231n`
architecture's 29-slot array:

```text
Module 0
Template {"NAME":"CB2S-2M GPIO26 LED","ARCH":"bk7231n","GPIO":[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,288,1,1],"FLAG":0,"BASE":0}
Restart 1
```

The CB2S-2M board recipe exposes GPIO26. This is an external LED example; the module itself
does not imply that a carrier has an LED connected there. See
[`config/boards/beken/cb2s-2m.json`](config/boards/beken/cb2s-2m.json).

## Beken: illustrative BL0937 metering carrier

A metering product uses one template for its controls and meter signals. The following
**illustrative carrier wiring** uses exposed CB2S pins; it is not a commercial plug pinout:

| Physical pin | Assumed connection | Function value |
| --- | --- | ---: |
| GPIO6 | BL0937 CF | 2720 |
| GPIO7 | BL0937 CF1 | 2656 |
| GPIO8 | Button 1 | 32 |
| GPIO10 | BL0937 SEL | 2592 |
| GPIO11 | Active-low LED 1 | 320 |
| GPIO26 | Relay 1 | 224 |

Build with `USE_BL0937`, verify the actual wiring and SEL polarity, then apply:

```text
Module 0
Template {"NAME":"CB2S BL0937 example","ARCH":"bk7231n","GPIO":[0,0,0,0,0,0,2720,2656,32,0,2592,320,0,0,0,0,0,0,0,0,0,0,0,0,0,0,224,0,0],"FLAG":0,"BASE":0}
Restart 1
```

Use `2624` for inverted SEL when the product requires it. The template selects the energy
driver automatically; no physical `EnergyConfig` tuple is needed. GPIO10/11 configuration
relinquishes the application console on those pads; bootloader output during reset remains.
Different chips need their own energy functions, for example BL0942 RX plus serial TX.
See [energy setup and calibration](src/app/drivers/energy/README.md).

## Beken: Tuya bulbs with LED driver chips

Tuya bulbs carry small BK7231N modules such as CB2L, CBLC or CB2S, or a chip directly on the
PCB. Build the `generic-bk7231n-qfn32-tuya` environment, which leaves every BK7231N pin to the
template, with the feature of the bulb's
[LED driver chip](src/app/drivers/led_driver/README.md). The pinouts below come from
OpenBeken's device catalog and are untested with Newmota.

Moes MWB-RCW RGBCW E27 bulb (BP5758D on a CBLC5 module, DAT on P24, CLK on P26; build with
`USE_BP5758D`):

```text
Module 0
Template {"NAME":"Moes MWB-RCW BP5758D","ARCH":"bk7231n","GPIO":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9056,0,9024,0,0],"FLAG":0,"BASE":0}
Restart 1
```

LSC Smart Connect GU10 bulb (SM2235 on a CBLC5 module, DAT on P24, CLK on P26; build with
`USE_SM2335`). `SM2335 DAT3` selects current code 2 for RGB and white, OpenBeken's
`SM2235_Current 2 2`. OpenBeken sends this chip's warm white before cold white, so
`SetOption37 1` swaps the two:

```text
Module 0
Template {"NAME":"LSC GU10 SM2235","ARCH":"bk7231n","GPIO":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9122,0,9088,0,0],"FLAG":0,"BASE":0}
SetOption37 1
Restart 1
```

The function values are the Tasmota ones: `9024`/`9056` BP5758D CLK/DAT, `9088` SM2335 CLK,
and `9120 + n − 1` for SM2335 DAT<n>. If the colours come out swapped, set the channel order
with `SetOption37`; `LightCurrent` changes the currents without a new template.

## Bouffalo: BL602 development board

For a BL602 carrier that brings out GPIO12 with a discrete LED connected, use the `bl602`
architecture's 23-slot array:

```text
Module 0
Template {"NAME":"BL602 GPIO12 LED","ARCH":"bl602","GPIO":[1,1,1,1,1,1,1,1,1,1,1,1,288,1,1,1,1,1,1,1,1,1,1],"FLAG":0,"BASE":0}
Restart 1
```

GPIO12 is an example connection, not a claim about every BL602 board. Confirm that the
selected carrier exposes it and does not use it for another function. The supported
`bl602-2m` recipe reserves GPIO7 and GPIO16 for its console.

## Bouffalo: Ai-Thinker development kits

Each kit has an active-high RGB indicator on PWM-capable pins, so its template assigns `PWM1`–
`PWM3` and Light drives it as a dimmable RGB light. Pins come from the Ai-Thinker kit
specifications. BL616 maps GPIO n to PWM channel n % 4 and BL602 to n % 5; two PWM pins on
one channel cannot run together.

Ai-M61-32S-Kit (`bl616dk`): red IO12, green IO14, blue IO15.

```text
Module 0
Template {"NAME":"Ai-M61-32S-Kit","ARCH":"bl616","GPIO":[1,1,1,1,1,1,1,1,1,1,1,1,416,1,417,418,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],"FLAG":0,"BASE":0}
Restart 1
```

Ai-M62-12F-Kit (`bl616dk`): red IO1, green IO30, blue IO0, and warm IO27 as `PWM4`, giving an
RGBW light. The white LED on IO29 stays `User`: it shares PWM channel 1 with red.

```text
Module 0
Template {"NAME":"Ai-M62-12F-Kit","ARCH":"bl616","GPIO":[418,416,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,419,1,1,417,1,1,1,1],"FLAG":0,"BASE":0}
Restart 1
```

Ai-WB2-12F-Kit (`bl602-4m`): red IO14, green IO17, blue IO3.

```text
Module 0
Template {"NAME":"Ai-WB2-12F-Kit","ARCH":"bl602","GPIO":[1,1,1,418,1,1,1,1,1,1,1,1,1,1,416,1,1,417,1,1,1,1,1],"FLAG":0,"BASE":0}
Restart 1
```

Use `Color FF0000` or `Dimmer 20` to test the light.

## AmebaD: BW16-Kit RGB indicator

The Ai-Thinker BW16-Kit schematic connects its RGB indicator channels to PA12, PA13, and
PA14 through resistors, with the LED common connected to ground. The `rtl8720dn` template map
uses the same pin numbers and has 64 slots. This template maps the three channels as separate
active-high discrete LED instances:

```text
Module 0
Template {"NAME":"BW16-Kit RGB indicators","ARCH":"rtl8720dn","GPIO":[1,1,1,1,1,1,1,1,1,1,1,1,288,289,290,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1],"FLAG":0,"BASE":0}
Restart 1
```

These are discrete LED outputs, not an RGB `Light` configuration. The board recipe exposes
PA12, PA13, and PA14; see [`config/boards/amebad/bw16-2m.json`](config/boards/amebad/bw16-2m.json)
and the [BW16-Kit specification](https://docs.ai-thinker.com/_media/bw16-kit_v1.2.3_product_specification_en.pdf).

## Pico W: external LED

For an external discrete LED connected to Pico W GPIO15, use the `rp2040` architecture's
30-slot array:

```text
Module 0
Template {"NAME":"Pico W GPIO15 LED","ARCH":"rp2040","GPIO":[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,288,1,1,1,1,1,1,1,1,1,1,1,1,1,1],"FLAG":0,"BASE":0}
Restart 1
```

This assigns an external LED. The Pico W's onboard LED is connected to the wireless chip,
not an RP2040 GPIO, so this template cannot control it. See the [Raspberry Pi Pico W
datasheet](https://datasheets.raspberrypi.com/picow/pico-w-datasheet.pdf).

## LED and PWM function values

Template entries use Tasmota-compatible function IDs, not pin numbers. They encode the
function and its one-based instance:

| Value | Function | Meaning |
| ---: | --- | --- |
| `0` | None | No function assigned |
| `1` | User | Available for a physical `GPIO<n>` assignment |
| `288` | `GPIO_LED1` | Active-high discrete LED, instance 1 |
| `289` | `GPIO_LED1` | Active-high discrete LED, instance 2 |
| `290` | `GPIO_LED1` | Active-high discrete LED, instance 3 |
| `320` | `GPIO_LED1_INV` | Active-low discrete LED, instance 1 |
| `544` | `GPIO_LEDLNK` | Active-high network link LED |
| `576` | `GPIO_LEDLNK_INV` | Active-low network link LED |
| `416`–`419` | `GPIO_PWM1` | PWM output, instances 1–4 |
| `1376` | `GPIO_WS2812` | Addressable LED output, with `USE_WS2812` enabled |

The numeric ID formula is `(base << 5) | (instance - 1)`. For example, `GPIO_LED1` has base
9, so its first instance is `(9 << 5) | 0`, or `288`. Use `320` for an active-low LED.

`GPIO_WS2812` requires a build with `USE_WS2812` enabled. See the
[LED strip documentation](src/app/drivers/led_strip/README.md) for configuration and platform support.

For the current function registry, see [`config/hardware/gpio-functions.toml`](config/hardware/gpio-functions.toml).
Architecture slot maps are in [`config/hardware/topologies/`](config/hardware/topologies/), and
runtime command behavior is described in [`config/hardware/README.md`](config/hardware/README.md).
