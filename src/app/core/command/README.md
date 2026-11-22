# Command system

All current command callers use the same portable implementation: serial console, browser
console, MQTT, Berry, and internal services submit a bounded request to the application
command processor. Transport code handles framing and authentication; command handlers do
not contain serial, HTTP, or MQTT code.

## Current behavior

- Names are case-insensitive and may carry a decimal index, such as `Power1`.
- Queries use an empty payload or `?` where the command supports querying.
- `ON`, `OFF`, `TOGGLE`, `TRUE`, and `FALSE` are accepted where the command expects a state.
- A line contains at most 16 synchronous commands separated by semicolons.
- The complete chain is validated before the first handler runs. A reboot-producing command
  must be last.
- A leading `_` suppresses external delivery of that command's response; Berry `newmota.cmd()`
  still returns its logical result.
- Responses are bounded and routed back to the original caller; oversized output reports a
  result instead of being silently truncated.

For example:

```text
Module 0; GPIO1 224; Restart 1
```

The current command surface includes status, restart, reset, Wi-Fi settings and scan/test,
filesystem operations, GPIO/Module/Template configuration, basic relay/button control,
power settings, hardware PWM and PWM Light commands, Berry, and MQTT settings. The exact compatibility boundary is recorded in
[Tasmota differences](../../../../TASMOTA_DIFFERENCES.md).

Application images expose `Time`, `Timezone`, `TimeStd`, and `TimeDst`. Images with NTP also
expose `NtpServer1`–`NtpServer3`. `NtpServer` always queries all three addresses and ignores an
unindexed payload; an indexed query returns just that slot. Set an
indexed slot to a DNS hostname or dotted IPv4 address of at most 63 ASCII characters.
Commas become dots, so `NtpServer1 192,0,2,123` stores `192.0.2.123`. URLs, ports, IPv6,
whitespace within an address, trailing dots, and malformed hostnames are rejected.
`NtpServerN 0` clears a slot; `NtpServerN 1` restores its default. The three defaults are `2.pool.ntp.org`,
`2.europe.pool.ntp.org`, and `2.nl.pool.ntp.org`. Writes must persist successfully before
they apply, and Safeboot preserves these settings without running NTP or exposing the commands.
An accepted indexed write, including an unchanged value, cancels the pending exchange and
retries from the first enabled slot.
Empty slots are skipped; clearing all three disables NTP without clearing the current clock.
There is no gateway or randomized fallback. An epoch set with `Time` keeps its manual hold
across server changes; `Time 0` releases that hold and requests synchronization. NTP retains
its five-second exchange timeout, one-minute failure retry, hourly refresh, and correlated
reply checks.

`Status 7` includes `StatusTIM.Source` (`none`, `manual`, `ntp`, `gps`, or `rtc`) and
`Valid`. Source records the origin of the latest accepted sample, independently of
network connectivity. Optional RTC chip features add `StatusTIM.RTC` diagnostics and can
bootstrap an invalid clock. Accepted NTP samples update the chip when needed; manual
epochs do not. `Time 0` retains the current time while releasing its manual hold.
See [external RTC behavior and diagnostics](../../drivers/rtc/README.md).
Optional `USE_GPS` adds UBX UTC and retained `StatusTIM.GPS` navigation diagnostics.
Fresh GPS takes priority over NTP; manual hold blocks both. Accepted GPS can also
seed the RTC. See [GPS configuration and time policy](../../drivers/gps/README.md).

Application images with the Time service expose `Timers` and `Timer1`–`Timer16`, plus the
`Latitude`, `Longitude`, and `Sunrise` solar settings. `Timers ON|OFF|TOGGLE` enables or
disables scheduling; its query returns only the global state. Each `TimerN` query returns one
complete record. Set a timer with a partial JSON object, clear it with `TimerN 0`, or copy
another record with `TimerN <source-number>`. Timer JSON uses Tasmota fields: `Enable`,
`Mode`, `Time`, `Window`, `Days`, `Repeat`, `Output`, and `Action`. `Latitude` and `Longitude`
take decimal degrees; `Sunrise` selects one of the four solar twilight thresholds. `Days` is
a seven-character Sunday-first mask; `0` and `-` disable a day, and any other character
enables it. Solar `Time` values are signed offsets up to 11:59 from sunrise or sunset.

