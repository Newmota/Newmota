# Application web interface

Network-capable normal application images include one bounded HTTP or HTTPS service and an
embedded responsive page. Large application profiles default to HTTPS on all platform
families; Compact, Standard, and Safeboot profiles use HTTP. The page reads device data from the
application APIs; it does not contain demo values for enabled controls. The build minifies
every embedded page before compressing it: comments and layout whitespace are removed, and
script and style bodies pass through `rjsmin` and `rcssmin`, which do not rename or rewrite
code; `pre` and `textarea` content stays verbatim. HTTPS builds embed Brotli, while HTTP-only
builds embed Gzip compressed at build time; the device serves the selected bytes verbatim and
performs no runtime compression. Recovery pages remain Gzip. Comments in `index.html` cost
nothing on the device.

The application `wget <HTTP-URL>` command excludes browser filesystem transfers and new
firmware uploads until its asynchronous download finishes. Existing browser uploads prevent
`wget` from starting. See the [command interface](../../core/command/README.md).

## Current interface

- The HTTP listener uses port 80 and the HTTPS listener uses port 443. It starts after station
  networking is ready or the recovery AP has a local address.
- An HTTPS build creates a device-unique P-256 key and self-signed certificate on first start
  and persists them at `/.newmota/web-identity.bin`. The private key is protected from browser
  reads and mutations. Browsers warn because the certificate is not issued by a trusted CA.
  A damaged identity is regenerated and atomically replaced. Storage, generation, or TLS
  startup failures leave the listener unavailable; they do not fall back to HTTP.
- Select HTTPS on another profile, or disable it on Large, in a personal build:

  ```yaml
  name: home-s3-https
  inherit: esp32s3-qio-qspi
  features:
    USE_WEB_HTTPS: true
  ```

  An HTTP build omits the web TLS identity generator and TLS server dependency.
  `crypto: none` is independent and does not disable the web server's TLS.
- `WebPassword <value>` provisions an 8..64-byte browser password through the physical
  console. `WebPassword 0` deliberately leaves the listener open. Password changes and
  settings resets invalidate existing sessions.
- `/api/state` supplies the small complete power snapshot, optional `/api/sensors` supplies
  the latest canonical sensor document, and `/api/sensors/readings` supplies current typed
  sensor values with display labels, units and, except for derived dew point, a `series` id.
  `/api/sensors/updates` supplies only sensors with new data, as StatusSNS members plus any
  dashboard-only fields: every received packet of a BLE device, or a changed value of a local
  driver, marks that sensor in the session's mask. `/api/sensors/updates/all` marks everything
  first, for a page that draws all sensors. An answer holds as many sensors as fit the response;
  `"more":true` says others are waiting and `"reset":true` asks the page to fetch everything
  again. `"heap"` is included about every 10 s. While Home or Info is open the page asks about
  four times a second, the next question only after the previous answer, and redraws only the
  sensors in the answer, so a failed request changes nothing. It shows every known BLE device,
  renders one card per sensor with its measurements as rows and `RSSI`, `Battery`, `LastSeen`
  and `NMT` in a small footer, and counts ages up by itself.
  `/api/sensors/history/<series>` returns that reading's last 24 hours as
  `{"step":<seconds>,"values":[...]}`, oldest first with `null` for gaps; the panel fetches one
  series at a time, each at most once a minute, and draws it under the measurement's row;
  footer fields fetch no history. The `series` id is a hash of sensor name and field.
  `/api/widgets/updates/<since>` answers what changed in the Berry widgets after revision
  `since`: every widget in display order with its markup and data revisions, the data changed
  since then (`"more":true` when not all of it fits), the revision to ask from next and a
  per-boot generation. `/api/widgets/markup/<id>` returns a widget's markup as text. Home asks
  every 250 ms while widgets change and once a second otherwise, fetches markup for at most two
  widgets per question, continues a partial answer after 250 ms, and shows them after the native cards. `POST /api/widgets/action` takes
  `id\nname\nvalue` from a widget control and queues it for the script (503 when eight are
  waiting). See the
  [widget module](berry/README.md). `/api/info`
  supplies the slower authenticated device
  metadata snapshot, `POST /api/filesystem/list` returns a bounded
  LittleFS directory page, `POST /api/command` submits commands,
  `POST /api/console/command` submits a command and returns its timestamped result to the page
  that sent it,
  `POST /api/console/poll` reads bounded Web and Berry console histories, and
  `POST /api/berry/console` runs short raw source in the persistent Berry VM. Larger submissions
  use `POST /api/berry/console/begin`, sequential binary `POST /api/berry/console/chunk` requests,
  then `POST /api/berry/console/run`; `POST /api/berry/console/abort` discards an unfinished
  upload.
