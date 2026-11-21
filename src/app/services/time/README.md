# Shared UTC and optional NTP server

The shared Time service accepts NTP, optional UBX GPS, optional RTC bootstrap, and
manual `Time <epoch>` samples. `Time 0` releases the manual hold. Civil timezone
settings affect local presentation; NTP replies always contain UTC.

Network-capable applications select the NTP client by default. Configure its three
servers with `NtpServer1` through `NtpServer3`; `0` disables one entry. Setting
`features.USE_NTP: false` removes the client while retaining the shared clock.

`USE_NTP_SERVER` is a separate default-off feature available only in personal
application variants. It selects Wi-Fi and Time independently of GPS and the NTP
client. Selecting it explicitly trusts valid manual/RTC time as a local fallback.

```yaml
name: lan-clock
inherit: esp32c2-2m
features:
  USE_NTP_SERVER: true
  USE_DS3231: true # Optional RTC bootstrap; no GPS required.
  USE_NTP: false  # Optional offline operation.
```

The listener runs on IPv4 UDP port 123 while the station is connected or the setup
AP is active. Point LAN clients at that interface's device address. Replies use
the receiving interface and local destination address. The listener owns its own
socket; NTP client DNS, exchanges and cancellation remain independent. There is no
runtime server command, persisted server switch, DHCP advertisement, authentication,
IPv6, broadcast/multicast serving, symmetric-peer mode, or extension/MAC support.

Only exact 48-byte NTPv3/v4 mode-3 requests are accepted. Replies copy the client's
transmit timestamp unchanged into the originate timestamp, including a zero token.
The lwIP receive callback samples arrival and departure on the native network
context, using one coherent copied clock publication for the whole request.
Processing retains no request queue and emits at most four replies per second
globally. Invalid, excess and failed requests count as drops. Request/reply storage
is bounded; native UDP packet allocation may fail without disrupting the client.

| Latest accepted source | Advertised quality |
| --- | --- |
| Fresh GPS | Stratum 1, reference `GPS`, for five seconds after accepted UTC; observed invalid UTC/UART loss ends freshness on the next clock publication (50 ms cadence) |
| NTP | Upstream stratum plus one, upstream leap flags, selected upstream IPv4 reference; expires after one hour plus the five-second refresh deadline |
| RTC or manual | Explicit local fallback, stratum 10 and reference `127.127.1.0`; no automatic-source provenance or freshness claim |
| Never synchronized or stale automatic source | LI 3, stratum 16, zero reference/receive/transmit timestamps; client token remains echoed |

An upstream stratum-15 sample can set the shared clock but cannot produce a valid
downstream stratum. Loss of GPS/NTP leaves the shared civil clock valid in holdover;
the server advertises unsynchronized time until a suitable source is accepted.
It never silently promotes automatic holdover to the local fallback policy.

The reference timestamp remains the original accepted sample, independently of
running-clock rebasing and monotonic counter wraps. Precision is -9 (approximately
2 ms clock-reading resolution). All sources receive a one-second dispersion
allowance; NTP adds upstream dispersion and the measured local exchange duration
to upstream root delay. Dispersion grows using the RFC 5905 nominal 15 ppm model.
These are conservative protocol policy values and an unqualified oscillator
assumption, not measured accuracy bounds. Serial reception, scheduling and Wi-Fi
latency remain unqualified. Receiver `AccuracyNs` is never used as server accuracy.
There is no PPS, oscillator discipline, fixed serial correction or measured
frequency calibration. Clients may reject a sample because its root distance is
too large, including a local fallback whose uncertainty has grown over time.

`Status 7` adds compact `StatusTIM.NTPServer` diagnostics:

- `State`: `waiting`, `listening`, or `failed` (native listener creation/bind).
- `Quality`: `none`, `gps`, `ntp`, `local`, or `holdover`; interpreted with `State`.
- `Age`: seconds since the latest accepted sample, saturating at `4294967295`;
  that value also means no sample has been accepted.
- `Req`, `Sent`, `Drop`: saturating lifetime request, successful reply and drop
  counters. A packet rejected by lwIP before delivery is outside these counters.

`StatusTIM.Source` continues to identify the actual latest accepted sample.
Selecting this feature proves no hardware timing or interoperability result.
Qualification remains open for station/AP clients, concurrent NTP refresh,
GPS/NTP loss and recovery, RTC battery restart, manual hold and malformed/flooded
traffic. Protocol references: [NTPv4, RFC 5905](https://www.rfc-editor.org/rfc/rfc5905)
and [SNTP server operation, RFC 4330 section 6](https://www.rfc-editor.org/rfc/rfc4330#section-6).
