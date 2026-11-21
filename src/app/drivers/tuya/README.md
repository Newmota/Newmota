# TuyaMCU

Many Tuya devices keep their relays, dimmers and sensors on a separate MCU that talks to the
Wi-Fi module over a serial link. `USE_TUYA_MCU` ports Tasmota's TuyaMCU v2 driver
(`xdrv_16_tuyamcu_v2.ino`). It is off by default; enable it in a personal environment:

```yaml
name: tuya-cb2s
inherit: cb2s-2m
features:
  USE_TUYA_MCU: true
```

The driver runs when the template assigns `Tuya Tx` and `Tuya Rx` (Tasmota `GPIO_TUYA_TX`,
`GPIO_TUYA_RX`). It uses 9600 baud, or 115200 with `SetOption97 1`, on the shared serial
owner. On Beken the MCU may sit on UART2 (P0 Tx, P1 Rx) or on UART1 (P11 Tx, P10 Rx), where
most Beken TuyaMCU devices connect it; UART1 then no longer carries the serial console, as
Tasmota gives up its serial log on ESP8266.

## Function map

`TuyaMCU <fnId>,<dpId>` maps one of Tasmota's function IDs to an MCU data point; `<dpId>` 0
removes it. Up to 16 pairs are saved, and a change restarts the device, as in Tasmota.
`TuyaMCU` alone lists them as `{"TuyaMCU":[{"fnId":11,"dpId":1}]}`. Without any relay and
without the dummy function 99, Relay1 is mapped to data point 1 at start.

| fnId | Function | Status |
| --- | --- | --- |
| 11–18 | Relay1–8 | Power endpoints without a pin |
| 41–48 | Relay1–8 inverted | Power endpoints without a pin |
| 1–4 | Switch1–4 | Switches without a pin |
| 21 | Dimmer | Light |
| 22 | Second dimmer | Light |
| 23 | Colour temperature | Light |
| 24 | Colour | Light |
| 25 | White | Light |
| 26 | Mode: 0 white, 1 colour | Light |
| 27, 28 | Reports, as 21 and 22; on a CT light 22 and 28 report the CT | Light |
| 51 | Low-power protocol, as Tasmota v1 | Supported |
| 61–64 | Enum1–4 | `TuyaEnum<x>` |
| 71–84 | Sensors, set points and timers | `TuyaSNS`, with `USE_TUYA_MCU_SENSORS` |
| 99 | Dummy: no default relay | Supported |
| 97, 98 | Motor direction, error | Accepted and saved, no function, as in Tasmota |
| 31–33, 36, 37 | Power, current, voltage, combined, total | Energy, with `USE_TUYA_MCU_ENERGY` |
| 34, 35 | Battery state and percentage | Accepted and saved, no function, as in Tasmota |

An MCU switch appears with its first report and then works like a GPIO switch: `SwitchMode`,
`SwitchTopic`, `SwitchText`, SENSOR and Status 8 apply. As in Tasmota it starts at the
released (high) level, a report of 1 is high, and the reported level is used without
`SwitchDebounce` filtering. It takes the place of a GPIO switch of the same number.

MCU relays behave like GPIO relays: Power commands, `PulseTime`, `Interlock`, `PowerOnState`,
buttons and switches all apply. A relay mapped to the MCU takes the place of a GPIO relay of the
same number. Power sends a data point only when it differs from the MCU's last report, and a
change the MCU reports switches Power without being sent back.

## Light

With a dimmer (21) mapped and the Light built in (`USE_PWM`), the MCU drives the Light, as
Tasmota's TuyaMCU light types:

| Mapped | Light |
| --- | --- |
| 21 | Dimmer |
| 21, 23 | Dimmer and CT |
| 21, 22 | Two dimmers, each its own Power and `Channel`; `TuyaMCU` sets `SetOption68` |
| 21, 24 | RGB |
| 21, 24, 25 | RGB and white |
| 21, 24, 23 | RGB and white with CT |

The light switches with the last mapped relay, two dimmers with the last two: those relays
become the Light's Power, as Tasmota's Light.device, so a dimmer with the default Relay1 is
`Power1`. `Dimmer`, `CT`, `White`, `Color`, `HsbColor`, `Channel`, MQTT, the web page and
Berry work as for any Light. The MCU fades on its own, so the Light sends the target at once;
`Fade`, `Scheme` and the `SetOption37` split do not apply.

