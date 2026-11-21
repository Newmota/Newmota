# Board build recipes

A board file describes one native build recipe. It is independent of the environment and
test matrices: adding a board does not make `make test` or `make all` longer.

ESP-IDF recipes state the external-memory and console choices that change SDK
configuration:

```json
{
  "chip": "esp32s3",
  "flash": {"size_kb": 4096, "mode": "qio", "frequency_mhz": 80},
  "psram": {"mode": "octal", "frequency_mhz": 80},
  "console": "usb-cdc",
  "profile_default": "Standard"
}
```

`psram.size_kb` may be omitted for a generic recipe whose capacity is detected by the
SDK. `cpu_cores` is used only for the ESP32 single-core recipe. `revision` selects a
native ESP32-P4 minimum revision. For ESP, `console` describes the preferred application
console rather than every physical interface on the chip. `usb-cdc` and
`usb-serial-jtag` each perform a short boot-time SOF probe and fall back to UART0 when no
host is present; `uart0` deliberately disables that probe. The ESP adapter verifies modes,
frequencies, console, and revision settings against the pinned IDF target.

SDKs that select a vendor board or project use `sdk_board`:

```json
{
  "chip": "bl616",
  "flash": {"size_kb": 4096},
  "sdk_board": "bl616dk",
  "profile_default": "Standard"
}
```

Every device must have at least 2 MiB flash. Partition allocation is not board data;
the build selects a target-specific
`config/partitions/<platform>/<size>-<sdk_target>.<native-format>` before the generic
`<size>.<native-format>` map for that flash capacity. On-chip capabilities belong to the
SDK/chip layer. A board may override a capability only for verified board-level companion
hardware; Pico W and Pico 2 W record their CYW43439 Wi-Fi and Bluetooth radio at board level.
Product GPIO templates remain application data rather than board build recipes.

A physical module or development board may add verified package exposure and fixed owners:

```json
{
  "gpio": {
    "source": "https://vendor.example/module-datasheet",
    "exposed": [0, 1, 4, 5],
    "reserved": {"radio": [23, 24, 25, 29]}
  }
}
```

These are physical SDK pin numbers, not Tasmota Template-array positions. `source` is an
HTTPS vendor pinout or datasheet used to review the facts. `exposed` limits application GPIO
to pins brought out by that exact module; omission deliberately keeps the architecture view
for generic recipes whose package is not fixed. `reserved` records permanent board wiring,
not feature-selected buses. Flash/PSRAM mode and console reservations belong to
`config/hardware/topologies/` because they are resolved from existing board fields.

All IDs and filenames use lowercase kebab-case. Unknown fields are errors; old notes,
duplicated board names, guessed capabilities, and ignored metadata are deliberately not
part of the schema.
