# Matter

Work in progress, and not yet tested beyond a first commissioning. `USE_MATTER` runs on ESP-IDF and
RP2350 with at least 4 MiB of flash and on Beken with 2 MiB; other targets give a build error. No
profile selects it. Enable it in a [user configuration](../../../../user_config/README.md):

```yaml
name: my-matter
inherit: esp32s3-qio-qspi
features:
  USE_MATTER: true
```

On Beken, an XH-CB3S (BK7238) with the tiny web page was commissioned with `chip-tool` and Apple Home;
no CB2S has been tested yet. Turn off `USE_MQTT`, or the image does not fit the
update area. Turning off `USE_WEB_CONSOLE`, `USE_WEB_FILESYSTEM` and `USE_BERRY_GPIO` gives a smaller
page and image. `USE_WEB_GPIO` stays on, so the Template tab can take
a pasted template JSON and the Module tab can assign its pins:

```yaml
name: plug-matter
inherit: cb2s-2m
features:
  USE_MATTER: true
  USE_MQTT: false
  USE_WEB_CONSOLE: false
  USE_WEB_FILESYSTEM: false
  USE_BERRY_GPIO: false
```

Then turn it on with `SetOption151 1` and restart. After Wi-Fi connects, the log shows the pairing
code and QR content. On a device without fabrics, the commissioning window stays open for 15 minutes.
The Matter card on the Home page shows the code and a QR code, drawn by the browser from the payload text,
while a window is open, and otherwise a button that opens one, which is also how another controller joins:

```berry
Br print(_matter.pairing_code())
Br _matter.open_basic_window(900)
```

Controllers see `DeviceName`, or the automatic hostname while it is still `Newmota`. A label written
by a controller takes over `NodeLabel` until the next restart. Set a build's default name with
`device_name`.

The passcode and discriminator are random and stored on first start. A personal build can fix them
instead. The code is then readable in the firmware image and the same on every device flashed with it:

```yaml
device_name: Kitchen
features:
  USE_MATTER:
    passcode: 20202021
    discriminator: 3840
```

Both settings are required. The build then prints the QR code, its payload and the pairing code
after the artifacts.

A device replaces its stored code at the next start when the build's differs, which takes a moment
once. Existing fabrics keep working. Going back to `USE_MATTER: true` keeps the stored code.

The node is built from the hardware at each start, in the order lights, relays, sensors:

- Each light becomes an on/off, dimmable, colour temperature or extended colour light, from what
  `light.get()` reports. Move, Step and "move to" commands with a transition time run on the stack's
  250 ms tick, or once a second for bridged lights, and `RemainingTime` shows what is left. An off
  light ignores level and colour commands unless `ExecuteIfOff` is set. Bridged lights can turn on
  with that bit set, because a Tasmota `Dimmer`, `CT` or `HSBColor` command does so unless
  `SetOption20` is on.
- `ColorMode` follows changes made outside Matter on local lights with both colour and white. In XY
  mode, a colour changed elsewhere stays in XY with the point worked out from the new colour. That
  includes the state a bridged light sends back, so its XY is not exactly what was commanded.
- Each remaining `POWER` becomes an on/off plug-in unit.
- With `USE_MATTER_ENERGY`, on by default when Matter and an energy driver are built, an energy
  meter with one channel adds an Electrical Sensor to the first plug-in unit, or gets an endpoint
  of its own without relays. Power Topology names the whole node. Electrical Power Measurement
  reports voltage, current, active, apparent and reactive power, frequency and power factor as
  `ENERGY` shows them, read every 5 seconds and null while the meter has none. Electrical Energy
  Measurement reports the lifetime imported total, and the exported one when, at the start, the
  meter reports export, as `ENERGY` does with `ExportActive`, or has exported, read every minute, with a CumulativeEnergyMeasured event at most every 15 minutes.
  These are the meter's lifetime totals, which `EnergyTotal` does not change. A total that drops,
  as when a Berry driver sets lower totals, sets `CumulativeEnergyReset`: the system time of the
  reset until the next restart, and its epoch time with valid time, kept in the Matter store.
  Tasmota has no energy totals in Matter. Meters with more channels are left out until hardware
  decides how they map onto endpoints.
