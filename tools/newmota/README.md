# Build and device commands

Every `make` target forwards to `PYTHONPATH=tools python -m newmota`; run that with `--help`
to see the full CLI. Platform ownership and extension rules are in the
[tooling guide](../README.md).

## Setup

```bash
make uv-setup                 # lockfile-pinned Python tools, including blflashcommand
make setup ENV=esp32c2-2m     # SDK and tools for one environment
make setup PLATFORM=esp       # one platform; PLATFORM=all for every platform
make sdk-check                # verify every installed SDK against its pin
make sdk-fetch                # download every pinned SDK archive, for offline builds
make sdk-checkout SDK=beken   # git checkout in .sdk/ for SDK patch work
```

Setup is optional: a build installs what it needs. SDK installers need a host Python 3.10 or
newer outside the project virtual environment. `PLATFORM=esp` and `PLATFORM=all` prepare both
pinned ESP-IDF tracks (5.5 and 6.1); `make sdk-check` without arguments requires
`make setup PLATFORM=all` or `make sdk-fetch`.

SDKs come as stripped, checksummed release archives of the pinned commits, built by
[Newmota/sdk-archives](https://github.com/Newmota/sdk-archives) and extracted read-only into
`~/.newmota/sdk/`. An SDK patch goes to its Newmota fork first: `make sdk-checkout SDK=<key>`,
commit and push in the fork, publish an archive with the sdk-archives workflow, then update
`revision`, `archive` and `archive_sha256` in `config/sdk/<key>.toml` in one Newmota commit.

## Catalog

```bash
make list                     # environments, including personal ones and their base
make config                   # configure and build a user configuration; ENV=<env> picks the start
make validate-catalog
```

Personal environments live in [`user_config/`](../../user_config/README.md) and work with every
build and device command.

## Build

```bash
make build ENV=esp32c2-2m
make build BOARD=esp32s3-qio-opi
make esp32c2-2m               # shorthand for make build ENV=esp32c2-2m
make test                     # catalog, host tests, and the test matrix
make clangd ENV=esp32c2-2m    # compilation database for editors
make format-cpp               # format first-party C/C++; format-check-cpp to check
```

| Variable | Effect |
| --- | --- |
| `JOBS=<n>` | Parallel jobs; default is every logical CPU |
| `VERBOSE=1` | Show native compiler commands and full SDK output (`NEWMOTA_VERBOSE=1` for the CLI) |
| `SDK_LOG_LEVEL=<level>` | Vendor SDK log level; see [build configuration](../../config/README.md#sdk-logging) |

`make test` builds the environments in `config/env/matrices/test.txt`. `make all` builds every
checked-in environment and takes a long time.

Native SDK work goes to `build/work/<environment>/`. Packages, with their manifest, go to
`build/firmware/<environment>/<version>/`. Pico builds also write a UF2 file below
`build/work/<environment>/native/`.

## Device commands

```bash
make probe ENV=<environment> [PORT=<port>]
make backup ENV=<environment> OUTPUT=<file> [PORT=<port>]
make flash ENV=<environment> [PORT=<port>]
make flash-monitor ENV=<environment> [PORT=<port>] [MONITOR_PORT=<port>]
make update ENV=<environment> [PORT=<port>]
make flash-only PACKAGE=<manifest.json> [PORT=<port>]
make flash-only-monitor PACKAGE=<manifest.json> [PORT=<port>]
make monitor ENV=<environment> [PORT=<port>]
```

The package manifest, hashes, target, flash capacity, and write bounds are checked before a
device is opened. With one likely USB or UART device the port is chosen automatically;
otherwise a prompt lists the candidates. `PORT` and `MONITOR_PORT` select them explicitly.

`flash` writes the factory package. `update` writes only the application region over the
wire; it is not network OTA.

On Bouffalo, Beken, and AmebaD, device commands print a line as soon as they try to
connect. If the boot ROM or bootloader does not answer, they repeat the board-specific
instruction (for example "hold BOOT, press and release RESET/EN, then release BOOT") with
the seconds left, for up to 15 s. Writes and full reads show one progress bar that grows in
place; without a terminal, they print a line per quarter instead.
Pico switches a running Newmota image to BOOTSEL itself; otherwise it repeats "hold BOOTSEL
while reconnecting USB" for up to 15 s, and `picotool` shows its own load, verify, and read
progress. ESP-IDF uses `esptool`'s own connection retries and progress output.

For Beken, `flash`, `flash-monitor`, and both `flash-only` forms install fresh Newmota
configuration: they initialize LittleFS and remove Newmota's credentials from NET. Native
NET vendor records are retained; foreign non-TLV configuration is replaced with an empty
native table. A damaged native table or insufficient credential space stops the operation
before any write. Bootloader, RF calibration, device identity, and BLE bonding are preserved.
Existing packages without the factory NET initialization policy must be rebuilt.

| Platform | Default upload baud | Console baud |
| --- | --- | --- |
| AmebaD | 230400 | 115200 |
| Beken | 1500000 | 115200 |
| Bouffalo | 500000 | 2000000 |
| ESP-IDF, Pico SDK | Programmer default | 115200 |

Override them with `BAUD` and `MONITOR_BAUD`.

### Platform notes

- **ESP-IDF:** ESP32-S31 images are for bring-up only; see the
  [chip notes](../../config/chips/README.md). C5, C6, and C61 Safeboot omit WPA3
  to fit their recovery slots; use a WPA2-capable network or the recovery setup AP.
  Their normal applications retain WPA3. C3 Safeboot retains WPA3-Personal but
  omits SAE-PK, OWE, and WPA3 SoftAP. S2 places Wi-Fi and non-ISR FreeRTOS code in
  flash to fit internal RAM, trading Wi-Fi throughput for available memory.
- **Beken:** there is no Safeboot. `update` preserves Wi-Fi credentials and the web password
  in NET, but RBL staging may overwrite LittleFS and its other settings/files. Network OTA
  has the same storage limitation.
- **Bouffalo:** operations use the pinned `blflashcommand` Python package, so Apple Silicon
  needs no Rosetta. Writes are verified by the loader's SHA-256 check. `flash` passes both
  partition-table copies with variants whose final `lfs` entry reaches each larger flash
  size up to 16 MiB; the write session picks one by the JEDEC capacity it reads, so no extra
  loader session runs. `update` also uses one session: it reads both table copies after
  the JEDEC check and writes only if the installed layout matches, accepting that grown entry. `probe` cannot tell
  BL616 from BL618; the environment selects the chip profile.
- **AmebaD:** reset the board manually when the uploader asks. `probe` and `backup` are not
  available.
- **Pico SDK:** operations use `picotool`. A running Newmota image usually switches to BOOTSEL
  automatically; otherwise hold `BOOTSEL` while connecting or resetting the board.

## Support level

| Platform | Device operations |
| --- | --- |
| ESP-IDF | Reference |
| Pico SDK | Factory and application flashing tested on hardware |
| Beken, Bouffalo, AmebaD | Implemented; not yet tested on hardware |

Network OTA and Safeboot recovery are not yet qualified on any platform.
