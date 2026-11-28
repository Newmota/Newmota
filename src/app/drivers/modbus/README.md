# Shared Modbus RTU buses

`USE_MODBUS_RTU` selects a portable, poll-driven RTU owner and `USE_SERIAL`.
It defaults off. The owner uses the existing native UART adapters on ESP-IDF,
Beken, Bouffalo, AmebaD and Pico. Native framing, route, console and pad restrictions
still apply. Safeboot excludes this application feature.

One configured physical bus owns one UART, an optional active-HIGH RS-485 DE
pad and an optional separate RE pad with a configured active level. Devices attach separate address clients to that bus; they do not open UARTs.
The bus serializes requests with one in flight and round-robin scheduling across
waiting clients. A failed attempt rejoins the queue, allowing other clients to run
before its retry. Completed results remain in the submitting client's queue slot
until consumed.

## Persistent configuration

```text
ModbusBus1 16,17,18,9600,0,1,19,0
ModbusBus1
ModbusBus1 0
```

`ModbusBusN rx,tx,de,baud,parity,stopBits[,re,rxActive]` configures a one-based bus slot.
`de=-1` omits direction control; `re=-1` omits separate receiver control.
The optional pair defaults to `-1,0`; `rxActive` is 0 for active-LOW or 1 for active-HIGH.
RE is disabled before transmission and enabled after the final stop bit, suppressing
local echo when the transceiver has a separate receiver-enable input. Parity is 0
for none, 1 for even or 2 for odd;
RTU uses eight data bits and one or two stop bits. Pads must be unassigned.
The query reports configuration, availability, client count and the silence guard.
Payload `0` closes an idle bus after all clients detach. Reapplying identical
configuration succeeds; replacing an existing configuration requires explicit close.
Overlapping-pin reconfiguration with rollback remains unfinished.

Successful configuration and removal commands persist to `/.newmota/modbus.a`/`.b`.
Boot restores buses and their exclusive pin claims after hardware initialization,
before persisted energy instances attach clients. Native restore failure retains the
configuration, logs the bus/result and reports `Available:false`; reapplying its
identical configuration retries opening it. Faulted native leases require explicit
close and cleanup first. Without a valid slot, an invalid or unreadable slot blocks
writes, reported by `StorageAvailable:false`. Safeboot and builds without this feature do
not modify the sidecar.

The fixed, little-endian version-1 sidecar contains `NMRB`, version, four-slot count,
a 32-bit slot generation, four 16-byte records and a Modbus CRC16. Each record stores RX/TX/DE/RE as 16-bit pin
IDs, 32-bit baud, parity, stop bits, RE active level and enabled flag as bytes.
Eight data bits, non-inverted UART and the native RX capacity are enforced. A save
writes the whole record in one atomic LittleFS write into the slot not holding the loaded
record; startup takes the newest valid slot, so an unusable newer slot falls back to the
older one. Failed saves report storage error, retain the previous intent and roll back
native opening/closing where possible; failed native cleanup retains its reservation.
This transport feature alone selects no meter; `USE_PZEM_AC` and `USE_PZEM_DC`
select their independent energy devices.
The full energy-driver inventory remains committed; TuyaMCU follows its shared owner.

## Native caller contract

Use `configure_bus()` once for the physical connection and `open_client()` for each
slave address. API bus slots are zero-based. Standard addresses are 1–247; address
zero permits no-response broadcasts. Devices using a reserved vendor address must
explicitly set `vendor_address=true` when attaching. A vendor programming client
can additionally request `exclusive_programmer=true`: admission requires at most
one existing address client, and further client attachments are blocked until its
asynchronous detach completes. Physical single-device wiring is still required.
A distinct `exclusive_bus=true` attachment requires zero existing clients and
blocks every other attachment through asynchronous detach. Multiplexed-board
owners use it while holding selector pins fixed through each complete transaction.
`retarget()` may change only an idle exclusive client's address; pending and
unconsumed-completion states reject retargeting. This keeps vendor programming on
the same physically exclusive route without admitting a second address owner.

`submit()` copies one function/data PDU and appends its address and CRC. Its
`Transaction` declares the exact expected data size, optional byte-count or request
prefix echo validation, response timeout and retry limit. Exception responses are
reported separately with their exception code. `take_response()` consumes one
completion. `cancel()` and `close_client()` finish or cancel active TX without
interrupting its final stop bit. There are no driver callbacks from the UART owner.
Call these APIs on the cooperative application task.

| Resource or operation | Bound |
| --- | --- |
| Configured physical buses | 4, further limited by native UART availability |
| Address clients across all buses | 16 |
| Pending requests per client | 1, including an unconsumed completion |
| Complete RTU ADU | 256 bytes |
| Function data, excluding address/function/CRC | 252 bytes |
| Response timeout | 10–5000 ms |
| Retries after the initial attempt | 0–3 |
| Configured baud in this implementation | 300–115200 |

Bus/client state allocates only for configured buses and attached clients. Responses
validate address, function, declared size, requested count/echo when selected, and
Modbus CRC-16. CRC, protocol and response-timeout failures can retry. An uncertain
UART or TX-drain failure faults the bus and completes queued clients with errors;
pads remain under the serial owner's cleanup policy. Successful native cleanup can
release them. Explicit bus close is needed before reopening a faulted configuration.

## Timing and qualification limits

The application polls TX FIFO acceptance, final stop-bit drain and RX availability;
it does not wait for response bytes. Explicit serial `begin_tx()`/`end_tx()` frame
hooks hold DE across partial writes and lower it only after the final accepted byte
drains. Ordinary serial writes retain their existing behavior.

The silence guard is the upward-rounded 3.5-character interval calculated from the
configured baud, data bits, parity and stop bits, plus one millisecond tick. Above
19200 baud it is also at least the upward-rounded 1.75-ms fixed RTU interval.
Every response, timeout, cancellation and broadcast restarts this guard. Stale RX
bytes extend it; no request begins while captured input remains queued.

The UART API does not provide byte-arrival timestamps. Buffer-empty observations
and millisecond deadlines therefore do not prove exact wire-level silence or the
1.5-character inter-byte requirement. The guard is a conservative scheduling
approximation. If a partially queued ADU has already drained, the owner aborts it
and retries fairly instead of appending bytes after an unmeasured gap. Regular
application polling remains necessary, and long blocking commands may cause timing
failures. Local transmit echo is not parsed: configure separate RE when required, suppress
RX echo during DE in the wiring, or use a connection without local echo.

These limits are implementation and qualification boundaries, not proof that faster
hardware or stricter capture is impossible. Exact receive timing and higher-rate
atomic native TX remain committed transport work. Format and portable syntax checks
have passed; linked firmware, bus arbitration, DE turnaround, framing fidelity and
meter operation have not been hardware-qualified by this package.