- With `USE_WEBCAM` or `USE_CSI_WEBCAM` and the camera configured, `/api/info` carries
  `"camera":true`, and `GET /api/camera` returns
  `{"stream","port","width","height","starts","sensor"}`. While `stream` is true, Home shows a
  card above the lights that embeds the camera's port-81 page (`frame-src *:81` in the CSP), or
  only links to it when the UI runs over HTTPS. A change of `starts`, the stream server's starts
  since boot, reloads the embedded page. See the
  [camera driver](../../drivers/camera/README.md#stream-endpoint).
- With `USE_WEB_AUDIO` and an I2S output in the template, `/api/info` carries `"audio":true`,
  and Home shows an audio player card below the camera. `GET /api/audio` reports the command
  session that the `I2S*` commands share with the console and MQTT: `state` (`idle`, `running`,
  `ended`, `failed` with `result`), and while a session exists `source`, `record`, `codec`,
  `location`, `loop`, `paused`, `elapsed`, `rate` and `channels` (0 until playback has decoded
  its first frames), plus `station` and `title` from ICY metadata for webradio. `gain` and the
  compiled playback `codecs` are always present. The card has no route of its own for control:
  it sends `I2SPlayN`, `I2SLoopN`, `I2SWrN`, `I2SPause`, a bare `I2SPlay` to resume,
  `I2SStop` and `I2SGain` through `/api/command`. Starting while a session runs stops it first.
  The selector `N` comes from the codec choice, or from the extension (`.mp3`, `.aac`, and
  `.opus`/`.ogg`/`.webm` for Opus; a URL without one defaults to MP3). With
  `USE_WEB_FILESYSTEM`, the source field suggests the audio files in the flash root and the SD
  card root. The card polls once a second while a session runs and every 5 seconds otherwise,
  only while Home is shown. The feature is off by default and adds the card's page section.
- With PWM or WS2812 Light configured, Home shows one light card per Light endpoint above the
  relay and sensor cards, and the relay grid omits the `POWERn` indices those lights own.
  `GET /api/light` returns `{"Lights":[...]}` with each endpoint's `Kind` (`dimmer`, `ct`,
  `rgb`, `rgbw`, `rgbct`), power index, and state; `POST /api/light` takes `endpoint=<n>` plus
  any of `power`, `bri`, `hue`, `sat`, `ct`, `rgb`, or `channels` and returns the resulting
  endpoint. Values use the Berry `light` scale (`bri` and `sat` 0..255, `hue` 0..360, `ct`
  153..500). Cards choose controls by kind only: a dimmer, a dimmer with CT slider, or a colour
  wheel for hue and saturation with a dimmer, plus a white or CT slider for RGBW and RGBCW.
  A light that switches between white and colour, a TuyaMCU RGBW or RGBCW light with its mode
  data point mapped, also returns `Mode` (`white` or `color`), and its card gets Tasmota's
  light mode button, named after the mode it switches to: colour sends the wheel's hue and
  saturation (full saturation after white), white sends the CT on RGBCW and full white at the
  current brightness on RGBW. Like the other controls it switches the light on.
  The wheel is keyboard operable: left/right change hue, up/down change saturation. As with Tasmota's `Dimmer` and `Color`, a nonzero brightness or a colour/CT change
  switches the light on and brightness 0 switches it off; an off light shows brightness 0 and
  keeps its stored dimmer. The wheel always sends hue and saturation together, so white does
  not lose the selected hue. Updates are sent at most every 150 ms per light, latest value wins, and are
  never retried. Changes use the `WebGui` power source and reach `State` and MQTT like commands.
