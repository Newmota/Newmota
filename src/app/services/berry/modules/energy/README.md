# Berry energy API

Select `USE_BERRY_ENERGY` for the virtual provider, or `USE_BERRY_ENERGY_API` for
checked access to the selected hardware energy driver. Both select Berry and the energy
service. Optional `USE_BERRY_ENERGY_ACCOUNTING_API` adds checked daily projection reads
and selects the shared live-daily/calendar owner; it selects no meter driver.

The Module/Template/GPIO configuration selects **one provider**. Source Option A9 selects
Berry energy. Scripts do not select hardware; apply hardware changes with explicit
`Restart 1`. See the [energy setup guide](../../../../drivers/energy/README.md).

```berry
import energy
print(energy.read())
print(energy.phase_count)
```

Phase views contain no native pointers and check the provider's generation on access. A
missing provider and out-of-range phases raise checked errors. Hardware measurement fields
are read-only. Only a selected Berry provider accepts script measurements.

```berry
# With Option A9 selected in the template and applied by Restart 1:
import energy
energy.voltage = 230.0
energy.current = 1.25
energy.active_power = 287.5
print(energy.read())
```

## Values and phase views

Scalars address channel 1. Numeric suffixes `_2` through the configured channel count
address further channels. `_phases` returns a bounded view with `size()`, indexed access,
negative indices and `tostring()`. `read()` / `tomap()` return copied values, phase count
and driver ID. The meter hardware determines its phases.

| Field | Units / meaning |
| --- | --- |
| `voltage`, `current`, `active_power` | V, A, W; negative power exports |
| `apparent_power`, `reactive_power` | VA, var |
| `frequency`, `power_factor` | Hz, signed ratio -1 through 1 |
| `temperature`, `battery_voltage`, `battery_current` | Optional °C, V, A |
| `import_active`, `export_counter` | Submitted absolute hardware counters, kWh |
| `export_active` | Enabled local export projection, otherwise native export counter, kWh |
| `total`, `total_sum` | Read-only signed compatibility Total / phase sum, kWh |
| `import_total`, `export_total` | Physical lifetime accounts, kWh; virtual provider seeds are writable |
| `import_total_sum`, `export_total_sum` | Read-only physical account phase sums |
| `start_energy` | Read-only imported-counter baseline, kWh |
| `valid` | Sample validity |
| `data_valid` | Source observation age where supported; virtual validity marker otherwise |
| `phase_count`, `driver_id` | Read-only selected configuration |
| `power_on` | Source Power/SetOption21 measurement gate |
| `use_overtemp` | Read-only driver's request for global-temperature protection |
| `energy_active_export` | Runtime local-export reporting control |
| `counter_export_routing` | Durable virtual imported-counter direction routing |
| `voltage_common`, `frequency_common` | Read-only hardware topology; virtual provider options |
| `type_dc` | Hardware sample classification; virtual provider option |
| `voltage_available`, `current_available` | Hardware field availability; virtual output options |

Measurements cross the shared integral milli-unit boundary. Out-of-range writes raise
`value_error`; calibration coefficients do not rescale Berry-submitted measurements.
Copied maps do not expose writable native storage.

Hardware `voltage_common` / `frequency_common` describe physical shared measurements,
not equality of sample values. They are meter-wide scalars, not phase arrays. A missing
runtime device returns unavailable topology rather than guessing it. Virtual stored flags
retain their own output semantics.

## Validity and accounting

Reading `data_valid` does not poll, age or refresh the source. Source clocks normally start
at zero, age on observed seconds and expire at five; renewal follows the actual driver's
accepted observation. `valid` remains a separate sample property. BL0939 preserves the
source's independently ageing second channel. No observation clock is fabricated for a
provider/input mode that lacks one.

`driver_enabled()` reports whether the selected provider is Berry. `update_total()` polls
the shared owner without adding a second integration. Optional accounting fields exist only when their owner is selected; unavailable fields
return the corresponding API's unavailable value or checked error.

The virtual provider supports elapsed-power and explicit absolute-counter accounting.
Use one input mode consistently; do not submit both a routed imported counter and the
same energy as a separate export counter. `energy_active_export` controls reporting,
while `counter_export_routing` controls submitted-counter direction. They are distinct.
Float kWh fields have Berry real precision limits; use the exact account API for full-range
values:

```berry
energy.mode('counter')
energy.counter(1, '18446744073709551600', 123456, '0', 0)
print(energy.totals(1))  # whole Wh strings plus integer micro-Wh remainders
energy.seed(1, '1500', 250000, '20', 0)
```

Account seeds are durable changes, not consumption. Coefficients, totals and exact
fractional carry survive ordinary restart through the energy sidecar. Invalid storage
blocks writes. Replacing the physical identity is not an implicit reset operation.

## Compatibility boundary

The API replaces raw ctypes pointer aliases with checked values and views. `_ptr`, ctypes
phase constructors and inherited byte-buffer operations are unavailable; `_deref()` is an
inert compatibility call. `init()` requires no manual invocation.

Tasmota writable internal calendar/history buffers, command internals and protection
counters have no general shared writable mapping. `phase_count` is read-only. This API
does not claim complete raw-memory compatibility with Tasmota's Berry energy structure.
