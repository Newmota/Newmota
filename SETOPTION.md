# SetOption support

Newmota accepts Tasmota-style `SetOption` commands. This page lists the slots that
currently affect Newmota behavior. Features and drivers vary by build; an option has
an effect only when its corresponding feature and hardware are available.

Query a slot with `SetOption8` or `SO8`, and change it with, for example,
`SetOption8 1` or `SO8 0`. Boolean slots accept `0`/`OFF` and `1`/`ON` and report
`OFF` or `ON`. Slots 32–49 are numeric bytes and normally accept `0..255`.
Settings are saved. A query without a slot lists boolean values; `SetOption 0`
lists enabled booleans and numeric slots, `SetOption 1` lists enabled booleans,
and `SetOption 2` lists numeric slots.

The command accepts slots 0–177, but accepting or reporting a slot does **not**
mean its Tasmota behavior is implemented. Unlisted slots are stored for
compatibility and have no implemented effect. Slot 34 can be queried but cannot
be changed through this command. The general boolean list reports through slot
165; query slot 177 directly.

## Implemented slots

Defaults below are for fresh settings. Existing saved settings are preserved.
Unless otherwise stated, boolean defaults are `0`.

| Slot | Default | Effect |
| --- | --- | --- |
| 0 | 1 | Save and restore Power and Light state. Turning this off clears the saved Power state. |
| 3 | 0 | Enable MQTT. |
| 4 | 0 | Use the command-specific MQTT response topic instead of `RESULT`. |
| 8 | 0 | Select temperature units: `0` Celsius, `1` Fahrenheit. |
| 10 | 1 | Mark the previous MQTT topic Offline when changing the relevant configuration. |
| 11 | 0 | Swap single- and double-press relay actions when one button and two relays are configured. |
| 13 | 0 | Enable immediate button action. |
| 15 | 1 | Assign PWM1–PWM5 to Light; `0` assigns them to raw PWM. Takes effect after restart. |
| 16 | 0 | Reverse the direction of the addressable strip clock scheme. |
| 17 | 0 | Use decimal color responses instead of hexadecimal. |
| 21 | 0 | Allow the supported primary energy drivers to observe measurements when all logical Power outputs are off. Does not turn an output on. |
| 24 | 0 | Select pressure units: `0` hPa, `1` mercury units; slot 139 selects which mercury unit. |
| 26 | 0 | Always number MQTT POWER topics, including a single output. |
| 31 | 0 | Disable network and MQTT problem blinks on the link LED. |
| 33 | 5 | Maximum-power protection retry count, when `USE_ENERGY_MAX_POWER` is enabled. With that feature, updates accept `1..250`; other values return the retained value. |
| 37 | 0 | Bits 0–6 (0–119) reorder the PWM Light channels with Tasmota's permutation; a value that would leave one of the Light's outputs dark keeps the default order, and WS2812 strips use `PixelType`. Bit 128 splits RGB and white power/brightness endpoints on four- or five-channel Lights; slot 68 takes precedence. Takes effect after restart. |
| 39 | 0 | Primary CSE invalid-power retry threshold; also the hardware-total plausibility bandwidth in whole kWh when `USE_ENERGY_COUNTER_FILTER` is enabled. For the filter, `1..127` enables the bandwidth and other values disable it. Primary CSE replaces zero with 128 at startup. |
| 42 | 90 | Primary energy overtemperature threshold in Celsius (`0..255`), when `USE_ENERGY_OVERTEMP` and an opted-in driver are enabled. |
| 52 | 0 | Append the timezone offset to valid formatted `Time` command responses. |
| 60 | 0 | Select normal loop sleep instead of dynamic sleep. |
| 68 | 0 | Give each Light channel an independent Power endpoint. Takes effect after restart. |
| 72 | 0 | Enable the primary hardware-total reference policy when `USE_ENERGY_HARDWARE_TOTAL` is enabled. |
| 73 | 0 | Decouple button actions from their local Power outputs. |
| 75 | 0 | Use simple MQTT group topics. |
| 90 | 0 | Publish only JSON Power results over MQTT; enabling it also disables Power retain. |
| 104 | 0 | Disable retained MQTT publications. |
| 105 | 0 | White blend on PWM RGBW and RGBCW Lights: while white is off, the common part of RGB moves to the white channels at the `RGBWWTable` white level; RGBCW splits it by CT. A fifth `RGBWWTable` value of 0 sets this option at the next restart, as in Tasmota. Applies with the next Light change. |
| 115 | 1 | Start or stop BLE when included in the build. |
| 127 | 0 | Disable Wi-Fi power saving where supported. Platform and Bluetooth coexistence limits apply. |
| 129 | 0 | Select phase arrays instead of phase sums for primary legacy ENERGY output; a single channel remains scalar. |
| 151 | 0 | Start Matter after the next restart when included in the build. |
| 135 | 0 | Disable automatic LVGL startup and splash when a panel is open; Berry can still start LVGL explicitly. |
| 139 | 0 | With slot 24 enabled, select `1` for inHg instead of `0` for mmHg. |
| 150 | 0 | Force phase presentation of common voltage and frequency in energy output. |
| 153 | 0 | Disable Berry `autoexec.be` on the next startup. Takes effect after restart. |
| 162 | 0 | Exclude newly accounted export from compatible signed energy projections when enabled. The change is prospective. |
| 177 | 0 | Keep 5-minute sensor history averages instead of hourly averages, using more memory. |

For details and feature limits, see the [command guide](src/app/core/command/README.md),
[energy guide](src/app/drivers/energy/README.md), [LED strip guide](src/app/drivers/led_strip/README.md),
[LVGL guide](src/app/services/berry/modules/lvgl/README.md), and
[Tasmota differences](TASMOTA_DIFFERENCES.md).
