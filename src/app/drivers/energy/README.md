# Energy monitoring

Energy monitoring uses **one active provider**, selected by the normal Module, Template
and GPIO configuration. A provider can have several phases or measurement channels when
its hardware requires them. Several drivers may be compiled into the same firmware;
that does not create several running meters.

## Set up a metering plug

1. Build firmware with the product's meter driver enabled. Energy drivers are optional
   and off by default; a template cannot add missing firmware code.
2. Select the product's module or paste its template in Configuration. The template
   assigns the button, relay, indicator and meter signals together.
3. Apply the configuration and issue `Restart 1`. GPIO changes do not restart implicitly.
4. Check Home or `Status 8`, then calibrate against a known reference if necessary.

No driver number or physical `EnergyConfig` tuple is required. Use the
GPIO editor to select meter functions just as for relays and buttons. Template positions
follow the architecture's slot map; `GPIO<n>` uses physical pin numbers. See
[GPIO templates](../../../../TEMPLATE.md) for the format and a Beken carrier example.
Actual product wiring, available UART routes and reserved pins still apply.

| Meter | Build selection | Template functions |
| --- | --- | --- |
| HLW8012 | `USE_HLW8012` | HLW8012 CF; optional CF1 and SEL |
| BL0937 / HJL | `USE_BL0937` | HJL CF; optional CF1 and SEL |
| CSE7759 / CSE7766 / HLW8032 | `USE_CSE7766` | CSE7766 RX |
| BL0939 | `USE_BL0939` | BL0939 RX and serial TX |
| BL0940 | `USE_BL0940` | BL0940 RX and serial TX |
| BL0942 | `USE_BL0942` | BL0942 RX variant and serial TX |
| CSE7761 | `USE_CSE7761` / `USE_CSE7761_POWCT` | CSE7761 model RX variant and TX |
| MCP39F501 | `USE_MCP39F501` | Model RX/TX and required reset signal |
| PZEM004T | `USE_PZEM004T` | PZEM004 RX/TX |
| PZEM AC / DC | `USE_PZEM_AC` / `USE_PZEM_DC` | PZEM016 / PZEM017 RX/TX |
| Serial Modbus meters | Corresponding `USE_SDM*`, `USE_DDS*`, `USE_FIF_LE01MR`, `USE_IEM3000`, `USE_ORNO_WE517` or `USE_MODBUS_ENERGY` | Model RX/TX and optional transmit enable |
| ADE7953 variants | Corresponding ADE7953 model feature | Explicit model marker and model bus/signals |
| ADE7880 | `USE_ADE7880` | Explicit IRQ marker, reset and I2C signals |
| BL0906 / V9240 | `USE_BL0906` / `USE_V9240` | Model RX/TX |
| Dummy / Berry | `USE_ENERGY_DUMMY` / `USE_BERRY_ENERGY` | Source Option A2 / Option A9 |
| TuyaMCU | `USE_TUYA_MCU_ENERGY` | Tuya Tx/Rx and `TuyaMCU 31,<dp>` or `36,<dp>`; see [TuyaMCU](../tuya/README.md#energy) |

`USE_BL09XX` selects all three BL09xx chips. Model-specific features remain independent.
Shared I2C wiring alone does not identify an energy chip. Do not assign two different
meter signatures: conflicting selections leave the provider closed and log the problem.
Functions missing from the firmware are retained as inactive template assignments and
reported; they do not silently activate another model.

### Pulse meters

CF selects the meter model. CF alone provides power and energy. CF1 without SEL provides
voltage; CF1 with SEL provides alternating voltage/current measurements. SEL and inverted
SEL encode the physical selection polarity. The driver uses the shared edge-capture owner;
it does not take relay ownership. Beken GPIO10/11 console use is relinquished when the
resolved configuration claims those pins. Bootloader output during reset is unchanged.

### BL09xx

| Chip | Channels | UART | Default power / voltage / current coefficients |
| --- | --- | --- | --- |
| BL0939 | 2 | 4800 8N1 | 713 / 17159 / 266013 |
| BL0940 | 1 | 4800 8N1 | 1430 / 33000 / 275000 |
| BL0942 | 1 | 4800–38400 8N1 | 596 / 15187 / 251213 |

The numbered BL0942 RX function encodes the source's baud and sign choices: variants
1–4 select 4800, 9600, 19200 and 38400 baud with absolute power; variants 5–8 select
those rates with signed power. This selects the UART framing; it does not program the
chip's baud register. Negative signed power accumulates export. There is no software
UART fallback; the selected native route must support the framing.

## Calibration and device configuration

Use the familiar `PowerCal`, `VoltageCal`, `CurrentCal` and corresponding `PowerSet`,
`VoltageSet`, `CurrentSet` commands. Frequency calibration exists only where the source
driver supports it. Calibration command indices identify phases of the provider.
A `*Set0` reference applies to all supported phases.

```text
VoltageSet 230
PowerSet 60
CurrentSet 263
PowerCal
VoltageCal
CurrentCal
```

Power references are W and voltage references are V. Current references follow the
selected source driver: ordinary metering drivers use mA, while V9240 has its own native
signed calibration semantics. Fractional input and rounding are driver-specific. Paired
`PowerSet watts,volts` or `CurrentSet watts,volts` derives power/current calibration from a
known resistive load and the measured voltage. A fresh usable raw reference is required.

Payload `1` is a reset only for drivers whose source defines that reset; it must not be
assumed to reset every driver. Coefficients and calibration defaults belong to the selected
model. Calibration does not replace the wiring template and does not erase accumulated
energy. Failed durable updates retain the previous coefficients.

`EnergyConfig` retains driver-specific meanings, such as ADE7880 register configuration,
ADC parameters and Dummy simulated values. It is **not** a command for selecting a driver
or assigning pins. As in Tasmota, the generic Modbus meter reads its register map from
`/modbus.json` when the template selects it; the last map that was read is kept when the
file is missing. `ModuleAddress` and other device controls are available only where the
selected protocol implements them. A successful queued hardware command is not proof that
the chip applied it; check its completion and subsequent readings.

ADE calibration files use `/calib.dat` on LittleFS. RULES-provided `file#calib.dat` fallback
is unavailable because Newmota does not implement RULES. Shelly 3EM v1 factory calibration
is not supplied. BL0942 frequency comes directly from its native period; a stored frequency
coefficient does not make that measurement calibratable.

## Readings, accounting and protection

The selected provider supplies channel measurements and exact retained import/export
accounts. Channels are not unrelated meters. Home shows the meter as one sensor card with
all its phases; see [energy readings](../../services/web/README.md#energy-readings).
Status and MQTT carry Tasmota's ENERGY object; the generic Modbus meter appends its user
fields there under their JSON names and BL0939/BL0940 add `"BL09XX":{"Temperature":t}` (BL0906:
`"BL0906"`),
as Tasmota does. That temperature adds `TempUnit` to the message when no sensor did.

Optional accounting features add daily/calendar totals, tariff handling, Total compatibility,
precision and period reporting. Invalid civil time does not invent dated consumption;
energy remains unassigned until the time owner's contract allows attribution. Physical
import/export accounts and signed compatibility projections are separate values.

Optional protection features retain the source Power/SetOption21 measurement gates and
MaxPower/MaxEnergy semantics. An ordinary relay plug needs no separate protection-target
setup. Global overtemperature requires a real temperature observation and
an opted-in driver; a meter without a temperature register does not fabricate one.
Software protection and reported relay intent do not establish physical cutoff.

## Retained data

Energy data lives in `/.newmota/energy.bin`, separate from ordinary settings. It holds the
selected provider's identity, calibration, options, accounts and calendar state. As in
Tasmota, a template that names another meter keeps the stored totals, calendar, tariff and
limit settings; calibration and options return to the new driver's defaults. The same driver
on other pins keeps everything. A file in another
record format is not read, and energy starts empty. A build without the stored driver keeps
its data. Safeboot does not read or write this file. Controlled restarts checkpoint energy.

## Limits and validation

The template is portable configuration, not proof of an available native UART, ADC or bus
route. Meter channels remain bounded by the selected driver and native resources. Exact
arithmetic and storage checks do not qualify the analogue front end or calibration accuracy.

Shelly Dimmer, Shelly 2.5 ADE7953, BL6523 and Shelly 3EM v1 device-specific factory behavior
are excluded. Teleinfo, MK SKY BLU, SolaX X1 and BIOPDU are not supplied by this setup path. Their inventory identities do not imply runtime support.

Compilation and host tests are distinct from hardware qualification. Meter reads, relay
operation, console handoff, calibration, off-state behavior, restart persistence and physical
protection require board-specific testing. No hardware result follows from a successful build.