- Configuration → Module and Template edit GPIOs in Tasmota's two steps. Module selects a
  built-in module or the user template (0) and assigns functions to the pins that template
  marks `User`; Template edits the user template itself (name, base, flag, one function per
  slot, `User`, or none), shows its JSON for copy and paste, and can activate it as Module 0.
  `GET /api/gpio/catalog` returns the architecture, implemented functions
  (`[first, instances, requirements, name]`), and modules; `GET /api/gpio` returns the
  selected module, the user template, and every pin as `[pin, slot, flags, capabilities,
  base, effective]`. Saving uses the validated `Module`, `GPIO {"GPIO":[...]}`, `Template`,
  and optional `Restart 1` commands; changes apply after restart. Functions a pin cannot
  support are not offered, reserved pins cannot be assigned, and duplicate functions block
  saving.
- Configuration → Timer lists all 16 Tasmota timers with the global `Timers` switch and edits
  one timer at a time: active, mode (time of day, sunrise or sunset with a ± offset up to
  11:59), random window, weekdays, repeat, output, and action (off, on, toggle, Berry
  callback). A location card for `Latitude`, `Longitude`, and `Sunrise` appears when a timer
  uses sunrise or sunset, and a warning shows while the device clock is not synchronized,
  because timers fire only with valid time. `GET /api/timers` returns the switch,
  `TimeValid`, location, and every timer in Tasmota fields; saving sends one complete `TimerN` record, `TimerN 0`, `Timers`, and the
  solar commands. Unsaved edits are kept until saved or discarded. The Beken page has the same
  editor in compact English.
- Firmware Update shows the running version and build. "Update from URL" saves `OtaUrl` and
  starts `Upgrade 1`, then polls `Upgrade` once a second while that page is open and an update
  is active; platforms that download in Safeboot report the handoff and restart. "Upload
  firmware file" restarts into the Safeboot recovery page with `Restart 3`, where the file is
  uploaded; the page warns when no WebPassword is set because Safeboot then refuses access. The
  update-channel card has no backend yet and stays visibly unavailable.
- Requests use random session cookies, a separate CSRF token, Host/origin checks, and bounded
  headers, bodies, responses, and connection counts. Commands are never retried automatically.
- Enabled browser controls currently cover Wi-Fi slots 1 and 2, Wi-Fi scan/setup, relay commands, Light controls, Module and Template GPIO editors, timers, firmware URL update and Safeboot handoff,
  restart, MQTT configuration/topic previews/connection diagnostics, read-only device information, the LittleFS browser and text editor, the Web Console, and
  the Berry Console when Berry is present in the firmware. The filesystem browser lists entry
  names, types, and sizes, follows bounded continuation pages automatically, and provides per-file
  Edit and Delete actions plus Delete for directories. Directory deletion is limited to empty
  directories. Upload accepts multiple files, folder selection, and dropped files or
  folders where the browser supports directory traversal. Relative folder paths are preserved;
  empty folders are not transferred. The editor accepts UTF-8 text, supports new files and
  Save As, normalizes line endings on save, and confirms navigation with unsaved changes.
  Transfers show progress and support cancellation; interrupted writes preserve the existing file.
  Newmota internal settings files are protected from browser reads and mutations. With
  `USE_SDCARD` a mounted card appears as the `sd` folder in the same tree: the storage bar and
  `/api/info` (`"sd"`: mounted, available, used and total bytes) report the card while it is
  open, Information adds an SD card row, uploads stage in the card's own `/sd/.newmota`, and
  the card root and that staging folder cannot be deleted. The two console histories have
  separate inputs and output panes;
  bounded polling reads them together. Console command results reach the submitting page in
  the HTTP response and other pages through shared history without duplication at the sender.
  Berry `print()` output also reaches normal log sinks,
  while Tasmota-compatible `log(message, level)` writes to those sinks and returns nil. Shared
  command results also appear in the Web Console history. Panels whose APIs do not exist are
  visibly disabled.
