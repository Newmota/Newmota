# Captured GPIO and relay timing

`USE_EDGE_CAPTURE` provides exclusive timestamped input capture through native
ESP-IDF, Beken, Bouffalo, AmebaD and Pico adapters. Input slots and callbacks belong
to that owner. Atomic snapshots expose cumulative counts and microsecond periods;
uncertain cleanup retains the pin reservation. Counts include every accepted edge;
`period_count` excludes the first edge and intervals exceeding an optional idle-gap
limit or 32 bits of microseconds. A long gap establishes the next baseline instead of
creating a spurious measured period. Counts and the cumulative period sum are 32-bit and
wrap; consumers take unsigned differences between successive snapshots. Edge timestamps
and window sums stay 64-bit, so an idle input never aliases to a recent edge. An atomic
bounded window retains only the first requested number
of periods within an absolute microsecond interval; cumulative counters continue
independently. All five native adapters implement these rules under their existing
interrupt guards, including the ESP-IDF IRAM callback. See the energy driver documentation
for pulse-meter and interrupt consumers.

`USE_ZERO_CROSS` optionally maps up to eight capture sources to at most two existing
core relay pads each. It never takes ownership of the relay outputs. Captures use
CHANGE edges and pull-up; mappings must refer to configured relay pads and cannot
overlap another source. BasicIo relay writes consult the timing owner. Plausible
half-cycles are 4–20 ms; capture older than two half-cycles is stale. Each timed write
waits at most one half-cycle using native microsecond clocks/delay. Stale capture
falls back to an ordinary write and increments its diagnostic count.

PowerService groups single actions, set-all, timer/blink updates and interlock
operations with nested WriteGroup
scopes. The first write for each source opens a timing window; later writes sharing
that source do not wait for another half-cycle. The outer scope records the group's
execution duration once, including early-error paths. Explicit interlock delays call
next_window(), ending the prior window before the safety wait. Independent sources
retain separate timing, rather than applying one global meter to unrelated relays.

CSE7761 Dual R3 uses 6150 microseconds of chip/relay compensation. Actual edge noise,
coil/contact delay, scheduling jitter and native timing still require board-specific
hardware qualification. The generic owner needs no additional platform timer API;
it uses the already implemented native capture clock and GPIO delay adapters.

BL602/BL702 support rising or falling hardware capture, but not native CHANGE/both
edges. Both HLW CF and CF1 use FALLING in the pinned source and work through that
route. CHANGE-based zero-cross mappings still reject those targets; software edge
polarity switching is not advertised as lossless hardware both-edge capture.
