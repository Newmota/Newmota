# LED driver chips

Tuya RGBCW bulbs often drive their LEDs through a two-wire chip instead of PWM. Each chip
has its own build feature, as in Tasmota, and selects the shared `USE_LED_DRIVER` bus. All
of them default off. Enable one in a personal environment, for example:

```yaml
name: bulb-cb2s
inherit: cb2s-2m
features:
  USE_BP5758D: true
```

| Feature | Chip | GPIO functions |
| --- | --- | --- |
| `USE_BP5758D` | BP5758D | `BP5758D CLK`, `BP5758D DAT` |
| `USE_BP1658CJ` | BP1658CJ | `BP1658CJ CLK`, `BP1658CJ DAT1`–`DAT16` |
| `USE_SM2335` | SM2335; OpenBeken drives SM2235 and SM2185N the same way (untested here) | `SM2335 CLK`, `SM2335 DAT1`–`DAT16` |
| `USE_SM2135` | SM2135 | `SM2135 CLK`, `SM2135 DAT1`–`DAT10` |

Tasmota templates that assign these functions work unchanged. The chip owns the Light as
five channels (red, green, blue, cold and warm white). Commands, power, MQTT, the web page and
Berry work as for a PWM Light, including `SetOption37` channel order, `RGBWWTable` and
`SetOption105`. Configured PWM pins stay raw outputs. A chip together with a WS2812 strip
disables the Light and logs why, and so does more than one chip.

The bus is open drain on every family: a high level releases the pad to its internal
pull-up. Bits are clocked with 2 µs half periods (SM2135: 4 µs) on the application task; a frame takes
under 1 ms and is sent only when its bytes change. The chips acknowledge every byte, but
the ACK is not checked, as in Tasmota and OpenBeken. When all channels are off, the chip
goes to sleep or standby, and the next non-zero frame wakes it.

## Currents

`LightCurrent <rgb>,<white>` sets the maximum current of the RGB and the white outputs; one
value sets both. It is a Newmota command; Tasmota has no equivalent. The setting is saved and
applies without restart. Without a setting, or with one outside the chip's range (saved
for another chip), the chip uses Tasmota's value.

| Chip | Unit | Range | Default |
| --- | --- | --- | --- |
| BP5758D | mA | 1–93 | 14, 14 |
| BP1658CJ | code c: RGB c × 10 mA, white c × 5 mA | 0–15 | `DAT<n>`: n−1 for both |
| SM2335 | code c: RGB (c+1) × 10 mA, white (c+1) × 5 mA | 0–15 | `DAT<n>`: n−1 for both |
| SM2135 | code c: (c+2) × 5 mA | RGB 0–7, white 0–10 | the `DAT<n>` preset |

As in Tasmota, the `DAT` index of BP1658CJ and SM2335 selects their current. On BP5758D an output
takes the RGB or the white current by the channel it carries after `SetOption37`, as OpenBeken
does. BP1658CJ and SM2335 have one current for OUT1–OUT3 and one for OUT4–OUT5.

## SM2135 presets

As in Tasmota, the `SM2135 DAT` instance selects how the bulb is wired and driven. `LightCurrent`
replaces only the preset's currents. The current byte puts RGB in the high nibble, as Tasmota
does. OpenBeken puts white there; if a bulb's RGB and white currents look swapped, report it.

| DAT | Colour order | RGB / white | Sends |
| --- | --- | --- | --- |
| 1 | GRB | 20 / 15 mA | RGB or white, never both (Action LSC) |
| 2 | BGR | 20 / 15 mA | RGB or white, never both (Polux) |
| 3 | GRB | 20 / 30 mA | both |
| 4 | BGR | 20 / 30 mA | both |
| 5 | GRB | 45 / 60 mA | both (LE LampUX) |
| 6 | BGR | 45 / 60 mA | both |
| 7 | BRG | 15 / 40 mA | all five values in one frame (Fitop) |
| 8 | BGR | 20 / 15 mA | both; reserved in Tasmota, which runs it this way |
| 9 | GRB | 20 / 15 mA | RGB or white, and zeroes the other (Qualitel) |
| 10 | BGR | 20 / 15 mA | RGB or white, and zeroes the other |

The white levels follow a 1 ms pause after the white mode frame, as in Tasmota.