- Each temperature, humidity, pressure and illuminance value in `Status 10` becomes a sensor, read
  when a sensor reports a new value, at most every 2 seconds. A sensor that appears after the start
  waits for the next restart.
- A board with none of these gets one virtual on/off output, to test commissioning.

Relays and lights changed outside Matter reach controllers on the next 250 ms tick. The tick compares
the change counter of the power owner and runs no command while it stands still. Bridged devices
are followed when their state message arrives.

With `USE_BLE` and `USE_MATTER_BLE`, a device without fabrics and without Wi-Fi also advertises for
BLE commissioning, 20 seconds after the start and until the window closes. A running BLE stack is
left to `SetOption115`: with it off, commissioning starts the stack and stops it afterwards. A Berry
script must not use `BLE` connection callbacks while commissioning runs. Wi-Fi credentials from the
commissioner stay in RAM until commissioning completes. If the fail-safe expires after a tested
network, the device restarts and returns to its stored state. AP onboarding stays available, and
whichever finishes first wins. When BLE commissioning wins, the SoftAP stops.

With `USE_MATTER_BRIDGE`, remote Tasmota devices appear as bridged endpoints over MQTT. Up to seven
remotes are configured from the console and take effect at the next restart:

```berry
Br _matter.bridge.discover(true)            # Listens to tasmota/discovery/#
Br print(_matter.bridge.discovered.keys())  # Discovered device ids, a few seconds later
Br _matter.bridge.add_discovered("<id>")    # Relays, lights and sensors from its discovery data
Br _matter.bridge.discover(false)
Br _matter.bridge.add("topic", [{'type': 'relay', 'relay': 1}]) # Without discovery
Br _matter.bridge.remove("topic")
```

Newmota publishes Home Assistant discovery, not Tasmota discovery, so Newmota remotes need `add`. Commands fail while a remote
is unreachable or has not answered live since the last link change. An MQTT publication above the
packet limit is now skipped with a log line, instead of ending the session.

Removing the device from a controller removes only that controller's fabric. A controller that added
two fabrics leaves the other one on the device, and the window stays closed until opened from the card.

The device uses the test attestation certificate (vendor `0xFFF1`), so controllers show it as
uncertified.

## Commands

| Command | Effect |
| --- | --- |
| `MtrInfo` | Fabrics, endpoints with device types, enforcement, window and, while it is open, the codes |
| `MtrJoin` | `1` opens a commissioning window, `0` closes it, no argument queries |
| `MtrReset 1` | Removes every fabric, which forgets all controllers, and opens a window |

The Matter card offers the same removal; its button needs a second press. `MtrUpdate` of Tasmota does not
exist yet. Commands answer only while the stack runs, otherwise with `{"<command>":"Off"}`.

## TCP and the camera

Not yet tested against a controller. `USE_MATTER_TCP` (ESP-IDF) adds Matter's TCP transport on port
5540 for large messages, which the camera's commands need. Each message travels after its 4-byte
length, up to 32000 bytes, because Berry `bytes` stop at 32 KiB. TCP carries no MRP. A session
established over TCP ends when its connection closes and is not stored. DNS-SD then announces
`T=6`, TCP client and server.

The same feature lets the device send commands to a controller. A command that needs TCP uses a
TCP session of that node. If there is none, the device opens a TCP connection to the address and
port of the session the request came from, and runs CASE as the initiator there. That port is a
guess that holds for SDK controllers, which listen for TCP on their UDP port. A request that gets
no answer within 30 seconds fails.

`USE_MATTER_CAMERA` selects `USE_MATTER_TCP`, `USE_CSI_WEBCAM` and `USE_AUDIO_OPUS`, since Matter
audio is Opus or AAC, never G.711. It adds a Camera endpoint
(0x0142) with Camera AV Stream Management (audio, video and snapshot), Camera AV Settings User
Level Management with digital PTZ, and the WebRTC Transport Provider, and lists the WebRTC
Transport Requestor as its client cluster.

