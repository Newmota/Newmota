# GPIO expanders

`USE_MCP23008` selects a portable cooperative register owner for MCP23008 addresses
0x20–0x27. It claims the I2C slave through the shared address-lease contract;
MCP23008, ADE7880 and ADE7953 cannot claim the same physical bus/address twice.
Opening probes the chip but never changes direction, polarity, pull-ups or output
latches. Reads/writes use the existing bounded I2C transaction API on all five
platform families. Interrupt flag/capture registers are read-only in this API.

BIOPDU is currently deferred and has no selectable runtime feature. Its retained
source uses a fresh GPIO register
snapshot and fails closed on read errors. Its explicit volatile register commands
use this same owner. A persistent relay/expander configuration service, generic
GPIO/relay mapping and the Tasmota Sensor29 surface remain unimplemented; selecting
this module alone does not configure or operate relays. Native/hardware validation
is handled separately from this source checkpoint.
