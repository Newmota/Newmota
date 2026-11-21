# External real-time clocks

Select the physical chip with one of these default-off application features. Each
selects `USE_RTC_CHIPS`, `USE_TIME`, and `USE_I2C` on all five platform families.
Safeboot excludes them. Configure SCL/SDA and external pull-ups using the
[I2C guide](../i2c/README.md).

| Feature | Physical chip | Address | Invalid-time indication |
| --- | --- | --- | --- |
| `USE_DS3231` | DS3231 | `0x68` | OSF or disabled battery oscillator |
| `USE_DS1307` | DS1307 | `0x68` | CH (halted oscillator); no historical oscillator-stop detector |
| `USE_BM8563` | BM8563 | `0x51` | VL or STOP |
| `USE_PCF85063` | PCF85063A | `0x51` | OS or STOP |
| `USE_PCF85363` | PCF85363A | `0x51` | OS, STOP, stopwatch mode, or external TS stop control |
| `USE_RV3028` | RV-3028-C7 | `0x52` | PORF |
| `USE_RX8010` | RX8010SJ | `0x32` | VLF or STOP |
| `USE_RX8025` | RX-8025T | `0x32` | VLF, VDET, or RESET |
| `USE_RX8130` | RX8130CE | `0x32` | VLF or STOP |

