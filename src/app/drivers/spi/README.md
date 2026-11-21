# General device SPI owner

`USE_DEVICE_SPI` selects a bounded GPIO SPI transport for application drivers on
ESP-IDF, Beken, Bouffalo, AmebaD and Pico. It defaults off. This implementation
uses each family's native GPIO and microsecond-delay APIs; it does **not** use a
hardware SPI controller. No energy driver is selected by this feature alone.

## Ownership and calls

`device_spi::open()` claims clock, optional MOSI and optional MISO pins. At least
one data direction is required. Identical complete wire configurations share one
physical bus through distinct generation-checked leases. Different configurations
cannot overlap pins. Pins must be available, support their requested direction and
have no configured or runtime owner.

`attach()` claims one separate CS for a device. Each device supplies mode 0–3,
MSB/LSB bit order, an upper clock-rate limit, CS polarity, setup delay and hold
delay. `transfer()` supports writes, reads with `0xff` dummy bytes, simultaneous
full-duplex transfers and identical in-place TX/RX buffers. MOSI is required when
TX bytes are supplied; MISO is required when RX storage is supplied. Other buffer
overlap is not supported. CS remains active for the complete supplied byte span;
a command prefix and its read payload therefore belong in one duplex transaction.

Calls belong to one cooperative application task. A bus rejects reentrant transfers,
attachment and final cleanup while a transaction is active. Each synchronous
transaction completes before another device can use that bus. There is no queued
scheduler or ISR/core access. Independent devices can share the wire owner while
selecting different modes and rates per transaction.

Close devices before closing their lease. Closing one lease leaves the other
leases active; the final close releases bus pads only after confirmed input
configuration. Device close deasserts CS and confirms its input configuration before
releasing that pad. Failure retains the claim and permits a cleanup retry.

An initialization failure after pin acquisition returns a populated handle and
`failed`. Retain that handle and call the matching `close()` overload; discarding it
quarantines the slot and pads until restart. A GPIO failure during transfer faults
the bus/device and retains ownership. A timeout attempts clock-idle and CS release;
confirmed cleanup permits later transactions, but a partially transmitted device
command may already have changed the peripheral. The owner never retries writes.

## Bounds and timing

| Resource | Bound |
| --- | --- |
| Physical GPIO buses | 4 |
| Independent bus leases | 16 |
| Attached devices | 16 |
| Bytes in one CS transaction | 256 |
| Requested upper clock rate | 1–250 kHz |
| CS setup/hold delay | 0–100 microseconds each |
| Transfer deadline | 1–100 ms, default 25 ms |

The half-period is rounded upward to whole microseconds. GPIO overhead, SDK calls
and interrupts can stretch clock periods; requested frequency is an upper bound,
not a measured wire frequency. Deadlines are checked at bit boundaries with a
millisecond clock. A call can overrun by a bit interval, cleanup delays and SDK or
interrupt latency. Slow rates and long transfers can exceed the deadline and return
`timeout`. There is no DMA or uninterrupted waveform guarantee.

Configured display and LED-strip pads remain reserved through the common pin owner,
so device SPI cannot steal those pads. This GPIO owner is separate from the native
SPI controllers: on ESP-IDF those belong to the [SPI host owner](../../../platform/README.md#esp-idf-spi-hosts),
which the display, Ethernet and LED strip use. Hardware-controller adapters for
application drivers, native sharing on the other families and hardware qualification
remain unfinished implementation work. This GPIO transport neither
proves native-route support nor makes a missing native adapter an unsupported chip.

ADE7953 Pro variants use this owner at an upper rate of 250 kHz. Their pinned
Tasmota 1-MHz controller setting must not be inferred from this GPIO transport.
See the [energy driver behavior and gaps](../energy/README.md#ade7953-i2c-and-spi-variants).
