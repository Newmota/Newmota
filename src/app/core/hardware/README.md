# Runtime GPIO configuration

Normal application images provide Tasmota's `Module`, `Modules`, `GPIO`, `GPIO<n>`,
`GPIORead`, `GPIOs`, and `Template` commands. They work on the pins and functions defined in
the [hardware catalog](../../../../config/hardware/README.md). Safeboot images do not include
these commands.

`Template` accepts Tasmota's JSON fields `NAME`, `ARCH`, `GPIO`, `FLAG`, and `BASE`, including
partial updates. Functions Newmota implements are checked against the pin topology. Other
function IDs from Tasmota's list are kept as inactive assignments, so a template can be
moved to firmware that supports them. IDs outside that list are rejected. `CMND` is not
supported yet.

The web interface edits the same settings through these commands; see the
[web interface](../../services/web/README.md).

Every native platform provides digital input, output with a defined initial level, read, and
write. Normal applications also support `GPIO_PWM1` and `GPIO_PWM1_INV`. Interrupts and other
peripherals are provided by their drivers.

The selected module, user template, and pin overrides are stored in the settings slots
`/.newmota/settings.a` and `/.newmota/settings.b`. A change is validated before it is written; an invalid change
leaves the running and stored configuration unchanged. Stored assignments for another
architecture or an unknown function remain visible and are used again by firmware that
supports them.

As in Tasmota, an Option A value (`GPIO_OPTION_A`, wire value 6208 + n − 1 for Option An) is a
flag, not a pin. On a valid pin it sets its flag and leaves that pin unconfigured and free: it
does not appear in `GPIORead`, cannot stop the serial console, and Berry may use the pin.
Repeating a value sets the same flag. Option A is offered in builds with `USE_ENERGY`; A2
selects the dummy meter and A9 the Berry meter. Other values are stored but have no effect.

## Energy selection

Compiled energy GPIO functions participate in the same Module/Template resolver and web
picker as relay and button functions. After explicit `Restart 1`, the resolved meter
signature selects one energy provider; its hardware determines the measurement channels.
BL0942 RX variants encode baud/sign settings, and SEL inversion encodes pulse-meter
polarity. No separate physical `EnergyConfig` tuple is needed. Conflicting meter signatures
are diagnosed rather than selecting a driver by registration order. See the
[energy guide](../../drivers/energy/README.md).