Values go out only while the light is on, and only when they differ from the MCU's last
report. A colour light shows colour or white: a colour goes out as the colour data point,
after mode 1 when 26 is mapped; white goes out as dimmer and CT, or as the white data point on
RGB and white, after mode 0. Reports from the MCU change the Light without being sent back:
dimmer, CT and white only while the light is on (`SetOption54 1`: also while off) and only if
not 0 (`SetOption131 1`: 0 too). While a different value still waits to be sent, as during
`TuyaDimDelay`, a report leaves the Light alone, as for relays.

With the mode (26) mapped on RGB and white or RGB and CT, the Home light card has Tasmota's
light mode button. It switches the Light, not only the MCU: to colour with the wheel's colour,
to white with the CT on RGB and CT, or full white at the current brightness on RGB and white;
the mode and values then go out as above. On RGB and white, the White slider changes the
white level only while the light shows white.

As in Tasmota, dim values wait `TuyaDimDelay` (2000 ms) after the MCU reports the light's
relay or a dim value, and while the relay is reported off, so turning on at a new level sends
the level once the MCU has switched on.

`DimmerRange <min>,<max>` sets the MCU values of 0 % and 100 % (default 0,100; many dimmers use
0,1000 or 10,1000), saved; dimmer and white values below the minimum are never sent. CT goes out as `max` for
153 mireds down to 0 for 500. `TuyaRGB <0..3>` selects the colour format, saved:

| TuyaRGB | Format |
| --- | --- |
| 0 | `HHHHSSSSBBBB`, upper case: hue 0–360, saturation and brightness 0–1000 |
| 1 | The same in lower case |
| 2 | `RRGGBBFFFF6464`, upper case |
| 3 | The same in lower case |

## Sensors

`USE_TUYA_MCU_SENSORS` shows the MCU's value data points as one sensor, `TuyaSNS`, in SENSOR,
StatusSNS, the dashboard and Berry's `read_sensors()`:

```yaml
features:
  USE_TUYA_MCU: true
  USE_TUYA_MCU_SENSORS: true
```

| fnId | Field | Decimals of the MCU value |
| --- | --- | --- |
| 71 | `Temperature` | `TempRes` |
| 72 | `TempSet` | `TuyaTempSetRes` |
| 73 | `Humidity` | `HumRes` |
| 74 | `HumSet` | `HumRes` |
| 75 | `Illuminance` (lx) | 0 |
| 76 | `TVOC` (ppm) | 0 |
| 77 | `eCO2` (ppm) | 0 |
| 78 | `CarbonDioxide` (ppm) | 0 |
| 79 | `Gas` (%LEL) | 0 |
| 80 | `PM` (PM2.5, µg/m³) | 0 |
| 81–84 | `Timer1`–`Timer4` | 0 |

As in Tasmota, the MCU's integer has as many decimals as the resolution: 235 with `TempRes 1`
is 23.5. A field appears with the MCU's first report. Each change is published at once as
`{"TuyaSNS":{"Temperature":23.5}}` on `tele/<topic>/SENSOR`, except in the first 8 seconds
after start; with `SetOption136 1` only `TempSet`, `HumSet` and the timers are, and the rest
waits for the TelePeriod. As in Tasmota, the values stay the MCU's: `SetOption8` only names the
dashboard's unit °F, nothing is converted, `TempOffset` and `HumOffset` do not apply, and the
temperature does not become the global temperature.

## Enums

`TuyaEnum<1..4> <value>` sends an enum data point (fnId 61–64) when the value differs from the
last one sent or reported, and answers `{"TuyaEnum1":2}`. The value may not exceed the enum's
`TuyaEnumList` maximum, which starts at 0. `TuyaEnum` alone lists the mapped enums as
`{"TuyaEnum":{"Enum1":2}}`. `TuyaEnumList <1..4>,<1..31>` sets a maximum, saved, and lists them
as `{"TuyaEnumList":{"Enum1":3}}`; without a mapped enum both commands fail. A value the MCU
reports is published as `{"TuyaEnum1":2}` on `stat/<topic>/RESULT` (`TUYAENUM1` with
`SetOption4 1`), as Tasmota publishes its TuyaEnum command, and like that command not at all
when the value is above the enum's maximum.

