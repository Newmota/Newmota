# Berry BLE

`import BLE` provides the staged Berry BLE interface: `init`, `loop`, `info`,
`conn_cb`, `serv_cb`, `adv_cb`, `adv_watch`, `set_MAC`, `set_svc`, `set_chr`, `run`
and `store`. The module owns `BLE.buffer`; set `BLE.len` to the input length before
`run`. Completion callbacks run from the cooperative application loop. Advertisement
and connection callbacks share the buffer; advertisements wait while a job owns it.
When a native consumer such as a sensor decoder also scans, it sees every report first and
the script receives one held copy; reports arriving while that copy waits count in
`adv_drops`.

Scanning is passive by default, with a 100 ms interval and an 80 ms window, on every
backend. Three commands change it until reboot; nothing is saved, so `autoexec.be` can
repeat them with `newmota.cmd()`:

| Command | Behavior |
| --- | --- |
| `BLEActive 0\|1` | Passive (default) or active scanning. Active scanning requests scan responses and appends them to the report after a brief wait. |
| `BLEScan <interval>,<window>` | Scan timing in milliseconds, 3 to 640, window no longer than interval. |
| `BLEWatch <MAC>[,<type>]` | Adds a device (12 hex digits, colons optional; address type 0 to 3, default 0) to the watch list, which `BLE.adv_watch` shares. Once it holds a device, only watched devices are reported. The list lasts until reboot, also before BLE starts and across Berry restarts. `BLEWatch 0` empties it; `BLEWatch` lists it. Bouffalo and AmebaD accept address types 0 and 1 only. |

BLE is opt-in. No checked-in environment or Safeboot selects it. For a personal
BL616 build, create an ignored `user_config/bl616-ble.yaml`:

```yaml
name: bl616-ble
inherit: bl616dk
features:
  USE_BLE: true
```

Build with `env UV_CACHE_DIR=/tmp/newmota-uv-cache make build ENV=bl616-ble`.
For BL602, use a separate variant inheriting `bl602-4m`.
This validation base does not establish a minimum flash capacity. A BLE-enabled
2 MiB application has not been sized or qualified. Every selected image must fit
its own catalog slot; no partition is enlarged by selecting BLE.

## Bouffalo BL602 and BL616 backend

The native SDK blestack runs over BL602's `m1s1` or BL616's
`ble1m10s1bredr0` BLE-only controller archive. BL616 retains the SDK's ten-link
host/controller pools and 32 KiB exchange-memory reservation; the adapter admits
one total link. The SDK linker reserves exchange RAM from WRAM before placing
Wi-Fi sections and its remaining heap. BL616 selects the SDK's 32 KiB lwIP
packet-heap override instead of its default 64 KiB; RX buffers, protocol pools
and DMA descriptors keep their native placement. Packet pressure during Wi-Fi
traffic remains unqualified. BL616 needs no BL602 linker adaptation.
Regular BL618 modules such as Ai-M61-32S use `bl616dk` with a GPIO template.
BL618DG is a separate target; its BLE selection is deferred and rejected.

- BLE 4 legacy scanning, undirected advertising and scan responses. Reports carry up
  to 62 bytes; active scanning waits briefly to append a matching scan response.
  Controller duplicate filtering is disabled. One report slot and eight watched
  addresses bound scanning; discarded reports are counted in `BLE.info()`.
- One total connection, either outbound or accepted. Scanning pauses during a
  connection attempt or link. An accepted connection can serve client jobs only
  when the staged peer matches it; another peer cannot consume that occupied link.
- Client service/characteristic discovery, read, write, subscribe/unsubscribe,
  notifications and one-shot operations. A confirmed subscription writes its CCC
  through the normal ATT completion path. An unconfirmed subscription reports
  transport submission only. Eight subscriptions are available per link.
- Four local services and twelve characteristics. The first advertisement freezes
  the database. Values and queued event payloads are limited to 255 bytes.
  Reads use the prepared value immediately; writes replace it and queue an event.
  Notifications and indications follow the peer's CCC selection. Prepared/offset
  server writes, signed/reliable/extended writes and application authorization
  flags are rejected. Own-address replacement (operation 231) is unsupported.
  Directed advertising (mode 1) is rejected; modes 0 (non-connectable) and 2
  (undirected connectable) are supported.
- Four queued unsolicited events and a separate job-completion slot. Queue overload
  cannot discard a job completion. Closing or destroying the VM revokes delivery,
  drains the link and pending server callbacks, then unregisters the database.
  The host and its two adapter tasks remain allocated for reuse and native scanning.
- Pairing accepts a staged PIN when the peer requests passkey entry; the local
  device advertises keyboard input capability. Secure Connections depends on the
  peer and native stack. The native PRNG is seeded through HCI LE Rand.
  There is no numeric-comparison UI or automatic confirmation of a displayed number.

