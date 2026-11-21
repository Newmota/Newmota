# Platform adapters

Each directory here connects the portable application to one native SDK. The adapter owns
SDK initialization, native entry points, task/callback handoff, storage geometry, network
and GPIO calls, console details, boot/update operations, and the SDK-specific web/MQTT
transport. The portable application sees only the interfaces under `include/newmota/platform/`.

## Current families

| Directory | Native SDK and current target | Current evidence |
| --- | --- | --- |
| `esp-idf/` | Espressif ESP-IDF; ESP32 reference environments | ESP32-S3 web and console smoke testing; optional descriptor-driven SPI, I2C, EPD, I80, RGB and DSI display transports compile on supported chips; display hardware qualification remains open |
| `beken/` | Pinned Beken BDK profiles; BK7231N, BK7231T, BK7238, BK7252 and BK7252N | XH-CB3S/BK7238 hardware check now passes: console `WiFiScan` and `WifiTest` work, and the setup SoftAP starts and is operational. Shared scan/AP recovery changes pass the native test matrix but need device qualification; browser onboarding submission and OTA qualification remain open. BK7231T (WB2S), BK7252 and BK7252N have native builds only; bk7231tools 2.1.2 cannot identify BK7252N, so `newmota flash` refuses it |
| `bouffalo/` | BouffaloSDK; BL602, BL616, BL618DG | Native build/package and initial hardware smoke: factory write/read-back and boot to setup AP/HTTP listener; LittleFS/settings were unavailable in this trial, and browser UI, recovery, and application-update qualification remain open; BL616/BL618DG application/Safeboot pairs are compile-tested |
| `amebad/` | Realtek AmebaD; RTL8720DN/BW16 | Latest BW16 log confirms STA/AP, `192.168.4.1`, DHCP, and scan results; onboarding routes and AP restart mode need hardware retest, and MCC scan warnings/client stability remain open |
| `pico-sdk/` | Raspberry Pi Pico SDK; RP2040/Pico W and RP2350/Pico 2 W | Both targets have positive hardware smoke tests; update and recovery qualification remain open |
| `lwip/` | Shared raw-lwIP transport helpers | Used by the current network-capable adapters where the SDK exposes lwIP |

Pico W/RP2040 uses cached LittleFS reads with normal filesystem/settings writes
enabled. Routine boot and stack diagnostics are disabled.