Actions 0, 1, and 2 apply off, on, and toggle to the selected active Power endpoint. Action 3
queues a Berry callback and does not change or blink an output. Register one callback with
`newmota.add_timer_handler(fn)`; it receives `(timer, output, action, scheduled_local_epoch)`.
Without a handler, the event is consumed without invoking a script. Rules are not part of
Newmota.

## Sleep

`Sleep 0..250` (default 50) is saved and applies at once, with Tasmota's dynamic-sleep meaning:
an idle device makes one main-loop pass per `Sleep` milliseconds. Values above 50 are kept for
compatibility but act as 50, preserving the relay-timer cadence; button input filtering
continues every 10 ms during the idle wait. The reply's
`Active` field shows the applied value, for example `{"Sleep":{"250":{"Active":"50"}}}`. While it waits, console input
and web requests are still served every millisecond and end the wait, so the web UI stays
responsive. The loop keeps its 1 ms pass while a light fades, an addressable LED strip runs, a
serial port is open, BLE is active, a firmware, `wget` or browser file transfer runs, or a
restart is pending; builds with I2S audio, edge capture or a display always keep it. Safeboot
never sleeps and has neither the command nor the sleep fields. `SetOption60 1` selects Tasmota's normal sleep: the full wait follows each pass
instead of completing the period that began with it. `STATE` and `StatusSTS` report
`"SleepMode":"Dynamic"` or `"Normal"`, the applied `Sleep`, and `LoadAvg`, the share of each
Sleep period spent working in percent, averaged over about one second as Tasmota computes it but
kept in thousandths, so passes shorter than the rounding step still count.

Wi-Fi follows Tasmota's rule: `Sleep 0` or `SetOption127 1` turn power saving off, any other
value selects modem sleep. ESP-IDF builds with Bluetooth keep modem sleep, which coexistence
requires. Pico 2 W keeps power saving off at any value, because its power-save mode delays
requests by seconds. Beken, Bouffalo and AmebaD keep their SDK defaults.

## Deep sleep

With `USE_DEEPSLEEP`, `DeepSleepTime 0` or `11..316223999` seconds (just under ten years) is saved in
`/.newmota/deepsleep.bin`; other values only report the current one. A nonzero value forces
`TelePeriod 300` if telemetry was off. As in Tasmota, the device counts down four seconds after
each TelePeriod, 64 after a start that was not a deep-sleep wake (so a freshly powered device
stays reachable), and then sleeps until the next whole multiple of `DeepSleepTime` on the local
clock. Fifteen seconds without a network address also start the countdown. Before it sleeps it
publishes the retained `tele/<topic>/DeepSleep`
`{"DeepSleep":{"Time":"<wake>","DeepSleep":<now>,"Wakeup":<wake epoch>}}`, saves the same state as
a restart and closes MQTT. A `DeepSleep` pin held low keeps the device awake. While deep sleep is
on, the boot count only grows with `SetOption76 1`.

Where the platform keeps memory through deep sleep (ESP except the C2, AmebaD, Pico), the
device learns the timer slip between cycles and sleeps at most an hour at a time, going straight
back to sleep while time is left. ESP32-C2, Beken and Bouffalo keep nothing: each wait is one
sleep, up to the chip's limit, and starts from the clock. The `Wakeup` rule of Tasmota's Timers is
not available.

With `USE_BERRY_ULP` on ESP chips with a ULP coprocessor, Berry's `import ULP` provides Tasmota's
`run([entry])`, `load(bytes)`, `set_mem(pos, value)`, `get_mem(pos)`, `gpio_init(pin, mode)`,
`wake_period(index, us)`, `adc_config(channel, attenuation, width)` and `sleep([seconds])`.
The ESP32 runs the ULP-FSM, the S2 and S3 the ULP-RISC-V, and the C5, C6, P4 and S31 the LP core.
`ULP.sleep` closes MQTT and powers down at once; the ULP, or the timer if given, wakes the chip.

