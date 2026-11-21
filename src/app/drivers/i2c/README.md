# I2C devices

`USE_I2C` provides two configured software I2C buses through each platform's GPIO
and microsecond timing adapter. Configure SCL/SDA with the existing hardware
functions. External pull-ups are required. Bus operations are synchronous,
cooperatively serialized and bounded by the platform I2C transfer contract.

With `USE_WEBCAM`, the Template's `WEBCAM_SIOC`/`WEBCAM_SIOD` pins become the first bus that
has no `I2C SCL`/`I2C SDA` pins. That bus enables the weak internal pull-ups, because camera
modules commonly rely on them for SCCB; see the [camera guide](../camera/README.md).

## Slave-address leases

ADE7953 I2C, ADE7880, MCP23008, universal display touch, and the optional external RTC
owner select `i2c_device_claims.cpp` as a shared dependency. Its
application-task-only owner provides at most 16 exclusive bus/address leases.
Generation IDs are never reused; a stale release cannot free a newer lease.
The implementation is included once when several participating drivers are selected.
SPI-only ADE7953 selections do not include or call it.

An address lease reserves a logical slave, independently of the hardware owner's
SCL/SDA pad claims. Device teardown releases the lease after its protocol work stops.
Two participating drivers cannot open address 0x38 on the same bus, while different
buses and different slave addresses can coexist. Lease ownership does not filter
raw I2C transactions or reserve addresses for unrelated drivers which do not use
this API. Avoid raw Berry/I2C accesses to a slave while its driver is active.

Optional RTC chips use the [generic RTC owner](../rtc/README.md) at their fixed addresses.
It reserves candidate addresses before probing and releases ownership before hardware
initialization in the application lifecycle. Any future bus reinitialization path must
first release its device owners. Selecting the RTC feature asserts the chip type;
an ACK is not unique model identification. Same-address model selections are mutually exclusive.