On AmebaD and RP2040, where LittleFS is the last region of the catalog layout, the filesystem
extends to the end of a larger physical flash chip. The JEDEC capacity only nominates a size;
it is accepted when no smaller power of two reads back as a mirror of offset 0, up to the 16 MiB
that 3-byte flash commands reach. The next mount grows the volume and keeps its files.
Application and Safeboot images resolve the same size. The reported device flash size stays the
catalog size. On ESP-IDF and Bouffalo, the Newmota factory flasher instead writes partition
tables whose final filesystem entry reaches the probed flash end. Bouffalo firmware accepts a
larger `lfs` entry only at its build offset, ending at a power-of-two flash end that the same
mirror probe proves. RP2350 keeps its partition-table size. Beken keeps LittleFS in its own region outside the update
area, placed per chip and bootloader map (see [partition layouts](../../config/README.md#partition-layouts)),
so updates keep files and settings.

Every platform now reserves at least 8 KiB for the main Newmota runtime task or thread.
Worker tasks retain separate platform-specific stack sizes.

AmebaD counts quick application restarts in SDK user backup register 3, with a magic
and inverse count in the same word. Software and watchdog resets continue a valid count;
power-on, brownout, deep-sleep wakes, unknown reset causes and invalid records start at 1.
The count saturates at 255 and clears after ten seconds of runtime or before the automatic
Safeboot handoff. Safeboot does not count boots. This enables protected Berry `autoexec.be`
and the shared boot-loop fallback without flash writes. Backup registers 1, 4 and 5 remain
owned by deep sleep; register 2 remains available to the SDK's factory tests. Retention and
boot-loop recovery still need hardware qualification.

Beken maintains a diagnostic quick-restart count in a no-init SRAM word containing
magic, inverse count and count. The generated linker script keeps it outside BSS
initialization and before the SDK heap. Software, watchdog and panic resets continue a
valid count; other reset causes and invalid records start at 1. It saturates at 255 and
clears after ten seconds of runtime. Counts above 1 appear in the existing startup log.
This is a retention candidate for BK7231N and BK7238: bootloader SRAM preservation,
warm-reset paths, cold starts and stable-window clearing still need hardware checks.
The boot guard continues to report retention unavailable, so Beken skips `autoexec.be`
and does not reduce a boot. The diagnostic count makes no flash writes.

Bouffalo maintains the same diagnostic record in the SDK's no-init SRAM region on
BL602, BL616 and BL618DG. Unknown resets continue a valid count; deep-sleep wakes
and invalid records start at 1. The count saturates at 255 and clears after ten
seconds, before the existing `FW` boot confirmation. No-init placement excludes
BSS initialization and heap use; it does not establish reset retention or a fresh
count after power cycling. Those checks remain open on every target. The boot
guard reports retention unavailable, so `autoexec.be` stays skipped and the count
does not reduce a boot. The diagnostic record adds no flash writes.

The common platform contracts cover clocks and sleep, GPIO, filesystem block access,
networking, web request handoff, MQTT streams, critical settings, and platform identity.
Native types stop at the adapter boundary. A platform reports an explicit unsupported result
when a physical capability is absent; it does not substitute a host implementation.

Wi-Fi application Ping uses one fixed raw-lwIP ICMP slot shared across the five adapters.
DNS and socket operations run on the lwIP core (under the CYW43 lock on Pico), with no
additional worker task. DNS cancellation keeps the slot reserved until its callback drains.
The adapter checks the selected source, IPv4 header length, complete ICMP length, checksum,
type/code, identifier, sequence, and payload token before consuming a reply pbuf. ICMP owns
its socket independently of the NTP UDP exchange and outbound TCP stream.

Optional custom-variant `USE_NTP_SERVER` owns a separate raw-lwIP IPv4 UDP123
listener across all five families. Copied clock publications cross to the lwIP core
(Pico CYW43 lock); arrival/departure timestamps and replies stay in that context.
Replies preserve the receiving interface and local address. See
[source quality and traffic limits](../app/services/time/README.md).

Optional `USE_WS2812` selects native RMT/SPI, PIO/DMA, BL602 continuous SPI/DMA,
or AmebaD SSI/GDMA. Beken rejects the feature pending a verified continuous
waveform: its pinned BDK DMA drain calculation includes an inter-word interval
that could exceed the selected WS2812B-V6 LOW limit. See the
[strip contract and routes](../app/drivers/led_strip/README.md).

Optional `USE_DEEPSLEEP` gives each family a timed deep sleep that ends in a fresh boot
reported as `Deep Sleep`:

| Family | Entry and wake | Kept across the sleep | Longest single sleep |
| --- | --- | --- | --- |
| ESP-IDF | `esp_deep_sleep_start` with the RTC timer; `USE_BERRY_ULP` adds the ULP wake | No-init RTC memory; nothing on the C2, which has none | Unlimited |
| Beken | The BDK's deep sleep with its RTC wake from the internal 32 kHz oscillator | Nothing: the only retention register holds the SDK's reset source | 0x1ffff s, about 36 hours |
| Bouffalo | HBN level 0 with the RC 32 kHz RTC; the HBN entry flag marks the wake | Nothing: the HBN registers belong to the SDK and boot ROM | 32-bit ticks, about 36 hours, on BL602/BL702; BL616/BL618DG much longer |
| AmebaD | KM4 asks the KM0 firmware for deep sleep on the AON timer over IPC | AON backup registers 1, 4 and 5, which the SDK leaves to users | 32-bit milliseconds, about 49 days |
| Pico | RP2350: POWMAN powers every domain down until its LPOSC alarm. RP2040: all clocks but the RTC's stop until the RTC alarm, then a watchdog reboot | RP2350: POWMAN scratch 0–4. RP2040: uninitialized RAM | Unlimited |

Pico switches the CYW43 radio off first. None of this has been measured on hardware yet.

When `USE_I2C` is selected, two shared software I2C buses use each native GPIO adapter
and its microsecond delay. Assign SCL/SDA with GPIO/Template and provide external pull-ups.
Berry and selected sensor drivers use the same bus owner. Hardware timing qualification
remains open.

`USE_DEVICE_SPI` uses each family's GPIO and microsecond-delay API for general
bounded SPI reads, writes and duplex transactions. This is a shared GPIO transport,
not a general native SPI-controller adapter. Existing display transports remain
separate, and their reserved pads cannot be reused by the device owner. See the
[device SPI contract](../app/drivers/spi/README.md) for timing and cleanup limits.

Optional `USE_DISPLAY` loads `/display.ini` across all five families. Shared GPIO
SPI, I2C and EPD work on each family; non-ESP I80 uses GPIO writes where the board
exposes enough pads. Bouffalo (BL602, BL616, BL618DG, BL702), AmebaD and Pico also
provide native SPI through the shared SPI controller owner.
ESP-IDF retains native I80, RGB and DSI where the chip has those controllers.
Explicit `USE_DISPLAY_QSPI` adds four-line native QSPI on ESP-IDF, BL616/BL618DG
DBI and RP2040/RP2350 PIO/DMA; ordinary display/LVGL selections omit it.
BL618DG uses a provisional inferred signal order requiring confirmation.
Unsupported native routes and controllers are rejected before configuration.
`USE_LVGL` selects this feature automatically on every platform. See the
[transport table and pin routes](../app/drivers/display/README.md#transport-support).
Optional `USE_UNIVERSAL_TOUCH` adds a bounded descriptor compiler and I2C executor
using the shared bus, with cooperative samples for Berry and LVGL pointer input.

### ESP-IDF SPI hosts

`esp-idf/spi_host.cpp` alone initializes and frees the GPSPI controllers. Display SPI and
SPI Ethernet controllers take shared leases: with identical clock, MOSI and MISO pins they
attach their own devices and CS lines to one bus, initialized for 4096-byte transfers
whichever user starts first. Different pins on a leased host are rejected. The display
reserves the bus while its GPIO CS is low, so Ethernet transactions from the driver task
cannot interleave with a display command. QSPI panels and the SPI-encoded LED strip
(chips without RMT) lease their host exclusively. Bus sharing has no hardware
qualification yet.

### Native SPI controllers

`newmota/platform/spi_controller.h` owns the hardware SPI controllers on Bouffalo (SPI0),
AmebaD (SSI0/SSI1) and Pico (SPI0/SPI1); ESP-IDF uses its own SPI host owner. Devices with
identical clock, MOSI and MISO pins share a controller and drive their own GPIO CS. Each
transfer names its clock, mode and frame size; the owner reprograms the controller only
when that format changes, and returns after the wire has drained. Display SPI and SD cards
use it through `display/native_spi.cpp` and `sd_card_spi.cpp`. 9-bit frames exist on
AmebaD and Pico only. None of this is hardware-qualified yet.

### SD cards in SPI mode

`sd_card_spi.cpp` speaks the SD SPI-mode protocol for `USE_SDCARD` on Bouffalo, AmebaD and
Pico: identification at 400 kHz in mode 0, then data at up to 20 MHz, single-block reads and
writes, CRC off, SDSC byte addressing and SDHC/SDXC block addressing. MMC cards are not
supported. On Beken the card reports unsupported.

SDIO cards use `bouffalo/sd_card_sdh.cpp` on BL616 and BL618DG, the only targets with an SD
host. It runs BouffaloSDK's `sdh_sd` driver in polling mode (`CONFIG_BSP_SDH_SD`,
`CONFIG_BSP_SDH_OSAL_POLLING_MODE`) and caps the clock at 20 MHz after the driver's
initialization. SDH owns six fixed pads, GPIO10–15 on BL616 and GPIO43–48 on BL618DG, which
carry D2, D3, CMD, CLK, D0 and D1 in that order; other SDIO pins are rejected. Only the clock
position comes from the SDK; the rest follows the BL616 pinout and, like the whole BL618DG
group, needs hardware confirmation. The driver switches to four data lines whenever the card
supports them, so one-bit wiring is rejected. On BL616 the same pads also serve the DBI QSPI
display. Other targets report SDIO cards unsupported.

### ESP-IDF SD cards

`esp-idf/sd_card.cpp` opens the card for `USE_SDCARD` and hands FatFs whole sectors through
`sdmmc_read_sectors`/`sdmmc_write_sectors`; ESP-IDF's VFS and FatFs are not used. SPI mode
takes a shared lease on SPI2 (bus 1) or SPI3 (bus 2) and attaches `esp_driver_sdspi` with its
own CS, so a display or Ethernet controller can share the wires. SDIO mode uses the SDMMC host
(ESP32, ESP32-S3, ESP32-P4/P4X, ESP32-S31): slot 0 or 1 when the pins equal that slot's
dedicated IOMUX pins, otherwise a GPIO-matrix slot (slot 1 first; both on the S3, slot 1 on the
P4). Pins no usable slot reaches are rejected before the host is touched. The slot ESP-Hosted is
built for (`CONFIG_ESP_HOSTED_SDIO_SLOT`) is never used: `sdmmc_host_init_slot()` would
re-initialize it under the Wi-Fi companion, so pins that only that slot routes are rejected. The classic ESP32 has dedicated
pins only (CLK 14, CMD 15, D0 2, D1 4, D2 12, D3 13); four-bit mode there logs that D2 is the
GPIO12 strapping pin. On the P4 slot 0 takes its IO power from on-chip LDO channel 4; with
ESP-Hosted on slot 1 the card must use slot 0's dedicated pins (39–44). Both modes clock at most 20 MHz.
None of this is hardware-qualified yet.
Hardware behavior and panel-specific timing remain unqualified.

When `USE_SERIAL` is selected, native UART capture continues while application/Berry
code runs. Console ports are excluded: runtime UART0/USB fallback on ESP-IDF, Bouffalo UART0,
and AmebaD LOGUART. Pico uses USB for the console. Beken opens one port: UART2 on GPIO0
TX/GPIO1 RX, or UART1 on GPIO11 TX/GPIO10 RX. The catalog reserves the SDK console pads
GPIO10/11 until a template uses them, which stops the console; SDK log output then moves to
the other UART's transmitter without pads, so it never reaches the application's wire.
BL602 UART1 rejects signal slots that alias the console (GPIO numbers modulo 8 equal 0
or 7), and RX/TX cannot share a signal slot. AmebaD uses the pinned SDK UART0/UART3 pin
maps and supports 7/8 data bits; LOGUART pads are excluded. Pico uses the native UART
route table, including RP2350 auxiliary mux functions. ESP-IDF supports native inversion;
other current adapters reject it. Application handles carry generations so closed handles
cannot access a reopened port. The serial contract is cooperative; IRQ handlers only
capture bytes and status into bounded rings.
Native serial consumers open unassigned physical pads through the same common owner as
Berry. The optional [UBX GPS owner](../app/drivers/gps/README.md) uses a separate
binding-authorized path for exactly one configured GPS RX/TX pair, checking its declared
baud and retaining ordinary UART/console ownership rules. Other configured bindings
remain reserved from serial clients.
The serial baud input bound is 1–4,000,000, with narrower native limits where enforced.
SDK clock/divider selection can quantize requested rates; native compilation does not
qualify the resulting baud or signal timing on a board.

## Optional I2S audio

`USE_I2S` requires at least 4 MiB flash and is not available on Beken. Each backend reports
its compiled SoC capabilities and rejects unsupported modes, directions, formats, and routes
before acquiring pins. A shared owner claims the complete pin set and native controller for
the life of an audio session.

| SoC family and current target | I2S implementation | Limits and resource ownership |
| --- | --- | --- |
| ESP32 family through ESP-IDF | ESP-IDF channel driver (`i2s_new_channel`), with SoC-capability checks | Standard and TDM are available where the SoC/IDF exposes them; PDM direction/conversion support is chip-specific. A paired standard/TDM lease owns TX and RX together. PDM PCM conversion uses controller 0. Controllers sharing an external MCLK or PDM clock configuration cannot be independently rebound. ESP32 variants without an I2S peripheral report unsupported. |
| Raspberry Pi RP2040/RP2350 | PIO state machine and chained DMA; no dedicated I2S peripheral | Philips standard mode, controller role, 16/32-bit slots, stereo wire framing with mono slot selection. Consumes one PIO state machine and DMA channels per direction. BCLK and WS must use adjacent GPIOs, WS = BCLK + 1; MCLK, PDM, TDM, and target role are unsupported. |
| Realtek AmebaD RTL8720D | One integrated I2S controller with page DMA | Standard mode only, fixed SDK pin routes, discrete native sample-rate table, and 16/24/32-bit samples. One session owns the bidirectional controller; mono is selected from its stereo wire slots. PDM and TDM are unsupported. |
| Bouffalo BL602 | No native I2S block exposed by the current target | The pinned BL602 SDK build excludes `bflb_i2s.c` and its device table has no I2S instance. The backend reports zero capabilities and feature resolution rejects `USE_I2S`; it does not emulate I2S through GPIO. The generic SDK matrix marks BL602 untested; BL616/BL618DG have native SDK drivers but their Newmota I2S adapters remain unavailable. |
| Beken | Deferred | No I2S adapter or audio feature is selected. |

See the [audio session, codec, and command contract](../app/drivers/audio/README.md) for
compiled codecs, bounded session ownership, and available network routes. Native builds
check integration only; clock accuracy, DMA behavior, audio quality, and full-duplex operation
still require board-level qualification.


`USE_ADC` provides configured single-ended ADC inputs through a fair shared owner
on all five families. Native GPIO routes, reference/calibration provenance, bounded
sampling and cleanup rules are documented in the [ADC contract](../app/drivers/adc/README.md).
`USE_ENERGY_ANALOG` uses that owner for per-pin calibrated voltage/current metrology;
board ADC/reference accuracy remains subject to hardware qualification.

### BL602 C++ headers and native scheduling

The BL602 build uses native FreeRTOS and lwIP APIs. Its optional SDK POSIX shim is
disabled: that partial pthread/sys-types layer lacks the once/key declarations
required by the pinned libstdc++ headers and was not used by Newmota. Standard
C++ headers therefore use the toolchain's own declarations without rewriting
portable application ownership APIs. This does not add a pthread or `std::thread`
runtime. Native SDK consumers must include FreeRTOS explicitly before task.h,
without relying on POSIX time headers for incidental inclusion.

### Bouffalo Wi-Fi6 targets

`bl616dk` and `bl618dgdk` use the SDK FHOST Wi-Fi6 manager, FreeRTOS and lwIP.
`bl618dgdk` names the BL618DG SDK target and B0 board defaults. Both environments
use Standard applications with HTTPS and exact paired Safeboot images.
BL616 PWM-v2 drives single-ended positive outputs: GPIO n uses channel n % 4, and all
four channels share one divider and period, so concurrent outputs need compatible carriers.
BL618DG PWM-v2 and both targets' ADC, auxiliary UART, and native display SPI routes are
rejected; general GPIO display transports remain available.
WS2812 and I2S feature selection is rejected until native transports are implemented.
Wi-Fi, storage, TLS, recovery, GPIO and peripheral behavior still need hardware trials.

The shared Bouffalo entry point invokes C++ constructors once after board and heap initialization.
BL602 uses a project linker fragment to retain constructor arrays; the Wi-Fi6 targets
retain their SDK linker templates. Image headers use each target's boot2 structure.
The application and Safeboot enable native image CRC/SHA-256 verification.

### Optional Bouffalo BLE

Custom BL602 and BL616 `USE_BLE` builds use the SDK blestack with one total link
in either role. BL602 selects `m1s1`; BL616 selects `ble1m10s1bredr0`, retaining
the archive's ten-link native pools while admitting only one adapter connection.
BL618 modules use the BL616 target; BL618DG BLE is deferred and rejected.
RF initialization is shared with Wi-Fi and independent of its startup.
The BL602 BLE generated linker adaptation bounds XIP by the selected application slot,
including its native header, and moves lwIP protocol pools to ordinary zeroed RAM.
The SDK's Wi-Fi descriptors, packet heap and BLE exchange-memory reservation retain
their native placement. BL616 uses its SDK linker unchanged, reserving 32 KiB of
exchange RAM from WRAM for the controller, and selects the SDK's 32 KiB lwIP
packet heap to retain room for Wi-Fi. Runtime packet pressure remains unqualified.
Standard builds keep the SDK linker regions and packet-heap size. See the
[BLE contract and limitations](../app/services/berry/modules/ble/README.md).

### Optional AmebaD BLE

Custom AmebaD `USE_BLE` applications use the pinned Realtek GAP/GATT library with one
total link, either accepted or outbound, and legacy scanning/advertising. All native BLE
calls run on the GAP message task; a separate worker sequences client jobs. Berry callbacks
remain cooperative. The controller uses internal UART1; native binary tracing is discarded
without acquiring UART3 or its pads. The generated project excludes the vendor flash FTL
and its startup call; bounded RAM hooks satisfy the native storage ABI instead. The
controller uses its pinned compiled firmware patch, without reading vendor flash-patch slots.

The native controller requires the Wi-Fi driver/radio to be powered. The SDK boot task
initializes that driver independently of station association, and Newmota then selects
its concurrent mode. The application starts BLE after that. `ble_start()` waits up to
15 seconds for native driver readiness before opening the native stack, whose HCI
transport refuses to start without it, and fails if the driver is unavailable; it does
not wait for an IP address, DNS, MQTT or NTP.
Pairing, secured attributes and Secure Connections use the native stack. Bonds live in
bounded RAM and can be exported/restored through validated records tied to this SDK and
the local identity; import and pairing never evict a different peer.

The service database remains registered until reboot because this SDK exposes no removal
API. VM teardown revokes delivery, drains the link and clears prepared values; a new VM can
reuse the same UUIDs and permissions, while a changed schema requires reboot. Select BLE
through a personal variant inheriting `bw16-4m`; standard applications and Safeboot stay
BLE-free. See [BLE limits](../app/services/berry/modules/ble/README.md).


### Optional Pico BLE

Custom Pico W and Pico 2 W `USE_BLE` applications use the pinned SDK BTstack/CYW43
host and transport. The SDK async run loop shares the existing CYW43 context, including
HTTP background and HTTPS polling configurations. All native calls and copied snapshots
use its lock; callbacks never run Berry. There is one total link, either role, legacy
scan/advertising, a dynamic prepared-value GATT server and a native GATT client.

The adapter supplies the SDK architecture's BTstack lifecycle hooks over its native HCI
transport, selecting the SDK RAM device database instead of its default flash TLV banks.
Four bonds have validated same-SDK export/restore records; scripts own persistence.
Standard applications and Safeboot remain BLE-free. See the
[BLE limits and personal build example](../app/services/berry/modules/ble/README.md#pico-w-and-pico-2-w-backend).