Tasmota spells RX8130CE as `USE_RX8030`; this spelling is accepted as an alias
for `USE_RX8130`. It does not name a separate Epson model. The Tasmota discussion
that introduced it identifies the [RX8130CE hardware](https://github.com/arendst/Tasmota/discussions/23855).
`USE_RX8025` supports the T variant, whose register addressing differs from SA/NB.
DS1307 uses its own backend; DS3231 status registers are SRAM on a DS1307.

An address acknowledgement or calendar contents cannot safely distinguish models
sharing an address. Selecting a feature asserts that an answering device has that model. Builds reject multiple physical models at the
same address, even if different buses are intended, since no per-bus model binding
exists. Features at different addresses can coexist; the owner selects one clock.
It scans buses in order, then DS3231/DS1307, BM8563/PCF85063/PCF85363, RV3028, and
RX8010/RX8025T/RX8130CE. Candidate addresses are reserved before reads; failed
candidates release their leases. An invalid calendar still establishes presence so
accepted automatic time can repair it. Selection happens at startup without hotplug
or periodic source switching.

Selected-chip preparation enables backup switching where required; it never invents
a calendar. Preparation failure retains the selection with `io_error`, prevents RTC
bootstrap, and retries preparation on the next eligible automatic sample. Successful
preparation runs once per owner initialization.

A trusted UTC calendar can bootstrap an invalid shared clock. It never replaces valid
manual or network time and does not delay NTP. Manual `Time` values keep their existing
hold and are not written to the chip. `Time 0` releases the hold while retaining the
current time. Every accepted NTP sample checks the selected chip; a valid calendar
within two seconds is left unchanged. Otherwise a bounded synchronous write updates
the RTC. Write failures leave network time valid and are retried on the next accepted
NTP sample. Accepted [GPS UTC](../gps/README.md) can also seed the chip immediately;
successful or unchanged GPS-driven checks wait one hour; failed updates retry on the
first accepted GPS sample at least one minute later. Without NTP or GPS, the owner
only prepares the selected chip and reads its existing calendar.

For a clock that boots from an already-set RTC without NTP synchronization, select:

```yaml
features:
  USE_DS3231: true
  USE_NTP: false
```

NTP remains enabled by default in network-capable applications.

Reads use one coherent calendar burst, strict BCD and Gregorian validation,
and correct 12-hour AM/PM conversion. Shared pure calendar conversion handles the
different weekday encodings and field orders; chip-specific protocols retain their
own flag and stop behavior. The calendar range is 2000–2099, additionally restricted
by the shared minimum valid epoch. Unsupported century bits are rejected rather
than applying Tasmota's historical year-offset guesses. DS3231/DS1307 writes use
24-hour mode; other chips preserve a configured 12-hour mode so alarm hour encodings
remain consistent. Alarms, temperature, SQW/CLKOUT, user RAM, EEPROM user storage,
and new mutation commands are outside the calendar feature.

Programming verifies the complete calendar before reporting success. DS1307 keeps
CH set while writing and verifying; BM8563, PCF85063A, PCF85363A, RX8010SJ, and
RX8130CE similarly keep STOP set. An incomplete write leaves those counters stopped
when bus access succeeds; the next accepted automatic sample retries. PCF85363A
clears only its prescaler and writes hundredths through years in one access. External
TS stop mode must be disabled before using it; the driver preserves that board policy.
RX8025T uses RESET, but its manual describes automatic RESET cancellation on I2C
conditions/timeouts: persistent halt after an interrupted operation is not guaranteed.
DS3231 and RV3028 also have no software halt covering programming. Read-back cannot
guarantee atomicity across power loss or an interrupted transaction; recovery requires
a fresh authoritative sample. Unrelated controls and pending W0C interrupt flags are
preserved. RX8010SJ sets only the required reserved bytes, retaining digital calibration
and interrupt routing.

RV3028 factory backup switching is disabled. Preparation reads the EEPROM backup
byte and its live mirror, enables direct switching only when disabled, and enables
the manufacturer's required fast-edge detection. Existing direct/level switching,
charger enable/resistance, calibration, and other bits are preserved. A changed EEPROM
byte is written once and read back, with auto-refresh temporarily disabled and bounded
busy polling. An unchanged byte is not rewritten. Setup requires stable VDD of at least
1.5 V for EEPROM writes; the battery must remain below normal VDD for direct switching.
Use preconfigured level switching when appropriate for similar VDD/battery voltages.
Password-protected configuration is not unlocked. EEPROM setup failure is reported as
`io_error`; backup retention must be qualified on the actual module.

RX8130CE requires INIEN to be set at least once after power-on to enable its backup
switch latch. Preparation briefly sets INIEN with charging disabled and restores the
configured interface/charger bits; it never enables charging. VDD must be above the
configured VDET1 threshold (normally approximately 2.75 V) during this sequence,
since INIEN gates I2C below it. DS1307 has no historical loss-of-oscillation flag:
a running, plausible calendar is its strongest available validity evidence.

`Status 7` adds `Source` (`none`, `manual`, `ntp`, `gps`, or `rtc`) and `Valid` to `StatusTIM`.
Source identifies the latest accepted sample's origin, independent of current network
connectivity. Berry's existing `rtc()` map adds `source`; `rtc('source')` returns the
same string. Optional `StatusTIM.RTC` reports `Chip`, one-based `Bus` (zero if no chip
was selected), `State`, `Valid`, and `Write`. State distinguishes `unavailable`,
`conflict`, `invalid`, `io_error`, and `ready`. Write reports `never`, `unchanged`,
`success`, `range_error`, `io_error`, or `verify_error`; it is separate from the most
recent observed calendar validity. Diagnostics use retained observations, not bus
transactions on every Status query.

These drivers are compile-tested only; electrical operation, battery retention, oscillator
recovery, and power interruption during programming need hardware qualification.
Protocol references: [Analog Devices DS3231 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ds3231.pdf).

[DS1307](https://www.analog.com/media/en/technical-documentation/data-sheets/DS1307.pdf),
[BM8563](https://www.belling.com.cn/media/file_object/bel_product/BM8563/datasheet/BM8563_V1.1_cn.pdf),
[PCF85063A](https://www.nxp.com/docs/en/data-sheet/PCF85063A.pdf),
[PCF85363A](https://www.nxp.com/docs/en/data-sheet/PCF85363A.pdf),
[RV3028 application manual](https://www.microcrystal.com/fileadmin/Media/Products/RTC/App.Manual/RV-3028-C7_App-Manual.pdf),
[RX8010SJ](https://download.epsondevice.com/td/pdf/app/RX8010SJ_en.pdf),
[RX8025T manufacturer manual mirror](https://device.report/m/9762b149ae9f4cda76ef4b56c6d84e77e31052d1c51ce1f043cee062e1c306a5.pdf),
and [RX8130CE](https://download.epsondevice.com/td/pdf/app/RX8130CE_en.pdf).