Bonds live in the native RAM key pool (one peer on BL602, ten peers on BL616).
The authenticated event, operation 230, exports a bounded `BLK1` record containing
named key fields, the full SDK revision, chip/controller configuration and local
public identity.
A script may persist it and restore it with `BLE.store()`. Import requires a matching
SDK revision, chip/controller configuration and device identity, validates
lengths/types/key masks, and refuses to evict another peer. These records are
incompatible with ESP NimBLE records. EasyFlash
settings are disabled; BLE never claims or writes a vendor settings partition.
Protect exported key material as credentials. A changed SDK or identity requires
re-pairing. `BLE.info()` omits RSSI when it has not been sampled; transmit power is
currently reported as zero because this adapter has no qualified power readback.

The native BL602 and BL616 builds and packaging are compile-tested only. Radio
operation, RAM headroom with an active server, dense advertising, pairing/bond round trips,
VM restart and coexistence with Wi-Fi, flash writes and OTA remain unqualified.

## AmebaD backend

The AmebaD adapter uses the pinned Realtek `btgap.a` host/controller interface and the
same Berry module. For BW16/RTL8720DN fitted with at least 4 MiB flash, create an ignored
`user_config/bw16-4m-ble.yaml`:

```yaml
name: bw16-4m-ble
inherit: bw16-4m
features:
  USE_BLE: true
```

Build with `env UV_CACHE_DIR=/tmp/newmota-uv-cache make build ENV=bw16-4m-ble`.
The standard `bw16-4m` application and its paired Safeboot remain BLE-free.

- Legacy scanning, advertising and scan responses, with up to 62 report bytes,
  one report slot, eight watched addresses and visible drop counters. Native duplicate
  filtering is disabled. Extended scanning/advertising and controller privacy are
  unavailable in this SDK selection. The reported version is 5; that is not an extended
  advertising claim. Operation 231 selects the public identity or a static random
  address. Advertising modes 0, 1 and 2 provide non-connectable, directed connectable
  and undirected connectable advertising; directed mode uses the staged peer.
- One total connection in either role. Scanning pauses during a client job/link.
  Client discovery, reads, writes, subscription changes, notifications and one-shot
  jobs are supported. An accepted link is usable for client jobs only when the staged
  peer matches it. Eight subscriptions are available. Confirmed writes, including CCC
  changes, wait for ATT completion; unconfirmed writes report native submission.
  Confirmed client writes support the native prepare/execute path up to 512 bytes.
- Four local services and twelve characteristics, with values and queued payloads
  limited to 255 bytes. The first advertisement freezes the database. Reads return a
  prepared value immediately; writes replace it and queue an event. Notifications and
  indications follow the peer's CCC selection. A failed or partial native database
  registration permanently disables that database until reboot.
  Encryption and authentication permissions are supported; local prepared/offset,
  signed/reliable writes and application authorization flags are rejected.
- Four unsolicited event slots plus a separate job completion. Advertisements cannot
  consume that completion. VM close revokes delivery before disconnecting, clears the
  prepared values and preserves the native scan consumer. The host, tasks and immutable
  native attribute tables remain allocated. A restarted VM can reuse matching UUIDs and
  permissions; changing the schema requires reboot. A failed connection drain disables
  the adapter until reboot, preserving native storage for any late callbacks.

Pairing uses the staged PIN for passkey entry and a fixed displayed passkey. Numeric
comparison is automatically confirmed, as on ESP; Secure Connections depends on the peer.
An ATT authentication/encryption failure pairs and retries once. The authenticated event
(230) exports a canonical `ADK1` bond record after successful encryption and native key
storage. `BLE.store()` validates its exact SDK revision, storage configuration, local
identity, key lengths and types, and refuses replacement or eviction of a different bond.
One peer is retained in bounded RAM; scripts own persistence across reboot. These records
are incompatible with ESP and Bouffalo records. The vendor flash FTL and its startup are
excluded; BLE never writes a vendor bond partition. `BLE.info()` reports live security,
bond count, peer identity/IRK and sampled RSSI; transmit power remains zero because the
pinned public API has no power readback.

Native controller initialization requires the Wi-Fi driver/radio to be powered, as it
is by the SDK boot task. The BLE message task waits up to 15 seconds for that native
readiness, then disables the adapter if it is unavailable; it does not require station
association or network services. BLE does not independently
power the radio when the driver is off. It uses internal UART1 and discards native binary
trace frames without claiming UART3 or its GPIOs.
The controller uses the pinned compiled native firmware patch; saved vendor system-data
flags cannot redirect it to vendor flash-patch slots.

The adapter is native compile-tested only. Radio operations, runtime RAM headroom,
advertising pressure, VM restart, Wi-Fi coexistence and flash/OTA coexistence still need
hardware qualification. There is no hardware result for this backend.