## Energy

`USE_TUYA_MCU_ENERGY` makes the MCU the energy meter, as Tasmota's TuyaMCU energy driver,
when power (31 or 36) is mapped:

```yaml
features:
  USE_TUYA_MCU: true
  USE_TUYA_MCU_ENERGY: true
```

| fnId | Data point | MCU value |
| --- | --- | --- |
| 31 | Power | 0.1 W |
| 32 | Current | mA |
| 33 | Voltage | 0.1 V |
| 36 | Power, voltage and current in one raw data point | bytes 0–1 0.1 V, 3–4 mA, 6–7 W |
| 37 | Total import | 0.01 kWh |

The meter is the one energy provider ([energy](../energy/README.md)), so ENERGY, Status 8, the
dashboard, `EnergyToday`, `EnergyTotal` and the built energy features apply; a template
with another meter as well opens neither. Voltage and current appear only when mapped,
and every value is 0 until the MCU reports it. The MCU measures on its own, so `PowerCal`,
`VoltageCal`, `CurrentCal` and the `*Set` commands are rejected.

Without a total, energy is counted from the last reported power until the next report. With a
total mapped, the increase of the MCU's counter since start drives Today and Total instead, as
Tasmota's EnergyUpdateTotal: the first report after start and a lower value only set the start,
so energy the MCU counts while Newmota is not running is lost. Where built, `SetOption72` takes
the MCU's total as the total within the `SetOption39` bandwidth; the counted increase is not
checked. The battery data points (34, 35) are not used, as in Tasmota.

## Commands

| Command | Effect |
| --- | --- |
| `TuyaSend0` | Ask the MCU for all data points |
| `TuyaSend1 <dp>,<0/1>` | Boolean |
| `TuyaSend2 <dp>,<value>` | 32-bit value; `0x` hex accepted |
| `TuyaSend3 <dp>,<text>` | String, at most 16 bytes |
| `TuyaSend4 <dp>,<value>` | Enum |
| `TuyaSend5 <dp>,<hex>` | String given as hex |
| `TuyaSend6 <dp>,<hex>` | Raw, optionally after `0x` |
| `TuyaSend7 <cmd>,<hex>` | Any command with a hex payload, sent at once |
| `TuyaSend8` | Ask for the product information |
| `TuyaSend9` | Toggle `stat/<topic>/DpType<t>Id<d>` publications of every reported data point |
| `TuyaEnum<1..4> <value>` | Enum value, see [Enums](#enums) |
| `TuyaEnumList <1..4>,<1..31>` | Highest enum value, saved |
| `TuyaTempSetRes <0..3>` | Decimals of `TempSet` (default 1), saved; with `USE_TUYA_MCU_SENSORS` |
| `TuyaRGB <0..3>` | Colour format, see above; only with a colour data point |
| `TuyaDimDelay <ms>` | Wait for dim values after a relay or dim report; not saved, as in Tasmota |
| `DimmerRange <min>,<max>` | MCU values of 0 % and 100 %; only with this driver |

As in Tasmota, text arguments end at a space or comma.

## Messages

With `SetOption66 1` every frame from the MCU is published as `TuyaReceived` on
`tele/<topic>/RESULT` (`TUYARECEIVED` with `SetOption4 1`); `SetOption137 1` leaves out
heartbeats, Wi-Fi state, time and upgrade frames. Without it the text goes to the debug log.

The MCU's Wi-Fi LED shows 1 with the configuration access point, 3 while a restart is pending,
4 while MQTT is connected, and 2 otherwise, as in Tasmota.

A low-power MCU (function 51, mostly battery sensors) gets the product query and then the
Wi-Fi state, without heartbeat, configuration or state query, and each of its reports is
acknowledged, as Tasmota v1 does. Such devices often power the module only briefly; whether
Newmota is up in time is untested. When the MCU asks for the time, it
gets local time once the clock is valid.

## Not yet

See [Tasmota differences](../../../../TASMOTA_DIFFERENCES.md).
