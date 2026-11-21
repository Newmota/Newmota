# Portable application

This directory contains the firmware behavior shared by all current platform families.
Application code uses the narrow interfaces in `src/platform/include/` and does not include
vendor SDK headers.

`Status 4` (`StatusMEM.PsrMode`) and the web Information page report the build's configured
PSRAM bus mode: `QSPI` (quad), `OPI` (octal), or `HEX` (16-line). The mode remains visible
if PSRAM initialization fails; `PsrMax` and `PsrFree` still require initialized memory.
Builds without a configured PSRAM bus mode omit it.

## Current layout

| Directory | Responsibility |
| --- | --- |
| `core/` | Application loop, scheduler, command processor, message routing, hardware resolution, filesystem, update session, restart, reset, and boot state |
| `drivers/` | Hardware-facing portable owners, including digital I/O, PWM, display, serial, and audio |
| `services/` | Network, Wi-Fi commands, time/NTP, settings, status, power endpoint routing, PWM Light, MQTT, Berry, and web services |
| `support/` | Small portable helpers such as bounded text, number, and CRC operations |
| `include/` | Public application headers and the command/core type declarations |

The current normal application path includes Wi-Fi onboarding and recovery AP behavior,
HTTP+Gzip browser UI on Compact and Safeboot builds, HTTPS+Brotli by default on Standard/Large
application profiles across all platform families, configurable plain MQTT with LWT/group/fallback routing and
retained publications. Application MQTT working buffers are allocated only while persisted
settings enable the broker. When Berry and MQTT are both selected, scripts can `import mqtt`
to publish strings or bytes and subscribe with topic callbacks; filters are restored after
reconnects. The path also includes NTP, Berry with the global `newmota.cmd()` and
`newmota.gc()` methods, `millis()`/`time_reached()`, one-shot `set_timer()`/`remove_timer()`
and `defer()` callbacks, plus `delay([0..50])` and `yield()` execution pauses. When time is selected, `rtc()`, `time_dump()` and `time_str()`
use the shared time owner; `strftime()` and `strptime()` provide bounded portable date
conversion, and `add_cron()`/`remove_cron()`/`next_cron()` provide cooperative local-time
recurrence. Cron callbacks share the 16-entry callback limit with timers and deferred work.
`StatusTIM.Source` and Berry `rtc('source')` identify the latest accepted sample as
`none`, `manual`, `ntp`, `gps`, or `rtc`. Optional default-off `USE_GPS` provides
UBX receiver setup, UTC priority over NTP, and navigation diagnostics; see
[GPS configuration and limits](drivers/gps/README.md).
Optional default-off RTC chip features add an external
RTC bootstrap and verified automatic-time-to-chip updates through the generic `USE_RTC_CHIPS`
owner; see the [RTC contract and limits](drivers/rtc/README.md).
Independent default-off `USE_NTP_SERVER` serves the shared UTC clock over IPv4
UDP123 in custom application variants, including explicit RTC/manual local fallback;
see [time serving and quality limits](services/time/README.md).
`newmota.wifi()` reports the connected station's signal quality from 0 through 100 and includes
`ip` when an address is available.
`USE_BERRY_ENERGY_API` adds safe `import energy` snapshots; `USE_BERRY_ENERGY`
also selects the virtual Berry provider with script measurement/counter publication.
Its read-only `use_overtemp` driver metadata does not select overtemperature enforcement
or trigger device/protection polling. Read-only `power_on` observes logical Power/SO21
without selecting driver power-policy enforcement.
Optional platform-neutral `USE_BERRY_ENERGY_ACCOUNTING_API` adds read-only signed
live Today and retained Yesterday projections.
`USE_BERRY_ENERGY_BALANCED_API` independently exposes read-only selected-provider balanced
runtime import/export sums without requiring a trusted date or changing accounting.
`USE_BERRY_ENERGY_PERIOD_API` optionally exposes read-only primary-channel pending
Period snapshots, explicitly selecting the Period owner's MQTT and ENERGY output dependencies.
It adds no platform-family restriction or publish-state mutation on reads.
See the [energy API](services/berry/modules/energy/README.md).
Berry also provides owner-backed `light.get()`/`light.set()` for configured
Light endpoints, plus Tasmota-compatible `light.gamma10()`, `light.reverse_gamma10()`,
and `light.gamma8()` integer helpers, and an importable `path` module for LittleFS entry queries, bounded
directory listings, creation, removal, and renaming. The application also provides LittleFS
settings, asynchronous IPv4 ICMP reachability checks with `Ping <address>` and HTTP file downloads with `wget <URL>` on Wi-Fi application images
(see the [command interface](core/command/README.md)), Tasmota-style command shapes, and
GPIO/Module/Template configuration, with physical capability differences reported by each
adapter. On the ESP32-P4, Wi-Fi runs on an ESP-Hosted companion; `Hosted` reports both
firmware versions, and `HostedOta [url|version]` or `HostedLoad [file|version]` update the
companion. With `USE_BERRY_GPIO` (on by default), Berry's `import gpio` provides `pin_used(function[,index])` and
`pin(function[,index])` over active assignments; `index` is zero-based and an absent
assignment returns false or -1. Constants such as `gpio.REL1` retain Tasmota's unshifted
function bases from the frozen registry, including identities without an enabled driver.
`pin_used(gpio.OPTION_A, index)` reads Tasmota's Option_A flag `index` (A1 is 0); an
Option A value is a flag, not a pin, so `pin(gpio.OPTION_A, index)` returns -1.
`digital_read(pin)` reads physical levels without changing the mode.
`digital_write(pin, LOW|HIGH)` requires a pin that Berry has successfully configured as
`OUTPUT`; `pin_mode(pin, mode)` requires an available, unassigned pad or the template's
`Buzzer`/`Buzzer_i` pin, which belongs to Berry. Other configured pads, including failed
assignments, remain reserved.
`set_pwm(pin, duty)` (duty 0..1023, Tasmota's range) starts PWM on such a pad at 977 Hz on first
use, as Tasmota's; a `phase` argument other than 0 raises. `set_pwm_freq(pin, hz)` restarts the
output at that carrier, starting it at duty 0 on a pad not yet used for PWM, where Tasmota ignores
the call. `pin_mode()` ends PWM on the pad. A `Buzzer_i` pin inverts the PWM output.
Supported modes are `INPUT`, `OUTPUT` (initially LOW), `PULLUP`/`INPUT_PULLUP`, and
`PULLDOWN`/`INPUT_PULLDOWN`, subject to native pin capabilities. Open-drain and DAC
modes raise an error. Native mutation failures retain a faulted script pad until restart;
VM teardown retains pin configuration and output levels. GPIO constants identify functions
and do not imply driver support. Mutations of other configured pads, DAC voltage, `read_pwm`, and counters are not yet provided
by this module. [`examples/audio/rtttl.be`](../../examples/audio/rtttl.be) plays RTTTL songs on a
buzzer, or on I2S through the `USE_BERRY_AUDIO` tone generator, with the command `Rtttl <song>`.
Berry's `import persist` stores dynamic attributes as JSON in `/_persist.json`. It provides
`has`/`contains`, `find`, `remove`, `zero`, `load`, `dirty`, and `save`; `save(true)` forces a
write, and dirty values are flushed before controlled restarts. In-place edits to nested
lists and maps require `persist.dirty()` before saving. The module accepts JSON values within
8192 serialized bytes and 16 container levels, and replaces the committed file only after a
successful temporary-file write.
Berry's `math` module computes its transcendental functions in `support/float_math.cpp`, in
double and rounded once, within half an ulp of the exact result over every float. newlib's float
and double sets cost about 43 KB on BK7231N; the timers' sunrise formula uses the same kernels
in double.
The global [`pixmat` class](services/berry/modules/pixmat/README.md) provides mutable
grayscale, RGB, and RGBW framebuffers, clipped drawing, matrix copies, packed glyph
rendering, and scrolling independently of the strip hardware feature.
Optional `USE_BERRY_IMAGE` adds Tasmota's global [`img` class](services/berry/modules/img/README.md):
one image in a kept native buffer, JPEG decoding, format conversion and regions of interest.
Optional `USE_GLOBAL_TEMPERATURE` owns canonical automatic observations independently of
sensor presentation. `USE_POWER_FORCE_OFF` provides the reusable source forced-output
operation. Optional `USE_POWER_TARGETS` adds explicit logical endpoint-mask force,
restore, shared last-nonzero intent and ordinary-operation APIs without choosing meter/relay mappings.
`USE_ENERGY_PROTECTION_BINDINGS` supplies source Power group/channel mappings for energy protection.
`USE_ENERGY_LIVE_DAILY` collects a confirmed signed runtime daily projection independently
of clock validity, separately from dated history.
`USE_ENERGY_MAX_POWER` adds primary threshold/hold/window enforcement;
`USE_ENERGY_MAX_ENERGY` independently adds live-daily threshold/start-hour enforcement. `USE_ENERGY_OVERTEMP` binds those owners to primary driver opt-in and SO42.
Optional `USE_GLOBAL_TEMPERATURE_COMMANDS` adds volatile manual input and durable
`GlobalTemp2 0..250` typed temperature-field selection;
`USE_POWER_PROVENANCE` records explicit Power-operation origins and retains forced-operation
causes separately. Source always-on/timer
behavior, field ordering and remaining API gaps are documented
in the [energy device guide](drivers/energy/README.md).
Optional `USE_ENERGY_STATUS9` projects the primary retained PowerDelta and P/V/I
thresholds into StatusPTH, plus MaxPower/MaxEnergy fields when their corresponding
protection features are selected.
Optional `USE_STATE_TEXT` adds durable state-word aliases to Power/STATE/MQTT and
Energy MARGINS; other command responses retain their existing words.
Optional platform-neutral `USE_MODBUS_USER_PRECISION` independently enables dynamic
Modbus User-field `D:21..29` selector formatting through shared retained precision
settings. Literal decimal counts remain available without it; details are in the
[energy device guide](drivers/energy/README.md).
Optional `USE_ENERGY_COUNTER_FILTER` applies the primary hardware-total SetOption39
bandwidth before import baselines/floors change; explicit sample injection has its own
accounting contract. See the [energy device guide](drivers/energy/README.md).
Optional `USE_DISPLAY` loads `/display.ini` and provides a shared display owner for
SPI, I2C, EPD and I80 panels across all platform families, plus native ESP-IDF RGB
and DSI panels supported by the selected chip. Board pins and native bus routes
limit which descriptors can be used; see the
[transport table](drivers/display/README.md#transport-support). SPI and EPD
descriptors resolve `*` pins through the configured SPI GPIO functions; I2C `*` clock
and data fields use the shared I2C service. E-paper descriptors must name the interface
`EPD`. Berry's global `display()` class exposes `width()`, `height()`, `pixel()`,
`fill_rect()`, `refresh()`, `power()`, `invert()`, `rotation()` and `brightness()`;
see the [display Berry API](drivers/display/README.md). `USE_LVGL` automatically
selects this feature on all platforms and uses the descriptor panel as its default display.
Display support is off by default and has not been hardware-qualified.

Optional `USE_I2S` provides a native I2S owner, bounded audio sessions, Tasmota-compatible
`I2SPlay`, `I2SLoop`, `I2SRec`, `I2SWr`, `I2SStream`, `I2SStop`, `I2SGain`, `I2SPause`, `I2SSay`, `I2STime` commands, and with `USE_BERRY_AUDIO` the Berry `audio` module (`Session`, `level`, `spectrum`, `tone`).
Compressed MP3, AAC, and fixed-point Opus support is selected independently. Audio requires
at least 4 MiB flash; supported transports and their SoC limits are listed in the
[platform guide](../platform/README.md#optional-i2s-audio), with command and session details
in the [audio guide](drivers/audio/README.md).
Optional `USE_WEBCAM` captures JPEG frames from a DVP camera whose sensor driver is a Berry
file, serves snapshot and MJPEG on port 81, and keeps Tasmota's `Wc*` commands; see the
[camera guide](drivers/camera/README.md). `USE_CSI_WEBCAM` does the same for ESP32-P4
MIPI-CSI sensors with the ISP, H.264 over RTSP and WebSocket, and WebRTC.
Normal applications expose `PWM`, `PWMRange`, `PWMFrequency`, `Dimmer`,
hex, decimal-CSV, and fixed numeric preset `Color1`/`Color2`, grouped `White` and RGB
`HsbColor`, `CT`, `Channel`, `Fade`, `Speed`, `LedTable`, bounded `Palette`,
`Color +`/`Color -`, `Scheme 0..4`, `Wakeup`, `WakeupDuration`, and `RGBWWTable`.
`PWMFrequency Hz,channel` changes one configured output when the native planner can
preserve its timing siblings; `PWMFrequency ?,channel` queries its requested carrier, and
the global form resets channel overrides. Numeric Color values keep their fixed preset
meaning with a palette; Color +/- cycles custom entries. `Scheme id,start` optionally sets
an 8-bit wheel position or one-based palette entry for schemes 2–4. Configured PWM1–PWM5
belong to Light by default; `SetOption15 0` selects raw PWM after restart, while PWM6–PWM16
remain raw outputs. `SetOption68 1` selects independent Light power endpoints after restart.
`SetOption37 128` selects separate RGB and white power/brightness endpoints for four/five
channel Lights after restart, with `Dimmer1` for RGB and `Dimmer2`/`White` for white.
`SetOption37` values 0–119 reorder the PWM Light channels after restart, as in Tasmota.
SetOption68 takes precedence. Light on/off uses the existing command and browser power controls. Safeboot reuses the small portable core but gets a reduced feature map, a separate
recovery-only web page when networking is enabled, and no normal application UI or Berry
runtime. Network-capable Safeboot retains the MQTT client, command path, and common broker
settings record. Its MQTT runtime uses static storage to reduce recovery-image code size and
may occupy RAM while MQTT is disabled. The application allocates the MQTT runtime only while
the persisted broker is enabled; its application message router remains resident for WebUI
and power-state consumers.

Application images also provide 16 persisted Tasmota-style civil timers through `Timers`,
`Timer1`–`Timer16`, `Latitude`, `Longitude`, and `Sunrise`. A `TimerN` query returns one
record; `Timers` reports the global enable state. Actions 0–2 use an available Power endpoint.
Action 3 queues a Berry event for `newmota.add_timer_handler(fn)`, whose callback receives
`(timer, output, action, scheduled_local_epoch)`. Without a registered handler, no script
runs. Rules and the Tasmota no-Rules blink fallback are not implemented. Timers require the
application Time service; scheduling starts as soon as its clock is valid and does not replay
missed minutes. Safeboot excludes the timer commands and scheduler.

Application GPIO buttons use a four-second startup guard and a 10 ms input filter. Filtering and edge
tracking continue during the guard, while actions and pending press sequences are suppressed.
`ButtonDebounce` queries or saves the handler interval and filter flags (default 50;
range 40..1000 except
values ending in 9). The filter integrates logical input samples up/down over the value
divided by 10; changes are dispatched at the configured millisecond interval.
The units digit's bit 0 requires consecutive LOW samples before leaving a stable HIGH;
bit 1 requires consecutive HIGH samples before leaving a stable LOW. These are logical
levels after button inversion: HIGH is released, LOW is pressed. Thus 51 filters noisy
presses, 52 filters noisy releases, and 53 does both. Bits 2 and 3 are ignored, as in Tasmota.
Values ending in 9 return `{"ButtonDebounce":"AC detection unsupported"}` and leave the
setting unchanged; native 2 ms AC pulse detection remains open.
Probes run during main passes and idle sleep; a blocked pass can delay sampling, and missed
samples are not replayed. Changes apply live after saving, discarding partial filter state
without generating an edge. Without a stored value it defaults to 50.
A single press normally toggles the matching logical Power endpoint, including a configured
PWM or WS2812 Light. Button1 can
select Power2 through Power5 with two through five presses inside the 500 ms multi-press
window; other buttons do not map multiple presses to additional outputs. `SetOption11`
swaps Button1's single/double actions when it is the only button and Power1 and Power2 are
the only endpoints, including the reported SINGLE/DOUBLE Action. With `SetOption13 0`,
holding a fresh post-startup press emits HOLD once and cancels its pending clicks.
`SetOption32 1..250` sets that threshold in 100 ms units (default 40, or four seconds).
Old unused SO32 zero values migrate to 40. `SetOption13 1` emits SINGLE immediately and
keeps single-only behavior. `SetOption73 1` emits SINGLE through PENTA and HOLD without
changing local Power; a release after HOLD or immediate SINGLE also emits CLEAR.
Events remain visible as level-2 `BTN:` lines when MQTT is unavailable.

`ButtonTopic <topic>` routes Tasmota's button keys to that topic's command prefix and POWER leaf;
0 disables this route, 1 copies the current device topic, and 2 selects the disabled
compiled default. Topic templates support the same MAC expansion as `Topic`.
`ButtonRetain ON|OFF` controls retention of these command publications; OFF also requests
paced best-effort clearing through the old session. `SetOption62 1` excludes HOLD from
retention, and `SetOption104` disables retention globally. When a remote route is connected,
the designated single-toggle action is consumed even if the writer is busy; a dropped
publication is logged. Without that route, ordinary local fallback applies. `SetOption61 1`
keeps local operation alongside ButtonTopic. Self/group toggle commands use concrete
ON/OFF values to avoid a second toggle. Multi-press and CLEAR key values publish
the ON state word, matching Tasmota; the SO73 JSON Action distinguishes those gestures.
Key payloads use the configured StateText words, but Power accepts only its standard words.
As in Tasmota, a custom word or HOLD sent to this device consumes the press without operating
Power; Newmota then reports a command error where Tasmota reports the current state.
SO73 additionally publishes `{"Button1":{"Action":"SINGLE"}}` on stat RESULT (or BUTTON1
with `SetOption4 1`), independently of ButtonTopic, unless `SetOption19 1` suppresses it
as in Tasmota. HOLD uses StateText4. Publications are
transient; congestion can drop them, and queued Actions are discarded on disconnect or
reconfiguration. An unreachable old broker may retain records requiring manual cleanup.
Rules execution, SO1/SO40 hold modifiers, extended-hold SO13 disabling, reset and six-press
Wi-Fi setup remain unsupported.

Optional `USE_SWITCHES` (on for applications) reads `Switch`, `Switch_n` and `Switch_pd`
GPIOs (Tasmota `GPIO_SWT1`, `_NP`, `_PD`, 32 instances) through the same 10 ms filter as
buttons. `SwitchDebounce` has the ButtonDebounce range and flags and also rejects values ending
in 9. `SwitchMode<x> 0..16` ports Tasmota's switch handler: the default 0 toggles Power<x> on
every change, 1 and 2 follow the level (a pressed, LOW switch is OFF in mode 1), 3–7 act as push
buttons, 8–12 add multi-change and hold gestures, 13 and 14 only switch on, 15 and 16 only
report. `SwitchMode0` reads all 32 or sets them all; `SetOption32` sets the hold time. Modes 11
and 12 need several handler passes per hold; with a long `SwitchDebounce` and a short
`SetOption32` their INCREMENT repeat stops, as in Tasmota. The
level at startup is the switch's last state, so a restart never switches Power; a change during
the four-second startup guard acts once the guard ends. Every key logs `SWT: Switch1 State 2`.
`SetOption114 1` detaches all switches from Power and publishes `{"Switch1":{"Action":"ON"}}`
on stat RESULT (or SWITCH1 with `SetOption4 1`) unless `SetOption19 1`.

`SwitchTopic <topic>` sends switch keys to that topic as `cmnd/<topic>/POWER<x>`, like
ButtonTopic (0 disables, 1 copies the device topic, 2 selects the disabled default); keys are
indexed by switch number, and a connected route replaces local Power unless `SetOption61 1`. As in
Tasmota, extended gestures (INCREMENT, INV, CLEAR, RELEASE, 100, DELAYED) publish the ON word;
`SetOption114` Actions carry the gesture itself.
`SwitchRetain ON|OFF` retains these publications; OFF clears the retained keys. SENSOR and
Status 8 begin with `"Switch1":"ON"` for each configured switch, the level flipped by the
`_INV` modes, and modes 15 and 16 publish SENSOR on every change. `SwitchText<x>` (1–28, at
most 15 characters without quotes or backslashes, spaces removed, a leading `"` clears) replaces `Switch<x>` in SENSOR and in Action
messages; `SwitchText` lists all 28. Safeboot excludes switches and their commands but keeps
their settings.

`WebLog 3` or `SerialLog 3` adds press diagnostics. Successful button, native timer
and pulse-timer operations also log `RSL: POWER = ON` (or indexed `POWER<n>`) at level 2,
including when MQTT is disabled. Command replies retain their own delivery path.
Safeboot excludes physical button handling and `ButtonDebounce`, `ButtonTopic` and
`ButtonRetain` commands. It preserves application assignments, debounce and routing settings.

The independently selectable ADE7953 I2C and Pro SPI variants expose one through
four channels through the energy service; see the [model table and current gaps](drivers/energy/README.md#ade7953-i2c-and-spi-variants).

Optional `USE_DEVICE_SPI` provides a shared GPIO SPI owner for bounded read,
write and duplex device transactions on all five families. It supports independent
CS devices and generation-checked bus leases; it is not a hardware-controller SPI
backend. See the [device SPI ownership and timing contract](drivers/spi/README.md).

Optional `USE_SDCARD` (ESP-IDF, Bouffalo, AmebaD, Pico) routes `/sd` and every path below it
to a FAT volume on an SD card through the vendored [FatFs](../third_party/fatfs/README.md); all
other paths stay on LittleFS. Listing `/` shows `sd` while the card is mounted, renames stay
within one volume, and web uploads stage on their destination volume.

The card mounts once at boot, right after GPIO init, with Tasmota's pins: `SDCard CS` with the
SPI pins of its bus (instance 2 wins over 1), otherwise `SDIO CMD`, `CLK` and `D0`, four-bit
when `D1`–`D3` are all assigned. It is never formatted; the log names why a mount failed (no
card, no FAT volume, unusable pins). Files carry local time once the time service is
synchronized, 2026-01-01 before. A controlled restart or deep sleep syncs open writers and
unmounts the card first. ESP-IDF supports SPI and SDIO cards; Bouffalo, AmebaD and Pico
support SPI mode, and BL616/BL618DG also four-bit SDIO; on Beken the card reports
unavailable.

Optional `USE_CSE7766`, `USE_PZEM004T`, `USE_MCP39F501`, `USE_ENERGY_DUMMY`,
`USE_BL0939`, `USE_BL0940`, `USE_BL0942` (or the `USE_BL09XX` umbrella), `USE_ENERGY_ANALOG`, the pulse selections
`USE_HLW8012`/`USE_BL0937`, and Modbus `USE_PZEM_AC`, `USE_PZEM_DC`, `USE_SDM120`,
`USE_DDS2382`, `USE_SDM630`, `USE_DDSU666`, `USE_FIF_LE01MR`, `USE_IEM3000`,
`USE_ORNO_WE517`, `USE_SDM72`, `USE_SDM230`, and `USE_MODBUS_ENERGY` provide independently selectable
energy drivers. One active provider is selected through the normal Module/Template/GPIO
configuration and starts after explicit restart. Its physical hardware determines its
phases/channels; no separate physical `EnergyConfig` tuple is needed. `USE_CSE7766` covers
the common CSE7759/CSE7766/HLW8032 protocol and retains learned chip calibration.
Source-compatible calibration and driver-specific configuration act on the selected provider.
Optional accounting/protection features add precision, directional daily and tariff accounts,
Total and Period compatibility, margins, PowerDelta and source Power/SetOption21 policies.
Energy data joins StatusSNS and MQTT SENSOR through the shared output owner.
See the [energy setup and calibration guide](drivers/energy/README.md).
Shelly Dimmer and its external controller firmware-upgrade path are excluded at
user request; energy ID 31 remains permanently reserved. The unsupported Shelly
runtime GPIO assignments and external device-Light endpoint registry are absent;
the frozen GPIO ID rows remain provenance only.
Ordinary Power/Light services remain available independently.
MK SKY BLU, SolaX X1, Teleinfo and BIOPDU runtime drivers are deferred until explicit
request; their permanent IDs remain provenance only. BL6523 and the Shelly 2.5
ADE7953 mapping are excluded; other ADE7953 variants and generic ADE7880 remain.

Optional `USE_BH1750`, `USE_TC74`, and `USE_BME280` drivers automatically select `USE_I2C`
and use the configured shared buses. `USE_I2C` also exposes `I2CScan` for probing active
buses. The Bosch driver detects BME280 and BMP280 devices;
BMP280 readings omit humidity. `USE_DHT` adds DHT11, DHT22, and SI7021 pulse-reading support
on all five platform families when assigned to a pin with digital input and output capability.
Use the matching `GPIO_DHT11`, `GPIO_DHT22`, or `GPIO_SI7021` assignment. The DHT driver
supports one assignment of each model. All four drivers default off. Selected
drivers form a generated constant registry; `USE_SENSORS` is derived from feature metadata.

The sensor service keeps canonical samples in Celsius, percent relative humidity, and hPa,
then applies unit conversion and precision consistently to StatusSNS, MQTT SENSOR, the
`/api/sensors` response, and `/api/sensors/readings`. Temperature uses `SetOption8` to select
Celsius or Fahrenheit. Pressure uses `SetOption24` to select hPa or mercury units, and
`SetOption139` selects inHg instead of mmHg. `TempRes`, `HumRes`, and `PressRes` set each
measurement's decimal precision from 0 through 3; each defaults to one decimal place.
`TempOffset` accepts -12.6 through 12.6 and `HumOffset` accepts -10.0 through 10.0, in
tenths. Temperature compensation is applied in the selected output unit. A failed sensor
sample is omitted until the next successful reading. Browser readings include sensor names,
fields, labels, projected units, and numeric values. Drivers do not render individual HTML
callbacks.

Besides those, readings carry battery and soil moisture in percent, conductivity in µS/cm,
weight in kg, impedance in ohm, signal strength in dBm, the age of the last observation in
seconds, 0/1 states, voltage in V, current in A, power in W, energy in kWh, carbon dioxide in
ppm, volatile organic compounds and particulates in µg/m³, distance in m, counts and the UV
index. A driver measuring elsewhere, such as a radio sensor, is marked
`remote` in its feature metadata; its temperatures never feed the global temperature.

### Berry drivers and commands

With the default-off `USE_BERRY_DRIVERS` feature, Berry scripts do the jobs of Tasmota Rules
the way Tasmota's Berry drivers and commands do. `newmota.add_driver(obj)` registers any
instance, without a limit; `newmota.remove_driver(obj)` removes it. For each event Newmota looks
up the method name on every driver, in registration order, as Tasmota does: a class method or
any member that holds a function, also one assigned after `add_driver()`. It calls the function
as `f(obj, cmd, idx, payload, raw)` with `raw` nil; `cmd` and `payload` are empty strings except
for `audio`. Berry ignores unused arguments. Adding a driver twice does nothing. The global `Driver` class is
Tasmota's: an instance with one variable per hook, so `d = Driver()`, `d.every_second = def
(self) ... end` and `newmota.add_driver(d)` work in any order.

| Method | When | `idx` |
| --- | --- | --- |
| `every_50ms`, `every_100ms`, `every_250ms`, `every_second` | Scheduler ticks | 0 |
| `any_key` | Each button or switch key, as Tasmota's SendKey | `device_save<<24 \| key<<16 \| state<<8 \| device`, key 0 button, 1 switch, state Tasmota's `POWER_*` code |
| `switch_action` | Newmota extension: each detached `SetOption114` switch Action, which Tasmota shows only to Rules | Same packing as `any_key`, key 1 |
| `button_pressed` | Debounced press or release | `count<<24 \| level<<16 \| last_level<<8 \| index`, level 0 pressed, zero-based index |
| `button_multi_pressed` | Completed one to five presses; not a `SetOption13` immediate press | `count<<8 \| index` |
| `set_power_handler` | Each Power command, interlock step or group write, changed or not, as Tasmota's SetDevicePower | Bit mask of all Power states after it, bit 0 Power1 |
| `network_up`, `network_down` | Station or Ethernet address gained or lost | 0 |
| `audio` | With `USE_I2S`: an I2S direction starts, changes its rate or stops. `cmd` `"rate"` comes first on a start or rate change, then `"power"`; `payload` is `"tx"` or `"rx"` | The sample rate for `rate`; 1 on start, 0 on stop for `power` |

As in Tasmota, a method that returns true ends that event's dispatch: the drivers after it do
not see it. A driver removed during dispatch shifts the later ones, so the next one misses that
event, also as in Tasmota. Unlike Tasmota, the hooks only observe: native button, switch and
Power actions always happen. Detach inputs with `SetOption73 1` (buttons) or `SetOption114 1`
(switches) to handle them in Berry alone. Hooks run in the Berry poll after the event, never
inside it, so a command a hook issues produces its own events in a later poll. A Light change
without a Power command, such as `Dimmer 0`, also calls `set_power_handler` once the Power mask
changes. Up to 40 input and Power events wait for the poll, enough for `Power0` on 32 outputs;
more are dropped with a log warning. Unlike Tasmota, a tick method (`every_50ms` to
`every_second`) assigned after `add_driver()` starts within a second, so drivers without tick
methods do not run the Berry poll on every tick.
An exception in one driver is logged and does not stop the others. A hook that runs into the
4 s Berry deadline is aborted and ends the poll; the drivers after it miss that event. Sensor
values, schedules and MQTT topics use the existing APIs; there is no Rules engine and no trigger
syntax.

`load(name)`, also `newmota.load(name)`, compiles and runs a `.be` file from the filesystem, as
Tasmota's `load()`. A missing leading `/` and `.be` are added. It returns true, or false when the
file is missing or fails, and logs the failure. Berry compiles a whole file before running it, so
`autoexec.be` loads drivers first and the script using their classes last. Tasmota's `.bec` and
`.tapp` files are not supported.

`newmota.add_cmd(name, f)` adds a command, as Tasmota's `tasmota.add_cmd()`. It runs only when
no native command has the name, from any source (console, web, MQTT, `newmota.cmd()`), and
matches the name without regard to case and without a trailing index. Newmota calls
`f(name, idx, payload, payload_json)` with the name as registered, the index (1 without one),
the payload string (empty without one) and `json.load(payload)`, nil when it is not JSON. `f`
must be a function or closure; a method raises, as in Tasmota. Adding a name again replaces its
function; `newmota.remove_cmd(name)` removes the exact name. The function answers with one of
`newmota.resp_cmnd_done()`, `resp_cmnd_error()`, `resp_cmnd_failed()` (`{"<Name>":"Done"}`,
`"Error"`, `"Failed"`), `resp_cmnd_str(text)` (`{"<Name>":"<text>"}`) or `resp_cmnd(json)` (the
JSON as given); the last call wins. Without one the command answers nothing. A command that
raises is logged and still answers with what it set before. A response longer than 1024 bytes
fails the command. `Driver.add_cmd(name, f)` registers `f(self, name, idx, payload,
payload_json)` for that driver.

```berry
class Follow
  # Power2 follows Power1.
  def set_power_handler(cmd, idx)
    var want = (idx & 1) != 0
    if want != ((idx & 2) != 0)
      newmota.cmd(want ? "Power2 ON" : "Power2 OFF")
    end
  end
  # With SetOption114 1, Switch1 toggles Power3 (state 2 is TOGGLE).
  def switch_action(cmd, idx)
    if (idx & 0xFF) == 1 && ((idx >> 8) & 0xFF) == 2
      newmota.cmd("Power3 TOGGLE")
    end
  end
end
newmota.add_driver(Follow())
```

```berry
var d = Driver()
d.every_second = def (self) log("tick") end
d.add_cmd("Greet", def (self, cmd, idx, payload) newmota.resp_cmnd_str("Hello " + payload) end)
newmota.add_driver(d)
# Greet World  ->  {"Greet":"Hello World"}
```

`newmota.read_sensors()` returns the MQTT SENSOR document as a string, as Tasmota's
`tasmota.read_sensors()` does, for `json.load()`. It holds the switch states, sensors and
energy fields with StatusSNS units and precision, without Tasmota's `Time`. It returns nil when
there is nothing to report or the document exceeds the 1024-byte MQTT payload. Tasmota's
optional display argument is accepted and ignored. A thermostat with hysteresis:

```berry
import json
class Thermostat
  var target, band, heating
  def init() self.target = 21.0 self.band = 0.5 self.heating = nil end
  def every_second()
    var raw = newmota.read_sensors()
    var s = raw != nil ? json.load(raw) : nil
    var t = s != nil && s.find("DS18B20") != nil ? s["DS18B20"].find("Temperature") : nil
    if t == nil return end
    var heat = self.heating
    if t < self.target - self.band heat = true end
    if t > self.target + self.band heat = false end
    if heat != nil && heat != self.heating     # switch only on a change
      self.heating = heat
      newmota.cmd(heat ? "Power1 ON" : "Power1 OFF")
    end
  end
end
newmota.add_driver(Thermostat())
```

### BLE sensors

`USE_BLE_SENSORS` decodes Xiaomi MiBeacon (including encrypted v3 and v5 frames), BTHome v2,
Qingping CGD1 and Mi Body Composition Scale advertisements while BLE runs (`SetOption115 1`).
Supported types are Flora, MJ_HT_V1, LYWSD02, LYWSD03, CGG1, CGD1, NLIGHT, MJYD2S, YLYK01,
MHOC401, MHOC303, MCCGQ02, SJWS01L, YLKG07/08, YLAI003, BTHome and MIBCS. The ATC and PVVX custom formats are not decoded;
switch those thermometers to BTHome in their firmware settings.

BTHome v2 decodes temperature, humidity, battery, illuminance, moisture, conductivity, mass,
pressure, voltage, current, power, energy, CO₂, TVOC, PM2.5, PM10, distance, counts, UV index,
the binary sensors (`Opening`, `Door`, `Motion`, `Window`, `Smoke` and the others, as 0/1) and
`Button<n>` and `Dimmer<n>` events; other objects are skipped. A second object of the same
kind gets a number, such as `Temperature2` or `Voltage2`, also when the two use different
encodings. Values without a device field share a pool of 32; when it is full, the least
recently updated value makes room, and a log line reports it once. Encrypted BTHome needs the device's key from `MI32Key`
or `/mi32cfg`; a packet whose counter does not increase is ignored, and once a device has a
key its plaintext packets are ignored. A packet the device sends because of a trigger, and
every button or dimmer event, publishes an event message.

Up to 32 devices are kept, and the dashboard shows all of them. At most 8 reach StatusSNS and
MQTT SENSOR: configured devices first, then the most recently seen, and a log line counts the
rest. Devices that would not fit the 1024-byte message are left out as well, with their own log
line, so local sensors and energy values always keep their place. A
device unseen for one hour is not shown, and each value is omitted one hour after its last
update. Each device reports its decoded fields plus `RSSI`, `Time` (local time of its last
packet in seconds, omitted until the clock is set) and `MAC`; `LastSeen` (seconds) appears only
in the dashboard. Event messages carry `RSSI` and `MAC` without `Time`. Remote temperatures never become the global temperature.

The configuration commands keep their Tasmota names and the `/mi32cfg` file format, so a
Tasmota file loads unchanged; ATC and PVVX entries are skipped with a log line:

| Command | Effect |
| --- | --- |
| `MI32Key <key><MAC>` | Sets a 16-byte bind key (44 hex digits; the 36-digit YLKG07/08 form is accepted). |
| `MI32Name<n> <text>` | Names device slot `n` (0-based; default 1); an empty text clears it. |
| `MI32Cfg` | Writes all devices, keys and names to `/mi32cfg`. |
| `MI32Option0` | Always aggregate every value in event messages (default 1). |
| `MI32Option1` | Keep BLE devices out of StatusSNS and MQTT SENSOR; the dashboard still shows them. |
| `MI32Option2` | Direct bridge: publish an event message for every new value or sighting. |
| `MI32Option3` | Stop admitting new devices; set automatically when `/mi32cfg` has entries. |
| `MI32Option5` | Generic sightings: any public address, or a random address resolved by an IRK stored as the key of a `PID 0000` entry. Set automatically by such an entry. |

Options last until reboot. Active scanning (`BLEActive 1`) is needed to receive device names.
Events are published immediately on `tele/<topic>/SENSOR` as `{"<device>":{...}}`, one
device per message, like Tasmota's triggered telemetry but without `Time`, matching the
periodic SENSOR message. Buttons (`Button<n>`), knob turns (`Dimmer`, `Dimmer_P`), holds
(`Hold`), motion and its end (`Motion`, `Events`), door changes, leaks, no-motion time and
the scale's stable weight, impedance or `ImpedanceFailed` trigger a message; with
`MI32Option2` every new value or sighting does. A message carries the changed fields, or all held values with `MI32Option0`, plus
`RSSI` and `MAC`; a sighting adds its raw `Payload`. A message the MQTT queue cannot take is
retried, a sighting with its next report, and dropped after 10 seconds.

The dashboard draws the last 24 hours of every reading on its card, in the card's units and
with its offsets, also for samples taken before a setting changed. The history is kept in RAM
and starts empty after a restart. By default each series holds 24 hourly averages, about
48 bytes per series. `SetOption177 1` keeps 5-minute averages instead, about 576 bytes per
series, and takes effect after a restart; if that memory is not available at boot, the
history falls back to hourly with a log line. Up to 32 series are kept; a series without
samples for a day makes room for a new one. Ages (`LastSeen`, `NMT`) are kept as presence:
1 for a period in which the age was below one minute, meaning the device was seen or motion
occurred, so a BLE device's visibility over the day shows at a glance. Dew point is derived
and not kept.

`SensorDriver` maps compiled stable IDs to driver names. `SensorDriver <name>` reports the
measurement fields, output routes, and enabled state; for example, `SensorDriver BH1750`,
`SensorDriver TC74`, `SensorDriver BME280`, `SensorDriver DHT`, or `SensorDriver ENERGY`, which
is the energy meter's Home card. Append `0` to disable a
driver or `1` to enable and probe again. Changes are saved before they take effect.
Disabled drivers clear current samples and perform no probing or polling, including after
restart. The shared I2C buses remain available for other drivers and Berry.
To add a sensor, register its source, never-reused ID, display name, output fields and route
surfaces in `tools/newmota/features.py`; the generator composes the service table from that
metadata. Each driver supplies `reset`, `initialize`, `poll` and `visit_readings` hooks, so
the shared service does not need a per-driver edit. A driver with events (a button, a motion edge)
calls `sensor_service().publish_event()` with numeric readings and preformatted JSON members;
the service formats the readings like StatusSNS and publishes `{"<device>":{...}}` on
`tele/<topic>/SENSOR`. It returns false when MQTT cannot take the message, so the driver may
retry. Keep display names to 16 ASCII
characters or fewer so the complete command listing stays bounded. Sensor names in
readings must stay below 32 bytes (`kSensorNameCapacity`); a longer name fails the whole
output. The canonical sensor snapshot is limited to 1024 payload bytes. If serialization
exceeds that capacity, publication is skipped and a warning is logged; the previous snapshot
remains available to consumers.

Optional `USE_WS2812` provides a configured RGB/RGBW Light with up to 512 pixels,
non-network strip schemes 5–14, pixel/configuration commands, and native waveform
adapters on ESP-IDF, Pico SDK, BL602, and AmebaD. The configured strip shares the
Light owner across commands, power, MQTT, browser and Berry. The Berry `led_strip`
RGB class can also open standalone strips on unassigned pins or borrow the
configured Light. See the [strip API and native routes](drivers/led_strip/README.md)
for ownership, wire formats, nonblocking transfers, and the Beken restriction.

Optional LED driver chips such as `USE_BP5758D` drive Tuya RGBCW bulbs as a five-channel
Light on every family, with `LightCurrent` for the chip's RGB and white currents. See
[LED driver chips](drivers/led_driver/README.md).

Optional `USE_TUYA_MCU` drives Tuya devices whose MCU switches the relays, over the shared serial
owner: Tasmota's TuyaMCU v2 protocol, MCU relays as Power endpoints, MCU switches, the MCU
light below the Light service, enums, `TuyaMCU`, `TuyaSend<x>` and `TuyaReceived`.
`USE_TUYA_MCU_SENSORS` adds the MCU's sensors as `TuyaSNS` on the sensor service and
`USE_TUYA_MCU_ENERGY` its power, voltage, current and total as the energy meter. See
[TuyaMCU](drivers/tuya/README.md).

Optional `USE_MODBUS_RTU` provides shared physical UART/RS-485 buses with queued
address clients, bounded frames, CRC validation and poll-driven timeout/retry handling.
`ModbusBusN` durably configures a physical bus, restored before energy devices at boot. See the [RTU contract and timing limits](drivers/modbus/README.md).

`USE_SERIAL` selects hardware UART ports and the global Berry `serial` class.
Construct `serial(rx, tx, baud[, mode, inverted])` with physical GPIO numbers; `-1`
omits a direction. The portable baud input range is 1–4,000,000; native adapters may
reject a narrower limit (Beken accepts at most 3,250,000), and SDK clock/divider selection
can quantize the resulting baud. Qualify the actual rate on the selected board.
Pads need no Template assignment, but configured pads and console
resources are reserved. Script GPIO and serial share exclusive runtime pad claims.
The class exposes all 24 Tasmota `SERIAL_5N1` through `SERIAL_8O2` constants. Unsupported
native framing, inversion, or routes raise an error before configuration.

`write(integer_or_bytes)` returns the number accepted during a bounded 10 ms attempt;
retry any remaining bytes later. `read([0..256])` returns currently captured bytes without
waiting; `available()` reports the staged count. Poll with `set_timer()` or other existing
cooperative script facilities. `flush()` drains TX before clearing RX, and raises a timeout
if TX is still busy after 25 ms. `close()` is idempotent, uses the same bounded drain, and
retains ownership on timeout. Closed methods return nil. Collected instances defer port cleanup until TX is idle. Open handles are closed when the
VM is torn down; uncertain native cleanup retains pad reservations until restart.

`config_tx_en(pin)` selects an initially LOW RS-485 enable pad; `-1` releases it after TX
is idle. Select another pad after clearing the previous one. DE stays HIGH through FIFO
and transmitter drain, and the application loop lowers it once the last stop bit is sent.
Cooperative script execution can delay that lowering, so protocols requiring immediate
turnaround need hardware qualification. `status()` returns cumulative `dropped` bytes and
`errors` events. RX uses bounded native buffering; IRQ rings default to256 bytes and drop
new arrivals when full. Device owners may explicitly request up to1536 bytes;
Ordinary Berry UART defaults remain unchanged. ESP-IDF sizes its SDK RX buffer to at least256 bytes and reports
SDK overflow/framing/parity events in `errors` (the exact lost-byte count is unavailable).
Hardware operation and RS-485 turnaround remain unqualified.

## Boundaries

Services own their data and command handlers. Serial, web, MQTT, Berry, and future transports
submit through the same command implementation. A service may request a platform operation
through a narrow interface, but it does not reach into an SDK adapter or assume a particular
task, queue, allocator, pin type, or filesystem API.