- One encoder serves everyone, so each allocation answers with the stream already allocated of
  its kind. Video takes H.264 and a resolution range around the camera's current mode; audio
  takes Opus, mono and 16 bits at 8 or 48 kHz (the microphone runs at 8 kHz, Opus on a 48 kHz
  clock); snapshots take JPEG. Allocations are stored in the
  Matter store.
- `CaptureSnapshot` sends one JPEG frame. A frame larger than the session carries, which means
  any real image over UDP, answers `ResourceExhausted`.
- `ProvideOffer` and `SolicitOffer` need `WcSession 3`. For `ProvideOffer` the driver answers,
  the device replies with the session, and then it invokes `Answer` on the controller's
  requestor endpoint. For `SolicitOffer` the driver makes an offer with `setup:actpass`, which
  goes to the requestor as `Offer`; `ProvideAnswer` then needs `setup:active`, because the device
  runs only the DTLS server. The driver serves one peer, so a new session ends the earlier one
  with `End` (`Replaced`).
- `ProvideICECandidates` is accepted and ignored, since the ICE-lite host learns its peer from
  STUN. `EndSession` forgets the session.
- Digital PTZ moves the sensor window, as `WcPan` and `WcTilt` do; see
  [pan and tilt](../../drivers/camera/README.md#pan-and-tilt). `VideoSensorParams` gives the
  whole sensor array, and viewports are rectangles in its pixels. One window serves every
  stream, so `DPTZStreams` lists each allocated video stream with the same viewport, and
  `DPTZSetViewport`, `DPTZRelativeMove` and a write of the `Viewport` attribute all move it.
  The camera crops and has no scaler: a viewport keeps the window's size, and another size or
  a `ZoomDelta` is a `ConstraintError`. A relative move stops at the sensor's edges. Moves made
  outside Matter are reported within a second.
- Media stays LAN only: an ICE-lite agent has host candidates only, so the `ICEServers` of a
  request are ignored, and a viewer outside the LAN needs full ICE with TURN, which the driver
  does not have.

## Access control

Each fabric keeps its own access control list, and CASE certificates are checked against the fabric's
root. Both only log for now: `MTR: NOC chain validation failed` and `MTR: ACL would deny`. They do not
refuse anyone yet, so **any holder of a fabric's IPK can still become administrator**. Once real
controllers commission and operate without those lines, turn enforcement on. It survives restarts:

```berry
Br _matter.set_enforce_access(true)
```

## Structure

Bytes from the network before authentication, and anything done per packet, are native and bounded:

- `matter_wire`: frame, exchange and TLV decoding, covered by `make test-host`.
- `matter_stack`: sessions, replay windows, AES-CCM, retransmission and acknowledgements.
- `matter_dns_sd`: the DNS-SD responder.
- `matter_btp` and `matter_ble`: BLE commissioning, with BTP's rules covered by `make test-host`.
- `matter_cert`: rebuilds the signed part of a certificate, checked against an X.509 encoder.
- `platform/lwip/matter_udp`: IPv6 sockets and the DNS-SD multicast group.

Everything above is solidified Berry in `berry/embedded/`. It never sees raw frames, and it parses
TLV only through the bounded `matter.tlv()`. In the result, structures and lists are maps by tag,
arrays are lists, and 8-byte integers stay little-endian `bytes` because Berry has no 64-bit integer.

Fabrics, the device identity, and CASE sessions with their subscriptions are stored in
`/.newmota/matter/`, unencrypted. A controller therefore carries on after a restart without a new
handshake, and each resumed subscription gets a full report as soon as the network is up. Sessions keep
a reserved range of send counters and resume above it, so a restart cannot reuse a nonce. Saving happens
when a session or subscription changes and when that range runs short.

## Solidified Berry

Berry sources in `berry/embedded/` compile into flash at build time. They use no heap until a
script names their class. To add one, list it in `SOLIDIFIED_SOURCES` in `tools/newmota/berry.py`,
mark each class with `#@ solidify:<class>,weak`, include `solidified_<stem>.h` in
`berry/matter_classes.c`, and add the class to `MATTER_CLASSES` in `berry/matter_module.cpp`.