## Pico W and Pico 2 W backend

Pico 2 W/RP2350 is the initial validation target. The shared backend uses the pinned
Pico SDK BTstack host and native CYW43 transport. Enable it in an ignored personal
`user_config/pico2-w-ble.yaml`:

```yaml
name: pico2-w-ble
inherit: pico2-w
features:
  USE_BLE: true
```

Build with `env UV_CACHE_DIR=/tmp/newmota-uv-cache make build ENV=pico2-w-ble`.
Pico W/RP2040 uses a separate variant inheriting `pico-w`. Selecting BLE does not
change the flash layout or establish a minimum flash size. Each resulting image
must fit its existing application slot. Boards without the supported W radio are
rejected. No standard environment or paired Safeboot enables BLE.

- Legacy scanning, advertising and scan responses, with 62-byte reports,
  one report slot, eight watched addresses and visible drop counts. Controller
  duplicate filtering is disabled. Active scanning waits briefly for scan responses.
  Extended/periodic advertising, coded PHY, BLE Mesh and controller privacy are
  outside this adapter. The version field reports 5; it does not imply these features.
- One total connection, either outbound or accepted. Client jobs on an accepted
  link require the staged peer to match. Scanning pauses during a client job or
  link. Native service/characteristic discovery, long reads, confirmed writes
  (including native prepare/execute when needed), writes without response,
  subscriptions, notifications and one-shot operations are supported.
  CCC changes wait for ATT completion. Eight client subscriptions are available;
  explicit value handles still discover their declaration so CCC discovery cannot
  cross into the next characteristic. Client payloads are bounded by the module
  buffer and ATT limits; queued notification payloads are limited to 255 bytes.
- Four local services and twelve characteristics, with prepared values up to 255
  bytes. The first advertisement freezes the database. Reads return the prepared
  value immediately; writes replace it and queue an event. Encryption and
  authentication permissions are supported. Local prepared/offset, signed,
  reliable/extended writes and application authorization flags are rejected.
  Updates notify or indicate according to the peer's CCC selection. Operations
  231–233 select a public/static-random identity, legacy advertising mode and
  intervals, and the prepared GAP device name. Mode 0 is non-connectable, mode 1
  directed connectable and mode 2 undirected connectable (the default).
  Address changes are refused while a link, attempt or bond exists.
- Four unsolicited event slots, a separate job completion and one authenticated
  export slot per native bond. Advertising pressure cannot discard a completion
  or another peer's bond export. SDK calls and snapshots use the CYW43 async
  context lock in both HTTP background and HTTPS polling builds. Native callbacks
  only capture data; Berry delivery remains cooperative. Native startup/restart
  and connection drains have deadlines; a failed drain disables reuse until reboot.
- Closing the VM revokes delivery before draining connections and native callbacks,
  clears prepared values and unregisters the dynamic database, while preserving
  native scan demand and local identity/name/advertising policy. A new VM may build
  another database. A bonded peer may need to clear its GATT cache after a schema
  change; this adapter does not provide Service Changed cache invalidation.

Pairing uses the staged PIN for input and a fixed displayed passkey; numeric
comparison and Just Works are automatically confirmed, as on ESP. Secure Connections
and legacy pairing use the native stack. A secured ATT failure pairs and retries once.
The SDK native random source supplies local security roots; it is not replaced by an
additional software DRBG. Four native RAM bonds are retained without eviction.
Successful authentication exports a byte-defined `PBK1` record with named key fields,
security roots, the exact SDK revision, controller identity and configured local identity.
`BLE.store()` validates the record, key sizes/security flags and identity, and refuses
incompatible roots, key replacement or eviction. The first restore cycles the idle BLE
controller to derive native identity keys from the restored roots; readiness returns
asynchronously. Wi-Fi is independent of that host power cycle. Scripts own persistence
across reboot and must protect exported credentials. Records are incompatible with other
backends. No vendor flash bond partition or SDK flash TLV bank is used.

`BLE.info()` includes live security, sampled RSSI, peer identity/IRK and negotiated MTU.
Transmit power is zero because this adapter has no qualified native power readback.
The SDK's BTstack supplemental licence is scoped to the Raspberry Pi products named in
[`LICENSE.RP`](https://github.com/raspberrypi/pico-sdk/blob/079c6f39023649b154152db30f1d781e884879bc/src/rp2_common/pico_btstack/LICENSE.RP),
including Pico W/WH and Pico 2 W/WH; preserve the SDK and BTstack notices in distributions.

Native builds establish compilation and image fit only. Radio operation, pairing and
bond round trips, dense advertising, notification pressure, VM restart, runtime RAM
headroom, Wi-Fi coexistence, flash writes and OTA still need hardware qualification.