- The Web Console and Berry Console keep separate, page-local histories of the 20 most recent
  submitted inputs. Up/Down navigates history; Berry preserves normal caret movement inside
  multiline source and uses the keys at the source boundaries. Berry input accepts up to 64 KiB
  of UTF-8 source. Inputs above the 2 KiB request-body size transfer in sequential chunks into a
  temporary heap buffer, which is released after Berry compiles the source. Incomplete uploads
  expire after 30 seconds; source is never staged to LittleFS. It runs from its button,
  Ctrl/Command+Enter, or Enter on a trailing blank line. Web command bodies remain capped at 512
  bytes. This input history is separate from the bounded
  device output histories read by polling. The device keeps the last 8 lines of Berry output; a
  console error report takes at most 7 of them (the error message first, then the traceback)
  and ends with "(traceback truncated, full trace in the log)" when it is longer, so a long
  traceback never hides its error. The log keeps every line.

MQTT-enabled normal applications expose authenticated `GET /api/mqtt` and atomic form-encoded
`POST /api/mqtt`, with explicit password preservation, replacement, or clearing. Password reads
return only presence. `POST /api/mqtt/preview` resolves a complete draft without saving it;
`GET /api/mqtt/status` reports connection/test state and bounded counters. `POST /api/mqtt/test`
tests the saved, enabled plain broker through CONNECT and all subscription acknowledgements,
pauses MQTT command delivery during the trial, and restores the normal client afterward. The
shared stream cannot be borrowed while an update is active. Settings save success is separate
from broker connection success. MQTT forms use the existing bounded request mailbox; console
command limits remain unchanged. Both the generic localized page and Beken's compact English
page include these controls. The normal web transport is selected at build time and is never
silently downgraded.

The update work adds two role-specific surfaces. Network-capable Safeboot images serve a
compact recovery-only page from `/`; Newmota application users enter it with `Restart 3` and
reconnect after Safeboot starts. Beken embeds one compact platform-specific root `index.html`
with the generic page’s sidebar, responsive panels, and four theme palettes, plus Wi-Fi
configuration/onboarding for both credential slots, read-only device information, filesystem browsing/upload/editing, console, and the native RBL updater. Its Home tab shows the same
`/api/light` cards in a compact English form (colour wheel, dimmer, white or CT slider, and the
light mode button),
one relay switch per `POWERn` that no light owns, and sensor cards, the energy meter among
them, from `/api/sensors/readings`. Its Configuration page
splits Wi-Fi, MQTT, Module, Template, and Timers into tabs, with compact Module and Template
GPIO editors on the same `/api/gpio` routes. Information and the filesystem browser show
LittleFS used, free, and total space from `/api/info`.
It retains a smaller English-only asset rather than embedding the complete generic page.
With Berry, Home also shows Berry widgets with their markup, data and controls. Its only charts
are `matrix` and, with `USE_MATTER`, `qr`; `w2`/`w3` widen a card, row spans and the update
highlight are left out. The build keeps only the page sections
whose features are on: `<!--#if USE_X-->` in markup and `/*#if USE_X*/` in scripts, closed by
`#endif` the same way, with `!USE_X` for the opposite; an unknown name or an unbalanced marker
fails the build. MQTT, timers, lights, energy, the RBL upload and the panels below follow their
features. The generic page uses the same markers; there only the audio card (`USE_WEB_AUDIO`)
and its file suggestions (`USE_WEB_FILESYSTEM`) are cut so far. Update
requests use sequential binary chunks capped at 1 KiB, with verification before activation.
The normal application login policy applies to Beken, including its existing open mode when
WebPassword is empty. Beken's compact root page uses the same filesystem listing and transfer routes. Safeboot requires a configured WebPassword and fails closed without it.
Beken updates keep LittleFS, because it lies outside the update area. These routes compile in
the five-family native matrix; end-to-end upload, reboot, and recovery behavior still needs
hardware qualification.

