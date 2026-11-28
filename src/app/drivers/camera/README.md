# Cameras

Two optional features capture camera frames; they exclude each other.

- `USE_WEBCAM` reads a parallel (DVP) sensor that encodes JPEG itself. It needs an ESP chip
  with a DVP camera controller (`camera-dvp` capability: ESP32-S3, ESP32-P4, ESP32-S31) and
  selects `USE_BERRY`, `USE_I2C`, and `USE_WIFI`.
- `USE_CSI_WEBCAM` reads a MIPI-CSI sensor on the ESP32-P4 through its ISP, JPEG and H.264
  hardware; see [MIPI-CSI on the ESP32-P4](#mipi-csi-on-the-esp32-p4). It selects
  `USE_BERRY` and `USE_I2C`.

Both need a board that declares PSRAM, because the frame buffers live there. The firmware
contains no sensor drivers. A sensor is a Berry file that configures the chip over I2C; the
native side owns the clock or PHY supply, the capture controller, the frame buffers, the
encoders, and the network endpoints. The sensor files are in
[`berry/drivers/camera/`](../../../../berry/drivers/camera/): `ov2640.be` (DVP),
`ov5647.be`, `imx219.be` and `ov02c10.be` (CSI). The OV02C10 reads RAW10 without binning,
up to 1920x1080, at a fixed exposure near the frame length: neither its automatic exposure nor
gain is qualified, and there is no ISP tuning for it yet.

## DVP pins

Assign the camera with Tasmota's webcam functions in the Template:

| Function | Value | Required |
| --- | --- | --- |
| `WEBCAM_XCLK` | 4992 | Yes |
| `WEBCAM_SIOD` / `WEBCAM_SIOC` | 5024 / 5056 | Yes, unless the sensor is on a configured I2C bus |
| `WEBCAM_DATA1` … `WEBCAM_DATA8` | 5088 … 5095 | Yes (sensor D0 … D7) |
| `WEBCAM_VSYNC` / `WEBCAM_HREF` / `WEBCAM_PCLK` | 5120 / 5152 / 5184 | Yes |
| `WEBCAM_PWDN` / `WEBCAM_RESET` | 4928 / 4960 | No |

SIOD and SIOC become the first I2C bus that has no `I2C SCL`/`I2C SDA` pins, so Berry reaches
the sensor with `newmota.wire_scan()` like any other I2C device. PWDN is held high (powered
down) until the camera starts; RESET is pulsed low at each start. The ESP32-S3-EYE template is
in the [template guide](../../../../TEMPLATE.md#esp-idf-esp32-s3-eye-camera).

## Start-up

1. Upload `ov2640.be` and add `load("ov2640.be")  cam.register(OV2640())` to `autoexec.be`.
   A build that lists it under [`solidify`](../../../../user_config/README.md#built-in-berry-drivers)
   has the class built in and needs only `cam.register(OV2640())`. A build can also ship the
   files in the factory filesystem with
   [`filesystem`](../../../../user_config/README.md#files-in-the-factory-filesystem).
2. With complete pins, the camera starts at once with the stored resolution and settings.
3. `WcStream 1` starts the stream server on port 81; it is stored and starts with the camera
   from then on.

A start runs in this order: PWDN low, XCLK on, RESET pulse, sensor `start(resolution)`, capture,
sensor `stream(true)`, then every stored setting through sensor `set(key, value)`. A failure
stops XCLK and leaves the reason in `Webcam` and `cam.info()`.

## Stream endpoint

Port 81 runs a separate plain HTTP server, because frames are larger than the main web
service's bounded responses:

| Path | Response |
| --- | --- |
| `/` | A page showing the stream |
| `/snapshot.jpg`, `/cam.jpg` | The newest JPEG frame |
| `/stream`, `/cam.mjpeg` | `multipart/x-mixed-replace` MJPEG |

While the camera streams and the server runs, the web UI's Home page shows the port-81 page
in a card, as Tasmota's main page shows the stream. The card is dropped when Home leaves the
screen, so it does not take a viewer slot. A page loaded over HTTPS may not embed the plain
HTTP stream and shows only the link to it.

The endpoint has no password, like Tasmota's. One sender task writes each frame to up to three
MJPEG viewers, as Tasmota's stream serves several clients; a slow viewer slows the others, and a
fourth gets `503`, which the `/` page retries every 2 s. The audio listener also uses port 81, so `I2SStream` listening
and `WcStream 1` exclude each other.

## Commands

Settings are stored in `/.newmota/webcam.a`/`.b`, outside the shared settings record, and applied
at every start. Changing one while streaming applies it at once.

| Command | Payload | Meaning |
| --- | --- | --- |
| `Webcam` | | Stored settings, state, sensor and frame size |
| `WcInit` | | Restart the camera |
| `WcStop` | | Stop the camera and stream server, PWDN high |
| `WcStream` | `0`/`1`, `off`/`on` | Stream server on port 81 |
| `WcResolution` | `0`…`15` | Tasmota's framesize index (`10` = VGA, default; `15` = UXGA) |
| `WcStats` | | `FPS`, `CamFail` (dropped frames), `JpegFail` (damaged frames), `Frames`, `Buffer` |
| `WcSetDefaults` | | Tasmota's defaults for all settings below |
| `WcMirror`, `WcFlip` | `0`/`1` | Horizontal mirror, vertical flip |
| `WcBrightness`, `WcContrast`, `WcSaturation`, `WcAELevel` | `-2`…`2` | |
| `WcSpecialEffect` | `0`…`6` | None, negative, grey, red, green, blue, sepia |
| `WcAWB`, `WcAWBGain` | `0`/`1` | Auto white balance and its gain |
| `WcWBMode` | `0`…`4` | Auto, sunny, cloudy, office, home |
| `WcAEC`, `WcAECDSP` | `0`/`1` | Sensor and DSP auto exposure |
| `WcAECValue` | `0`…`1200` | Manual exposure |
| `WcAGC` | `0`/`1` | Auto gain |
| `WcAGCGain` | `0`…`30` | Manual gain |
| `WcGainCeiling` | `0`…`6` | 2× … 128× |
| `WcGammaCorrect`, `WcLensCorrect`, `WcWPC`, `WcBPC`, `WcDCW`, `WcColorbar` | `0`/`1` | DSP corrections and test pattern |
| `WcQuality` | `4`…`63` | JPEG quality scale, lower is better; default 12 |

Frame buffers hold a quarter byte per pixel (at least 32 KiB): 76 KiB at VGA, 480 KiB at UXGA,
three of them plus the controller's spare in PSRAM. A frame that does not fit is counted in
`JpegFail`; raise `WcQuality` or lower the resolution.

## Berry

The `cam` module follows Tasmota's `cam` module where the two overlap: `setup`, `get_image`
and `info` with its `mode`, `width` and `height` keys. Tasmota's `cam.init()` has no
counterpart, because `register` starts the camera.

```berry
import cam
cam.info()             # {'state': 'streaming', 'sensor': 'OV2640', 'mode': 10, 'width': 640, ...}
var jpeg = cam.get_image()   # bytes of the newest frame, or nil; raises above 32 KiB
cam.setup(13)          # restart at a resolution without storing it; no argument: the stored one
cam.stop()
```

With `USE_BERRY_IMAGE`, `cam.get_image(img [, format])` stores the frame in an
[`img`](../../services/berry/modules/img/README.md) instead, or decodes it there into RGB565,
grayscale or RGB888, and returns `true`, or `false` without a frame. The `img` keeps its
buffer from frame to frame, and there is no 32 KiB limit.

`get_image()` returns the newest completed frame without waiting for the next one, so two
quick calls can return the same frame. It is bound by Berry's 32 KiB `bytes` limit and raises
`value_error` for a larger frame; use a lower resolution or a higher `WcQuality`, or fetch
`/snapshot.jpg` on port 81.

A sensor class provides:

| Member | Meaning |
| --- | --- |
| `start(resolution)` | Detect and configure the sensor; return `true` on success |
| `stream(on)` | Start or stop sensor output |
| `set(key, value)` | Apply one setting; return `false` for an unsupported key |
| `width`, `height`, `name` | Set by `start` for the active mode |
| `xclk` | Optional sensor clock in Hz; default 20 MHz |

Register helpers avoid nested Berry lists for large tables:

- `cam.write_regs(wire, addr, table [, reg_bytes])` writes `bytes` of register/value pairs.
  With 2-byte registers, register `0xFFFF` waits for value milliseconds.
- `cam.write_reg(wire, addr, reg, value [, reg_bytes])` and
  `cam.read_reg(wire, addr, reg [, reg_bytes])` access one register. Reads send the
  register address and the read as two transfers, because SCCB has no repeated start.

The sensor runs inside the Berry execution deadline. Sensor calls happen only at start,
stop and setting changes; frames never pass through the VM.

## MIPI-CSI on the ESP32-P4

`USE_CSI_WEBCAM` is a port of Tasmota's CSI camera. The sensor is on a configured I2C bus;
the P4's MIPI PHY supply (LDO 3 at 2.5 V) is switched on before the sensor is configured.
The ISP takes its colour matrix, gamma, sharpening, contrast, denoise and auto white
balance from `/isp.json`, one object per sensor name; without it the ISP runs bare.
[`isp/imx219.json`](../../../../berry/drivers/camera/isp/imx219.json) and
[`isp/ov5647.json`](../../../../berry/drivers/camera/isp/ov5647.json) are Tasmota's tunings
for the IMX219 and OV5647; upload the one for your sensor as `/isp.json`. `WcQuality` (1…100, default 50) sets the JPEG quality and the H.264
bitrate (quality × 60 kbit/s) and restarts a running camera.

`WcSession` selects the output, stored with the other settings:

| Session | Output |
| --- | --- |
| `1` MJPEG | `/snapshot.jpg` and `/stream` on port 81, Berry `cam.get_image()` |
| `2` H.264 | RTSP on port 554 (`rtsp://<device>/`), and a WebSocket on port 82 that the page on port 81 plays with WebCodecs |
| `3` WebRTC | The page on port 81 opens a WebRTC connection; video and audio go to UDP 5005 |
| `4` Still | With `USE_CSI_STILL`: a full-sensor JPEG still for each `/snapshot.jpg` on port 81; see [full-sensor stills](#full-sensor-stills) |
| `0` | Stops the camera |

WebRTC is LAN only: the device is an ICE-lite host candidate and a DTLS 1.2 server with a
fixed self-signed certificate (Tasmota's), pinned by its fingerprint in the SDP answer.
Each track sends an RTCP sender report every 5 seconds, and a picture loss or full intra
request from the viewer makes the next frame an IDR.
With `USE_I2S` the configured I2S microphone is sent at 8 kHz, as Opus with
`USE_AUDIO_OPUS`, otherwise as G.711 A-law; the audio driver must not be using the input
at the same time. Without `USE_I2S` the answer offers an audio track that stays silent.
The offer is posted to `POST /webrtc/offer` on port 81, because SDP offers exceed the
main web service's request size. With `USE_MATTER_CAMERA`, a Matter controller can send the
offer instead; see [Matter](../../services/matter/README.md#tcp-and-the-camera).

The pixel formats depend on the P4 silicon revision, which the board's `revision` selects
at build time:

| Silicon | Frames | JPEG |
| --- | --- | --- |
| Revision 3.x (`esp32p4r3`, `esp32p4x`) | YUV420 in every session | Every session: snapshots also work while H.264 or WebRTC streams |
| Before 3.0 (`esp32p4`), IDF 5.5 | YUV422 for MJPEG, YUV420 for H.264 and WebRTC | MJPEG session only |
| Before 3.0 on IDF 6.1 | YUV420 for H.264 and WebRTC | None: IDF 6 names a YUV422 byte order the old ISP cannot feed the JPEG encoder |

The P4 reaches the network through its ESP-Hosted companion (see the
[network service](../../services/network/README.md#esp-hosted-on-the-esp32-p4)); a build
without Wi-Fi refuses the H.264 and WebRTC sessions and the port-81 server, and the MJPEG
session runs for Berry `cam.get_image()` only.

| Command | Payload | Meaning |
| --- | --- | --- |
| `WcSession` | `0`…`3`, `4` | Output session; a change restarts the camera. `4` needs `USE_CSI_STILL` |
| `WcRes`, `WcResolution` | `0`…`254` | Sensor preset, as the sensor file defines them |
| `WcWindow` | `x,y,w,h,bin,fps,format` | Custom window: `x`,`y` shift it from the centre in output pixels (may be negative), binning 1 or 2, format 0 RAW8, 1 RAW10, 2 RAW12; the reply has the applied offsets |
| `WcStatus` | | Frame counters, encoder time, sizes, motion, SPS/PPS and WebRTC state |
| `WcConfig` | | Sensor, size, format, lanes, lane rate (`MipiClock`), FPS |
| `WcPan`, `WcTilt` | `-100`…`100` | Digital pan or tilt: shifts the window within the sensor; positive pans right and tilts up |
| `WcPosition` | `pan,tilt` | Both at once; the reply has the applied position and offsets |

The ISP takes at most 1920x1080. When the sensor's mode is larger, the camera asks the
sensor for a centred window cut to that size on each axis, as Tasmota does, so the 8 MP
IMX219 preset streams 1920x1080 of the middle of the sensor. H.264 and WebRTC also need both
sides to be multiples of 16: their window is cut further, 1920x1080 to 1920x1072. MJPEG
windows are aligned to 8x2. Streams take three PSRAM frame buffers, or two when the third would
leave no room for the controller's spare frame or the encoders' buffers; IDF 5.5 keeps the controller claimed until
reboot when it runs out of memory, so the camera checks before it creates the controller.

The DVP image commands (`WcBrightness`, …) are passed to the sensor's `set`; the CSI sensor
files take only `WcMirror` and `WcFlip`, since the ISP does the image tuning. A CSI sensor
gets every setting before `start`, and a mirror or flip change restarts the camera, because it
changes the sensor's colour filter order that the ISP is built for. In H.264 sessions
the size of P-frames gives Tasmota's motion value in `WcStatus` and `cam.info()`.

A CSI sensor file also sets `format`, `lanes` and `lane_mbps` (or Tasmota's `mipi_clock`),
`fps` (the rate its timing programs), `bin_mode`, `bayer` (colour filter order after flip and
mirror: 0 BGGR, 1 GBRG, 2 GRBG, 3 RGGB) and the applied `offset_x`/`offset_y` in `start`. It
provides `window(x, y, w, h, bin, fps, format)` for `WcWindow` and for the cut to the ISP
limit; `x` and `y` shift the window from the centre, and the sensor clamps and aligns them.
It also has `max_width` and `max_height`, the full array, and takes `set("session", n)`
before `start` so it can choose its still mode.

### Pan and tilt

Pan and tilt move the window within the sensor; they do not scale. They work where the
window is smaller than the sensor (or than its binned frame): 1920x1080 of the OV5647's
2592x1944 pans by 336 pixels each way. `0,0` is the centre, ±100 the edge, and mirror and
flip are taken into account so that positive pan always moves the picture's view right.
The sensor clamps and aligns the shift; replies give the applied position.

Underneath, the window is a viewport: the rectangle of the sensor it shows, in full-sensor
pixels before binning, oriented as the picture is shown, as Matter's digital PTZ describes it.
Pan and tilt, the Home card and Matter all move that rectangle. In Berry:

```berry
import cam
cam.viewport()                          # [336, 432, 2256, 1512], nil while not streaming
cam.set_viewport(0, 0, 1920, 1080)      # the applied viewport, or nil
cam.info()['sensor_width']              # 2592: the array the viewport moves in
```

`set_viewport` keeps the window's size: the camera crops and cannot zoom, so a rectangle of
another size, or one that leaves the sensor, returns nil. [Matter](../../services/matter/README.md#tcp-and-the-camera)
maps its digital PTZ onto the same calls.

When the sensor keeps the mode, only its window registers change: capture pauses, the
sensor is reprogrammed, and viewers stay connected through a short gap; H.264 viewers get an
IDR. Otherwise the camera restarts. The position is not stored: a restart, a preset or a
`WcWindow` centres it again. While the window can move, the Home camera card shows a
direction pad that steps a tenth of the range.

### Full-sensor stills

`USE_CSI_STILL` adds session `4`. The ISP takes at most 1920x1080, so stills bypass it: the
sensor sends RAW8 (or packed RAW10) into one PSRAM buffer, and a software ISP corrects it
and writes RGB565 in place for the hardware JPEG encoder: black and white points from the green
histogram, grey-world white balance, bilinear demosaicing, the 6500 K colour matrix of the
sensor's `/isp.json` section (the one the ISP uses for streams) and a gamma-like curve. Stills
apply the white balance before the matrix, stream AWB after it, so their tints can differ
slightly.

Sensor and controller run all the time, so exposure has settled before a request; the
controller writes into its own spare buffer except for the one frame a waiting reader gets.
Each `/snapshot.jpg` captures a new still, and `/stream` becomes a slow MJPEG of stills.
The port-81 page shows one still and a button for the next. The first second after a start
is left to the sensor's exposure. Tasmota measured the OV5647 at 2592x1944: about 0.7 s of
conversion with one colour per 2x2 cell and 70 ms of JPEG encoding per still; bilinear
demosaicing and the colour matrix cost about three times that conversion on a desktop. `WcStatus` reports `ConvertUs` and
`EncodeUs`; Berry `cam.get_image()` is bound by its 32 KiB limit and refuses most stills.

RAW8 lines reach 2880 pixels (the bypassed ISP still checks 1920 before it counts words, so
a wider line is declared as RAW12 with the same word count), RAW10 lines 1920. A larger
sensor mode is cut on each axis, so the IMX219's 3280x2464 becomes 2880x2464. The sensor files
pick RAW8 presets in this session, and the OV5647 a brighter exposure target. Memory is
checked before the controller is created, because IDF 5.5 keeps the controller claimed
until reboot when its constructor runs out: two bytes per pixel for the buffer, the raw frame
again for the controller's spare, half a byte per pixel for the JPEG, and 512 KiB. At
2592x1944 RAW8 that is about 18 MB.
