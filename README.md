# Newmota

Newmota is portable firmware for ESP32, Beken, Bouffalo, AmebaD, and Raspberry Pi Pico W
devices. It is partly Tasmota-compatible: commands, GPIO templates, and the device web
interface follow Tasmota where it makes sense. See [Tasmota differences](TASMOTA_DIFFERENCES.md)
for what is deliberately different.

Newmota is an independent project. It is not part of, affiliated with or endorsed by Tasmota
or OpenBeken, so please report problems with Newmota to the Newmota project and not to them.
Newmota owes a lot to both: Tasmota for the commands, templates and Berry integration it
follows, and OpenBeken for its work on Beken and other Tuya-module chips.

The project is in active development.

## Features

- Wi-Fi setup and a device-hosted web interface
- Tasmota-style command processor and GPIO templates
- MQTT
- Berry scripting
- Settings stored in LittleFS
- Safeboot recovery image for firmware updates
- HTTPS with a per-device self-signed certificate on large builds
- Optional BLE (Berry) and I2S audio
- Optional SD card at `/sd` (FAT, SPI or SDIO) with Tasmota's pins
- Import of Wi-Fi credentials and web password from an existing Tasmota installation (ESP)

Available features depend on platform and flash size.

## Platforms

| SDK | Environments |
| --- | --- |
| ESP-IDF | `esp32`, `esp32-solo1`, `esp32s2`, `esp32s3-*`, `esp32s31`, `esp32c2`, `esp32c3`, `esp32c5`, `esp32c6`, `esp32c61`, `esp32h2`, `esp32p4*` |
| Beken | `cb2s-2m`, `cb3s`, `wb2s`, `xh-cb3s`, `generic-bk7231n-qfn32-tuya` (Tuya BK7231N modules), `generic-bk7252-2m`, `generic-bk7252-4m`, `generic-bk7252n-2m`, `generic-bk7252n-4m` (Beken SDK bootloader) |
| Bouffalo | `bl602-2m`, `bl602-4m`, `bl616dk` (also BL618 modules), `bl618dgdk` |
| AmebaD | `bw16-2m`, `bw16-4m` |
| Pico SDK | `pico-w`, `pico2-w` |

`make list` shows all environments.

## Quick start

```bash
make uv-setup                     # project Python environment
make build ENV=esp32c3            # installs the SDK on first use
make flash-monitor ENV=esp32c3    # flash over serial and open the console
```

`make help` lists all commands. See [build and device operations](tools/newmota/README.md)
for flashing, updates, backups, and monitoring.

## Configuration

`make config` creates a personal build in `user_config/` based on one of the checked-in
environments: choose platform, chip, profile, language, and features. Device settings such
as Wi-Fi, MQTT, and GPIO templates are set at runtime through commands or the web interface.

See [configuration](config/README.md) for overrides and optional drivers, and
[`TEMPLATE.md`](TEMPLATE.md) for GPIO template examples.

## Repository

| Path | Contents |
| --- | --- |
| [`src/`](src/README.md) | Application, platform adapters, third-party source |
| [`config/`](config/README.md) | Chips, boards, environments, partition layouts, SDK pins |
| [`tools/newmota/`](tools/newmota/README.md) | Build, flash, monitor, and setup tooling |
| [`language/`](language/README.md) | Web interface translations |

## License

Newmota is licensed under the GNU General Public License v3.0 or later; see [`LICENSE`](LICENSE).
Third-party code under `src/third_party/` keeps its own license.