When no station credentials exist, Wi-Fi starts the open access point using the resolved
Newmota/MAC-derived hostname as its SSID. The shared identity resolver bounds that name to a
31-byte payload so the platform's 32-byte Wi-Fi SSID buffer retains its NUL terminator, matching
Tasmota's fixed hostname formatting. If a configured station
cannot connect, the same AP is used for recovery. The onboarding form can test candidate
credentials while keeping the AP available; credentials are committed only after association
and DHCP succeed. The setup page is `http://192.168.4.1`, or `https://192.168.4.1` when
`USE_WEB_HTTPS` is enabled. Captive-portal DNS
redirection is not provided.
The authenticated `/api/info` result reports the persisted `DeviceName` separately from the
runtime network hostname.

The browser and serial command paths share the application command processor. The web page
uses the same canonical relay `STATE` payload that feeds the MQTT telemetry path, so the
firmware does not maintain a second dashboard-only device model.

Three panels are optional for small images; each is on by default with `USE_WEB`, and turning it
off removes both its routes and, on Beken, its part of the page:

- `USE_WEB_CONSOLE`: the Web and Berry consoles, `/api/console/*`, `/api/berry/console*` and the
  console history. `/api/command` stays, because the page saves settings through it.
- `USE_WEB_FILESYSTEM`: the file browser and editor, `/api/filesystem/*`.
- `USE_WEB_GPIO`: the Module and Template editors and `/api/gpio*`; it needs
  `USE_HARDWARE_COMMANDS`.

The generic page is not filtered yet and still contains these panels.

## Platform handoff

ESP-IDF uses its HTTP or HTTPS server, AmebaD uses the native HTTPD library, Beken and Bouffalo
use their lwIP socket APIs, and Pico uses raw lwIP or its altcp TLS layer with the CYW43 lwIP
lock. Native
callbacks only perform bounded request handoff; settings, filesystem, relay, and command
work runs in the application loop.

The normal application web slice is compile-tested across all five current platform
families. ESP32-S3, XH-CB3S/BK7238, BW16, and Pico 2 W have browser smoke evidence; those
tests predate the Beken RBL root page. The revised generic-style shell, Wi-Fi/dashboard
controls, and package size pass native builds for BK7231N and BK7238; browser validation of
the complete page and hardware qualification of the native RBL update path remain open. An
initial XH-CB3S/BK7238 Wi-Fi/UI hardware check on 2026-10-04 reported no regression.
BL602 has build and size evidence while hardware remains unavailable.

The Tasmota-style `OtaUrl` setting and `Upgrade 1` command start an asynchronous HTTP pull
through the selected native writer; querying `Upgrade` reports state and byte progress. The
first transport accepts a direct HTTP 200 response with `Content-Length`; redirects, chunked
transfer, and HTTPS are rejected. The URL-pull code compiles in the five-family native matrix;
exact-device update qualification remains open. Runtime compression selection and WebSockets
remain open. Console output uses bounded polling; streaming logs while a synchronous command is
running remains open.


## Energy readings

With `USE_ENERGY`, the selected meter is one more sensor: it shows one `ENERGY` card with
all its phases, through the same sensor routes as every other sensor and with no energy
route of its own. The rows follow Tasmota's web table: Voltage, Frequency, Current, Power,
PowerTotal, ApparentPower, ReactivePower, Factor, Today, Yesterday, Total and ExportActive,
and the ADE7880 adds CurrentNeutral.
Rows of the driver's own follow, each in its unit: the generic Modbus meter shows every user
field that has a GUI name and a first-phase register, as Tasmota's table does, and BL0939/BL0940 and BL0906 add
their chip Temperature. That copy does not feed the global temperature; the meter reports it itself.
A multi-phase meter numbers its fields per phase (`Power1`, `Power2`, …); a common voltage
or frequency appears once, and Today, Yesterday and Total are sums unless SetOption129 is
on. Values use `VoltRes`, `AmpRes`, `WattRes`, `EnergyRes` and `FreqRes`, which the other
sensors share; energy shows at most three decimals. Only power keeps a 24-hour history.
Missing or invalid measurements are left out, so an offline meter still shows its totals.
Configure the wiring in the normal Module/Template GPIO editor and apply it with explicit
`Restart 1`. See the [energy setup guide](../../drivers/energy/README.md).