## WifiPower

`WifiPower` follows Tasmota on ESP-IDF builds and replies like `{"WifiPower":16.0}`; `Status 5`
reports the same field. The value is saved and applies within 250 ms.

| Payload | Effect |
| --- | --- |
| none | Reports the setting, or the last applied power in dynamic mode |
| `0` | Dynamic: follows the station's RSSI; without a link it uses the chip default |
| `1` | Chip default (the factory setting): 15 dBm on S2, 19 dBm on C2, C3 and S3, 16 dBm otherwise |
| `0.1` and up | Fixed power in dBm, capped at the chip's maximum |

The SDK supports levels between 2 and 20 dBm and rounds a request down to the nearest, so the
real output can be below the reported value. Beken, Bouffalo, AmebaD and Pico builds have no
`WifiPower` command and keep their SDK's power.

## Status LEDs

Discrete LEDs (`Led1`–`Led4`, normal or inverted) and the optional `LedLink` follow Tasmota's
rules. `LedState 0..7` (default 1) selects what they show: bit 0 the Power state through
`LedMask` (default `0xFFFF`), bit 1 received MQTT messages, and bit 2 published MQTT messages.
The link LED, or `Led1` when no `LedLink` is configured, blinks every second while Wi-Fi is
down and every two seconds while enabled MQTT is not connected, and stays lit during a restart
or update. `SetOption31 1` stops those problem blinks. Without `LedLink`, `Led2` (or `Led1`
alone) shows Power; with `LedLink`, `LedN` mirrors `PowerN`. `LedPower<n> 0|1|2` sets an LED
manually and disables Power indication; without `LedLink` only `LedPower1` exists. `LedMask`
accepts decimal or `0x` hexadecimal. `LedPwmMode<n> 0|1|2` drives an LED through PWM instead
of on/off, using `LedPwmOn` and `LedPwmOff` (0–255, default 255 and 0) through the Light gamma
curve; without `LedLink` only `LedPwmMode1` exists. A PWM LED shares the PWM carrier with Light
outputs (`PWMFrequency`); a pad without a PWM route stays on/off. `LedState`, `LedMask`, and the
three PWM values persist.

## Global temperature input

