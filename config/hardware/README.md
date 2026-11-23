# Hardware assignment catalog

This directory defines GPIO functions, architecture pin topologies, built-in product modules,
and display descriptor templates. Chip and board facts stay in `config/chips/` and
`config/boards/`. How the device uses this data at runtime is described in
[runtime GPIO configuration](../../src/app/core/hardware/README.md).

| Path | Contents |
| --- | --- |
| `gpio-functions.toml` | Tasmota's numeric GPIO function IDs, frozen at one upstream commit |
| `topologies/<platform>/<architecture>.toml` | Pins, Template slots, and reservations per SDK target |
| [`modules/<platform>/<architecture>/<module>.toml`](modules/README.md) | Built-in product modules |
| [`displaydesc/`](displaydesc/README.md) | Display descriptors to upload as `/display.ini` |

## GPIO function IDs

Every `[[id]]` in `gpio-functions.toml` has an explicit base number; file order never assigns
one. The list includes ESP32-only functions and the Modbus Relay range, so selecting features
never renumbers a function. A stored digest over `base:symbol\n` for every ID detects
accidental changes.

Only functions that Newmota implements have a `[[function]]` descriptor. Firmware includes
only the descriptors owned by `core` or an enabled `USE_*` feature. An owner can list
alternative features separated by `|` for shared signals such as SPI and pulse-meter SEL.

## Product modules

```toml
schema = 1

[module]
number = 1                 # 1..254; 0 is the user template, 255 is reserved
name = "Generic"
flag = 0
requires = []              # Optional USE_* features required by this module
# base = 1                 # Optional compatible base-module number

[provenance]
source = "Tasmota"
revision = "<full commit>" # Required when copied from an upstream revision
path = "tasmota/include/tasmota_template.h"

[[assignment]]
slot = 0                   # Template slot, not necessarily physical GPIO 0
function = "GPIO_USER"     # Or one of the frozen GPIO_* symbols
# instance = 1             # One-based instance; defaults to 1
```

Unassigned slots are omitted.

## Pin topologies

Each topology declares the SDK pin count, the Tasmota-compatible Template-slot map, default
digital capabilities, absent and input-only pins, strapping cautions, and the pins reserved
for flash, PSRAM, or the selected console. It names the SDK source files it was reviewed
against; ESP slot maps also record their Tasmota revision.

Interrupt capabilities follow the native GPIO edge-capture adapters; analog capabilities
list their supported external ADC routes. These capabilities do not bypass board exposure,
reservations, native trigger/framing checks or driver resource ownership.

Physical pins and Template slots are different identifiers. A module assignment names a
slot; board `gpio.exposed` and `gpio.reserved` entries name physical SDK pins. ESP32 and
ESP32-S3 slot arrays, for example, have their own ordering and gaps.

A pin is resolved in this order:

1. the architecture decides whether it exists and what it can do;
2. the board optionally limits which pins are exposed;
3. memory mode, console, vendor board, and fixed board wiring reserve pins; and
4. the pin is available to the application only when it exists, is exposed, and is not
   reserved.

A console reservation with `template_may_claim = true` leaves its pins available. The
configuration wins: when the active template assigns one of them, or a runtime owner such as
the selected energy driver or Berry `gpio` claims one, the console stops before the pin is driven and stays
off until restart. Beken uses this for UART1 on GPIO10/11, which plugs often wire to LEDs or
BL0937 SEL. The bootloader still prints on UART1 during reset. ESP-IDF chips with a USB console
use it for the UART0 pads, which are only the boot fallback there (for example GPIO43/44 on the
ESP32-S3): a USB console keeps running, a UART0 fallback stops. Environments whose console is
UART0 keep those pins reserved. The ROM and bootloader still print on UART0 during reset.

Strapping and other sensitive pins remain available with a caution flag, so valid
Tasmota templates keep working. A board without `gpio.exposed` exposes every pin of the
architecture.

## Validation and generated files

`make validate-catalog` checks the ID list, every topology, every module slot, and every
board and environment resolution. A build writes the firmware tables and a readable
`catalog.json` to `build/work/<environment>/generated/hardware/`.
