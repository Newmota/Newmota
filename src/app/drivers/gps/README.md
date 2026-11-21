# UBX GPS

Default-off, application-only `USE_GPS` selects Time and the shared hardware UART
owner. The receiver protocol is UBX only. The initial target is the older
CFG-MSG/CFG-RATE interface used by u-blox 6–8 receivers; newer generations are not
qualified. All five platform families can select it where their native auxiliary
UART supports the chosen pins. Bouffalo BL616/BL618DG cannot select it.

```yaml
name: gps-clock
inherit: esp32c2-2m
features:
  USE_GPS: true
  USE_DS3231: true # Optional battery-backed UTC retention.
```

Configure exactly one GPS RX and one GPS TX through GPIO/Template, then restart.
The RX variants select baud rather than separate receivers:

| Function | GPIO value | Baud |
| --- | --- | --- |
| GPS RX1 | 4352 | 9600 |
| GPS RX2 | 4353 | 19200 |
| GPS RX3 | 4354 | 38400 |
| GPS TX | 4384 | Same as RX |

Connect receiver TX to the configured RX pad and receiver RX to TX, with common
ground and compatible signal levels. Both directions are required, using 8N1.
Choose native UART routes described in the [platform guide](../../../platform/README.md).
Configured GPS pads remain reserved from Berry GPIO/serial and other owners; console
and native UART conflicts fail without taking over the existing owner.

Setup disables the six ordinary NMEA sentence outputs, enables NAV-TIMEUTC,
NAV-STATUS, NAV-POSLLH and NAV-VELNED, and requests 1 Hz navigation. Changes are
volatile: receiver baud and saved configuration are untouched. Each command uses
bounded partial writes, an ACK deadline and at most three attempts. Failed setup
retries after 30 seconds. After setup, ten seconds without recognized UBX navigation
starts a 30-second backoff and another setup attempt. Reception continues during setup.
`setup_sent` records the completed ACK sequence, not read-back verification of every
setting: CFG-MSG ACKs identify the command type, not its target message.

The fixed-storage streaming parser checks checksums and exact known message lengths.
Unknown frames are skipped; partial frames expire after 1.5 seconds. Each app iteration
reads at most 256 bytes. UART overflow/error discards partial input and invalidates
observations. UTC requires valid TOW, week and UTC flags, a valid Gregorian date no later than
2099 above the shared trusted epoch, and a valid signed nanosecond fraction.
Leap-second `60` is skipped until representable UTC arrives. UTC does not require a
position fix. Position and velocity need a valid position fix with matching `iTOW`.

Fresh accepted GPS time takes priority over NTP for five seconds after each sample.
NTP resumes when GPS expires or invalid UTC/transport loss is observed. Holdover
remains valid when GPS disappears. Manual `Time <epoch>` blocks both automatic
sources; `Time 0` releases that hold. RTC remains a bootstrap source. Accepted GPS
can seed an optional RTC immediately. Successful or unchanged checks wait one hour;
failed updates retry on an accepted GPS sample after at least one minute. NTP retains
its existing chip update behavior. Manual time never programs
the chip. No PPS input or fixed serial-latency correction is applied; receiver time
accuracy is distinct from system synchronization accuracy.

`Status 7` includes `Source: "gps"` when GPS supplied the latest accepted time.
Berry `rtc('source')` uses the same source. `StatusTIM.GPS` is retained diagnostics:

- `State`, `Baud`: binding/UART/setup state and requested rate.
- `Time`: receiver UTC `Valid`, `AgeMs`, and `AccuracyNs`.
- `Fix`: positional `Valid`, receiver fix `Type`, and GPS-week `ITOWms`.
- `Position`: `Valid`, `AgeMs`, `LonE7`/`LatE7` in degrees × 10⁷, `AltMm`
  above mean sea level, and accuracy `HAccMm`/`VAccMm`.
- `Velocity`: `Valid`, `AgeMs`, ground `SpeedCmS`, and `HeadingE5` in degrees × 10⁵.
- `Errors`: checksum, length, timeout, rejected navigation, setup retries/NAKs,
  dropped UART bytes and UART error counters.

Validity expires after five seconds. `AgeMs` is `4294967295` before an observation or
after invalidation. Retained numeric fields must be interpreted with their `Valid`
flag. Receiver UTC validity can remain true during manual hold even though the shared
clock rejects automatic samples. Reading status performs no UART transactions.

The port follows Tasmota's UBX message choices and baud variants, with explicit wire
decoding and documented validity bits. NMEA parsing, PPS, automatic persisted
latitude/longitude changes, Sensor60 commands, flash logging/GPX, TCP bridging and
NTP serving are outside this feature.
The independent optional [NTP server](../../services/time/README.md) can serve the
shared clock, including accepted GPS UTC, with its own source-quality policy.

Pure parser/time checks and native compilation do not establish hardware operation.
Qualification remains open for startup/configuration, actual baud, UTC before a
position fix, reset/reacquisition, GPS/NTP failover, manual hold, and RTC seed/battery
restart. Protocol reference: [u-blox 8/M8 receiver and protocol manual](https://content.u-blox.com/sites/default/files/products/documents/u-blox8-M8_ReceiverDescrProtSpec_UBX-13003221.pdf).