Optional `USE_GLOBAL_TEMPERATURE_COMMANDS` adds `GlobalTemp` query/manual input
and durable `GlobalTemp2 0..250` automatic-field selection. Input units follow SetOption8,
but the response is always Celsius at one decimal (null when unavailable).
Manual override is volatile and does not rewrite any physical sensor sample.
Zero restores ordinary raw automatic observation; nonzero selects an ordinal
reported temperature (first-field fallback when absent). A valid setter clears
manual input, including when the retained index is unchanged. Queries do not. See the [energy guide](../../drivers/energy/README.md#readings-accounting-and-protection)
for protection behavior.

## Energy protection outputs

Optional `USE_ENERGY_PROTECTION_BINDINGS` routes energy protection through the ordinary Power
outputs, as Tasmota does: a group operation addresses every configured Power output and phase
N uses Power N. There is no separate target command. Optional `USE_ENERGY_MAX_POWER` and
`USE_ENERGY_MAX_ENERGY` add the MaxPower and MaxEnergy policies. See the
[energy guide](../../drivers/energy/README.md#readings-accounting-and-protection).

## Status

`Status` through `Status 11` report the fields Newmota has a backing value for, in Tasmota
order; a field the platform cannot report is left out, not zeroed. `RestartReason` is one of
`Power On`, `Software`, `Watchdog`, `Panic`, `Brownout`, `Deep Sleep` or `External`. `Status 6` without MQTT or in Safeboot,
`Status 9` without an energy provider, and `Status 12` and `Status 13` return
`{"Command":"Invalid arguments"}` and are left out of `Status 0`. Over MQTT each section uses
Tasmota's topic, such as `STATUS5`, with `Status 8` on `STATUS8` and `Status 10` on `STATUS10`.
`Status 0` and `Status0` publish their sections separately over MQTT, paced by the response
queue, and return one combined object on the console, HTTP and Berry. `_Status 0` publishes
nothing, and a request during a publication runs once after it, on the topics of the newest
request. Safeboot stays combined on `STATUS0`, and its always empty `StatusSNS` uses `STATUS10`. A single section above the 1024-byte MQTT limit returns `{"Command":"Failed"}`
there. See the [Tasmota differences](../../../../TASMOTA_DIFFERENCES.md).

## Boot-loop fallback

Like Tasmota, the application counts consecutive quick restarts: boots that end in a
restart, watchdog, or panic before they have run for 10 seconds. Power-on, brownout, and
deep-sleep wakes start the count again. From the fourth quick restart, that
boot skips the BLE start (SetOption115), Matter (SetOption151), LVGL autostart
(SetOption135), and `autoexec.be`. From the fifth, it also ignores the user GPIO
assignments made with `GPIO` (the module and template pins still apply), and `GPIO` keeps
reporting the saved ones. Nothing is written to the settings: the next boot that is not a
boot loop starts everything again. From the tenth, the application clears the count and
restarts into its paired Safeboot before it mounts the filesystem; `Restart 3` there returns
to the application. Without a paired Safeboot it logs the fact and continues the reduced
boot. `SetOption115 1` and `lv.start()` still start BLE
and LVGL by hand. A reduced boot logs an error, adds `"BootLoop":<count>` to `Status 1`, and
shows the count on the web Information page.

The count is kept on ESP chips with RTC memory and on Pico. ESP32-C2, Beken, Bouffalo, and
AmebaD have no retained count, so they never reduce a boot and always skip `autoexec.be`.

## Energy Status 9

Optional `USE_ENERGY_STATUS9` reports primary retained PowerDelta and the six
P/V/I threshold vectors in `Status 9`, indexed `Status9`, and aggregate `Status 0`.
One phase is scalar; multiple phases are arrays irrespective of SO129. The five
MaxPower/MaxEnergy fields appear only with their selected protection owners; this query does
not provide cutoff behavior. See the [energy guide](../../drivers/energy/README.md#primary-status-9-reporting-settings).

## State words

Optional `USE_STATE_TEXT` adds durable `StateText1` (OFF), `StateText2` (ON),
`StateText3` (TOGGLE) and `StateText4` (HOLD) aliases. Query `StateText` for all four;
indexed queries return one. Indexed setters replace spaces with underscores.
Aliases accept 1..31 bytes without control characters; oversized/invalid requests
are rejected without truncation. Empty payloads query. A failed save retains old
values. Settings retain aliases through feature exclusion and Safeboot saves;
excluded builds use default words and omit the command.

Power responses, canonical STATE and MQTT Power publications and Energy MARGINS
use escaped aliases. Numeric/standard ON/OFF/TOGGLE input parsing stays unchanged;
aliases are presentation text. Other subsystem command responses retain their
existing state words. Identical ON/OFF aliases are permitted; MQTT uses typed
Power state, not text comparison, in the application binding.

## Transport entry points

The physical serial console is the reference command path. The browser uses authenticated
`POST /api/command`; MQTT uses the bounded `cmnd/<topic>/#` subscription and publishes
results under `stat/<topic>/RESULT` by default. `FullTopic`, `Prefix1–3`, `GroupTopic1–4`,
and result routing options configure the MQTT paths. All of these paths call the same handlers.

Application images start each console result line with the `HH:MM:SS.mmm ` log timestamp, on
serial and in the Web Console. MQTT and `POST /api/command` results stay plain JSON.

MQTT also exposes `Publish`/`Publish2`, retain controls, broker timing settings, and
Tasmota aliases such as `Topic`.

Wi-Fi application images provide asynchronous IPv4 ICMP `Ping <address>`. The destination
is one IPv4 literal or DNS hostname (up to 127 bytes); URLs, ports, IPv6, whitespace, and
ambiguous numeric address spellings are rejected. `Ping` sends one echo, `Ping1` through
`Ping8` select the attempt count, and `Ping0` tries up to four times, stopping at the first
reply. Larger indices are rejected. `Ping` or `Ping ?` without a destination queries the
current or last state and counters.

An accepted campaign immediately returns `{"Ping":"Done"}`. Its later JSON result names the
destination and reports `Reachable`, `IP`, `Success`, `Timeout`, `MinTime`, `MaxTime`,
`AvgTime` (integer milliseconds), and `Error`. Resolution and transport failures use the
same numeric counter fields; `Error` explains the failure. The result goes to the serial
console, browser console history, and shared response router for MQTT. Browser polling
receives the complete result in bounded history entries. MQTT uses
`RESULT` by default or `PING` with command-topic response routing enabled. Each delivery is
retained until its surface accepts it.

Only one campaign runs at a time. Another start reports `Busy` while probes, cancelled DNS
callbacks, or result deliveries remain pending. DNS expires after five seconds. Echoes have
a 32-byte payload, a one-second reply deadline, and at least one second between sends.
Only one echo is outstanding; delayed application polling does not queue missed probes.
Network loss and pending restarts cancel the campaign. Ping uses its own ICMP socket and
can run alongside NTP, MQTT, and `wget`. Safeboot omits Ping. Native builds and host protocol
tests do not establish hardware reachability qualification.

The filesystem commands are `df`, `ls`, `rm`, and `mv`. Wi-Fi application images also
provide `wget http://host[:port]/path/file`: exactly one URL, with no destination argument
or indexed form for starts. `wget` or `wget ?` reports the current or last state, destination,
received/total byte counts, and error. It downloads asynchronously to `/file`, using the URL
path's last component without the query, or `/index.html` for an empty component. Percent
escapes in the name remain literal. The command returns `{"wget":"Started"}`; console logs
report completion with the saved path and byte count, or the failure reason.

`wget` requires a direct HTTP 200 response with `Content-Length`. HTTPS, redirects, URL
fragments, IPv6 literals, URL user information, chunked transfer, and compressed bodies are
unavailable. URLs must fit the bounded host/path buffers (127/319 bytes), filenames fit 190 bytes, and response headers
fit 1024 bytes. Transfers expire after 30 seconds without progress or five minutes overall.
The server must close the response connection after exactly the declared body length.
A staging file is synchronized and atomically renamed on success; failed downloads preserve
the previous destination. Space is needed for both the existing file and its replacement.
MQTT pauses while the transfer owns the shared TCP stream and reconnects afterward. Browser
filesystem transfers and firmware updates are excluded, as are `rm` and `mv` while downloading.
A pending restart rejects or aborts the download. Connection cleanup retains exclusion until
the native stream closes, with a five-second cleanup deadline. A cleanup timeout is reported
and releases the service reservation; the native stream continues rejecting opens until it is
idle. Safeboot does not expose `wget`.

With `USE_SDCARD`, `ls`, `rm` and `mv` reach a mounted SD card under `/sd`, and `ls /`
shows `sd` while it is mounted. `mv` keeps both paths on one volume; moving between flash
and the card fails with `Invalid path`. A card rename replaces an existing destination file
like LittleFS does, but not atomically. `df` adds an `"SD"` object with the card's
`Mounted`, `Total`, `Used` and `Free` bytes; without a card it reports
`"SD":{"Mounted":false}`. `wget` always saves to flash.

Newmota uses synchronous command chains in place of Tasmota's deferred `Backlog`
queue. Tasmota's web `GET /cm` route is not part of the interface.

Safeboot has a smaller command table and excludes `ButtonDebounce`, `ButtonTopic`,
`ButtonRetain`, `SwitchMode`, `SwitchDebounce`, `SwitchTopic`, `SwitchRetain`, `SwitchText`,
physical button and switch handling,
normal application hardware configuration
and Berry.

## Defer

Application images expose `Defer <ms> <command>`. It runs the command line later from the main
loop, once `ms` (0 to 3 600 000, one hour) has passed since the `Defer` command ran:

```text
Power1 ON; Defer 2000 Power1 OFF; Defer 4000 Power1 ON
```

- The deferred line may be at most 128 bytes. A chain inside it needs escaped semicolons:
  `Defer 5000 Power1 OFF\; Power2 OFF`.
- Only its syntax is checked when it is scheduled, with the full chain rules. It runs with the
  source of the `Defer` command (serial, web, MQTT, Berry), so commands limited to one source
  behave the same, and a command can still refuse its payload then. On an MQTT group topic, the
  deferred line must pass the same group-topic check as a direct command.
- It answers at once with `{"Defer":{"Ms":2000,"Pending":1}}`. When it runs, the deferred line
  answers on the serial console, in the web console history and on `stat/<topic>/RESULT`, also
  when it came from MQTT. A deferred `Status 0` from MQTT answers section by section on MQTT,
  as a direct one does, and not on the serial console.
- Eight lines can wait at the same time; a ninth answers `{"Defer":"Full"}`. One line runs per
  main-loop pass: the earliest due first, lines with the same due time in the order they were
  scheduled. Timing follows the loop, so a line can be late by up to the `Sleep` period.
- `Defer` and `Defer ?` answer `{"Defer":{"Pending":<n>}}`. `Defer Cancel` cancels every
  waiting line and answers `{"Defer":{"Cancelled":<n>}}`.
- Waiting lines live in RAM. A requested restart or deep sleep drops them, and while one is
  pending `Defer` answers `{"Defer":"Restarting"}`. Nothing is kept across a restart. For
  outputs that must switch off even after a restart or power loss, use `PulseTime`.

## Home Assistant discovery

Images with `USE_HA_DISCOVERY` and MQTT publish Home Assistant's MQTT discovery. Home Assistant's
MQTT integration (and openHAB or Domoticz) then finds the device without further setup.

- One retained message per entity on `homeassistant/<platform>/<MAC>/<id>/config`, where `<MAC>`
  is the 12-digit station MAC. Entities: one `switch` per relay, one `sensor` per sensor
  reading (energy phases as array elements of `ENERGY`), the diagnostic sensors RSSI, Signal,
  Uptime and IP address, and a Restart `button`. Lights, switches and buttons follow later.
- State comes from `tele/<topic>/STATE` and `tele/<topic>/SENSOR`; commands go to the normal
  `cmnd/<topic>/...` topics. Availability follows the LWT.
- The device shows `DeviceName` (or the host name), model = module or template name,
  manufacturer Newmota, the firmware version, the MAC and a link to the web interface. A device
  with a single power output names its entity after the device; otherwise "Relay <n>". Rename
  entities in Home Assistant.
- Messages go out 2–5 seconds after each MQTT connect and again within about two seconds when
  something they depend on changes: `DeviceName`, the topics, `SetOption19`, the state texts,
  the relays or the set of sensor readings. They are paced through the MQTT queue.
- The published entity ids are kept in `/.newmota/ha_discovery.txt`. An entity that is gone
  (a sensor removed, fewer relays) gets an empty retained message, which removes it from Home
  Assistant together with its user settings. `SetOption19 1` removes every entity this way;
  `SetOption19 0`, the default, publishes them again.
- `SetOption104` (no retain) also drops the retain flag of discovery messages, so Home Assistant
  then only sees them while it is running.

## I2C diagnostics

Images selecting `USE_I2C` expose `I2CScan`. Configure complete SCL/SDA pin pairs with
GPIO/Template, restart to activate them, and provide external pull-ups. The command scans
the two shared logical buses in order, skipping inactive buses. As in Tasmota, numeric
suffixes and payloads are ignored: `I2CScan`, `I2CScan0`, `I2CScan1`, and `I2CScan ?`
all perform the same scan.

Newmota probes nonreserved addresses `0x08` through `0x77`; Tasmota probes `0x01` through
`0x7f`. Each address probe uses the shared platform's default 25 ms timeout. NACKs continue
the scan. Any other error stops that bus while the next active bus still runs. Timeout is
reported as error code 5; other failures use code 4 and retain the failing address. A bus
error replaces any earlier discoveries on that bus and is a completed diagnostic result.

A single active bus returns one object, for example
`{"I2CScan":"Device(s) found on bus1 at 0x23 0x48"}`,
`{"I2CScan":"No devices found on bus1"}`, or
`{"I2CScan":"Error 5 at 0x08 on bus1"}`. Two active buses return their objects in bus order
inside `{"Results":[{...},{...}]}` through the normal response router. With neither bus
active, the result is `{"I2CScan":"No enabled buses"}`. The complete result is sized before
delivery; exceeding the shared 1024-byte MQTT response capacity returns
`{"I2CScan":"Response exceeded limit"}` without partial scan JSON and marks the command
failed.

I2CScan host, native build, and hardware validation remain open.

## Display controls

Images selecting `USE_DISPLAY` expose one descriptor-selected panel to the shared command
processor, Berry and LVGL. Commands are unindexed and state is retained until restart; these
controls do not modify `/display.ini` or save runtime changes across restarts.

| Command | Behavior |
| --- | --- |
| `Display` or `Display ?` | Reports panel availability, descriptor name, initialization readiness, logical width/height, effective dimmer, rotation, inversion and power. |
| `DisplayDimmer 0..100` | Zero powers off and retains brightness. A nonzero percentage sets brightness and powers on. Values round through the native `0..255` brightness scale. |
| `DisplayRotate 0..3` | Selects the descriptor rotation and changes logical dimensions. An attached LVGL display adopts those dimensions before its next application-loop render. |
| `DisplayInvert 0..1` | Disables/enables panel color inversion. |
| `DisplayPower OFF\|ON\|TOGGLE` | Changes panel power, preserving its brightness setting. Numeric and boolean state aliases follow the common command parser. |
| `DisplayRefreshNow [0\|1]` | Requests refresh now: empty payload or `0` selects full; `1` requests partial, falling back to full when the EPD descriptor has no partial sequence. |

Dimmer, Rotate, Invert and Power accept an empty payload or `?` to query the current value.
Dimmer queries report zero while powered off. Controls report the last successfully accepted
native state, including changes made through Berry, rather than reading physical panel
registers. `Ready:false` can mean EPD initialization is still in progress or a retained panel
fault has occurred.

RefreshNow is an action and rejects `?`. Its `Accepted` response means the request was
accepted; EPD initialization and coalesced refreshes continue cooperatively, and completion
requires later application-loop progress. Direct panels need no additional framebuffer
publication. The old Tasmota `DisplayRefresh` content-update interval command is absent.
Unavailable panels return `Unavailable`; missing panel-specific controls return `Unsupported`
(for example, brightness without a descriptor dim command or backlight, or inversion without
an inversion opcode). Native failures return `Failed`, `Busy`, `Timeout`, or `No memory` as
applicable. Display commands do not recreate predefined layouts or Tasmota's DisplayText
drawing language. Display hardware qualification remains open.

## PWM and Light commands

Assign `PWM1`–`PWM16` or their inverted GPIO functions on usable native PWM pins. Hardware
routing, timing, and channel limits still apply. `PWM<n>` reads or writes raw duty in
`0..PWMRange`; Light-owned outputs report `OwnershipConflict`. `PWMRange 1` resets to 1023,
and values 255–1023 round upward to 255, 511, or 1023. `PWMFrequency 1` resets to 977 Hz;
values 2–50000 request a common carrier for all configured compatibility PWM outputs.
Frequency and range edits that cannot be applied to inactive outputs are saved with
`RestartRequired:true`. Queries report requested settings; retained hardware diagnostics
remain available in `State` and `Status 11`.

`SetOption15 1` gives configured PWM1–PWM5 to Light; `SetOption15 0` selects raw PWM.
`SetOption68 1` gives each Light channel its own power endpoint. Both selections take effect
after `Restart 1`. Light endpoints follow the highest configured relay index, preserving
relay holes. `Channel<n>` starts at the first Light power index, matching Tasmota's command
boundary; it is distinct from the physical PWM index.

The initial Light commands are `Dimmer 0..100`, `Channel<n> 0..100`, `Color1` and `Color2`,
grouped `White 0..100`, `HsbColor H,S,B` for grouped RGB, RGBW, and RGBCW Lights,
`CT 153..500` for two/five channels, `Fade OFF|ON|TOGGLE`, `Speed 1..40`, and
`LedTable OFF|ON|TOGGLE`. Color accepts full-length hex (optionally prefixed with `#`) or
comma-separated decimal components from 0 through 255, with exactly one component per active
Light channel. Numeric presets `1..12` select RGB colors for three-, four-, and five-channel
layouts; `200..203` select white levels for two-channel CW, four-channel RGBW, and five-channel
RGBCW layouts. Preset `203` starts at 50% dimmer on CW/RGBCW and 13% on RGBW under Newmota's
peak-based normalization. `Color2` retains brightness. `Dimmer` and `Channel` also accept
`+`/`-` in steps of ten. Power OFF retains brightness and color. Accepted Light and raw
duty intent uses `SaveData` persistence and is flushed before orderly restart; fade samples
are transient.

`HsbColor` requires exactly three unsigned values: hue `0..360`, saturation `0..100`, and
brightness `0..100`. Queries report hue and saturation derived from retained RGB channels
and effective RGB brightness from the retained channels and dimmer, regardless of power or
gamma. Queries derive hue from 8-bit RGB, so low saturation can shift the reported hue;
achromatic RGB queries use hue zero. An HSB update selects RGB by clearing any white
channels. Partial tuples, all indexed forms, and independent-channel mode are not supported.

`White` and `White1` select or query the grouped white output on two-channel CW, four-channel
RGBW, and five-channel RGBCW Lights. Other indices, RGB-only layouts, and independent-channel
mode are rejected. A White update clears RGB. RGBW sets its white channel to full scale; CW and
RGBCW retain and normalize the current cold/warm ratio, or rebuild it from the retained exact CT
when the mix is empty. The requested brightness is applied through the Light dimmer, preserving
the CT value. `White 0` retains the white mix while setting brightness to zero and powering off.
Queries report retained white-channel sum times dimmer, bounded to `0..100`, regardless of power,
gamma, or an active fade. HsbColor clears the white mix; a later White command on CW or RGBCW
reconstructs it from retained CT.

Optional `USE_ENERGY_MAX_POWER` provides primary `MaxPower[0..N]`,
`MaxPowerHold[0..N]` and `MaxPowerWindow[0..N]` (unindexed selects phase1, explicit0
selects all). Values0..6000, payload1 substitutes1/10/30; invalid payload queries.
Explicit protection bindings are required before enforcement; see the energy guide.

Optional `USE_ENERGY_MAX_ENERGY` provides primary `MaxEnergy[0..N]` (0..6000Wh)
and `MaxEnergyStart[0..N]` (0..23 local hour). Unindexed selects phase1, explicit0
all configured phases. Invalid payload queries; values persist when excluded.
Explicit output bindings and a complete live daily accumulator are required for
switching; see the energy guide for start/rearm and group restore semantics.

`USE_ENERGY_DAILY_COMPAT` selects the live collector: `EnergyTodayN <signed-Wh>[,timestamp]`
works without civil time, updates no undated CalendarDay and leaves physical counters
unchanged. Its query reports complete live Today even with `ClockValid:false`, while
Yesterday reads 0 without a trusted observed date. Incomplete/faulted Today reads 0
until an explicit seed or trusted rollover, as Tasmota prints numbers only; see the
energy guide.
