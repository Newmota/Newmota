# Build configuration

This directory is the declarative build catalog. Each kind of fact has one owner:

| Directory | Contents |
| --- | --- |
| [`chips/<platform>/`](chips/README.md) | Public chip ID, SDK target, and rare capability corrections |
| [`boards/<platform>/`](boards/README.md) | Native build recipes: chip, flash/PSRAM, console, vendor board, GPIO exposure |
| [`env/<platform>/<role>/`](env/README.md) | Firmware environments (`application/`, `safeboot/`) and test matrices |
| `partitions/<platform>/` | Flash layouts in each SDK's native format |
| `build/<platform>/<chip>/` | Checked-in native SDK defaults (`sdkconfig.defaults`, `defconfig`, `cmake.defaults`) |
| `sdk/` | SDK and host-programmer version pins, plus ESP-IDF Python constraints |
| [`hardware/`](hardware/README.md) | Frozen GPIO function IDs, pin topologies, product modules, display descriptors |

Platform directories are `esp-idf`, `beken`, `bouffalo`, `amebad`, and `pico-sdk`. An entry
may refer only to its own platform directory; platforms are never inferred from name prefixes.

A build combines the native defaults with board requirements and environment settings and
writes the result below `build/work/<environment>/config/` in the repository root. It never
edits checked-in defaults or the SDK sources.

Each `sdk/<key>.toml` pins one SDK commit of its `repository` and a stripped release archive of
that commit (`archive`, an https `.tar.xz` URL from
[Newmota/sdk-archives](https://github.com/Newmota/sdk-archives), and `archive_sha256`). Builds
install the archive into `~/.newmota/sdk/` (read-only; `NEWMOTA_SDK_PATH` moves it); downloads
are kept beside it for offline re-extraction. For SDK patch work, `make sdk-checkout SDK=<key>`
clones the pinned commit into `.sdk/<source>` (ignored by git); an existing checkout wins over the
archive. `NEWMOTA_SDK_MODE=checkout` or `archive` forces one source; `archive` affects only SDKs
that pin an archive.

Run `make validate-catalog` after changing anything here. It checks every chip, board,
environment, matrix, native default, partition selection, and hardware definition without
installing tools or compiling firmware.

## Partition layouts

Layouts are named by flash capacity, for example `esp-idf/4m.csv`. When targets on one platform
need different maps at the same capacity, `<size>-<sdk_target>.<ext>` takes precedence over
`<size>.<ext>`; for example, `bouffalo/4m-bl616.toml` serves BL616 boards with 4 MiB of flash.

Beken layouts always name the chip, because the map depends on the chip and its bootloader:
`beken/2m-bk7231n.toml`, `beken/2m-bk7238.toml`, `beken/2m-bk7252.toml` and
`beken/2m-bk7252n.toml` are the Beken SDK maps. The 4 MiB BK7231N, BK7252 and BK7252N maps reuse
the BDK's 2 MiB table, which their SDK configuration compiles in, and leave the upper 2 MiB
unused. A board whose modules
ship with a vendor bootloader names that map with `flash_layout`, which selects
`<size>-<sdk_target>-<flash_layout>.<ext>`; CB2S, CB3S and CBU use `"tuya"`, so
`beken/2m-bk7231n-tuya.toml`. WB2S uses the BK7231T Tuya map at
`beken/2m-bk7231t-tuya.toml`. A planned Beken chip without its own layout has none until it is
built. Every Beken layout keeps LittleFS outside the update area, and the factory flasher refuses
a module whose RF calibration is not where the layout says.

An OTA update is accepted only by a device with the same partition layout. Moving a device to
a different layout requires flashing its factory package.

## Where to set configuration

| Setting | Location |
| --- | --- |
| Chip and board hardware facts | `config/chips/` and `config/boards/` |
| Supported firmware defaults | `config/env/<platform>/application/*.yaml` |
| Personal build choices (profile, locale, features, crypto, native SDK settings) | [`user_config/<id>.yaml`](../user_config/README.md) |
| Wi-Fi, MQTT, GPIO template, names, `OtaUrl` | Device commands or the web UI; stored on the device |
| Default `DeviceName` for a build | `device_name` in environment YAML; a saved name still wins |
| Default GPIO template for a build | `template` in [`user_config/<id>.yaml`](../user_config/README.md); a saved template still wins |

Tasmota's `my_user_config.h` choices map by purpose: compiled features belong in environment
YAML, hardware facts in the catalogs, and mutable values in device settings. There is no
catch-all user header.

Keys of our own use `lower_snake_case`, features use `USE_` in `UPPER_SNAKE_CASE`, and keys under
`native` keep the SDK's spelling.

## Optional features

Select optional modules with a boolean `features` mapping in environment or personal YAML:

```yaml
name: workshop-c2
inherit: esp32c2-2m
features:
  USE_BH1750: true
  USE_TC74: true
```

A feature's own build settings go under its `USE_` key, in place of `true`. The feature is then
on, and all its settings are required. `true` means the feature's defaults:

```yaml
features:
  USE_MATTER:
    passcode: 20202021
    discriminator: 3840
```

Each feature declares the settings it accepts in `tools/newmota/features.py`, and anything else
is a build error. Use this for values fixed at build time; values that change on the device stay
in device settings. Write numbers without leading zeros, because YAML reads them as octal. A
setting declared as personal, such as the Matter passcode, is accepted only in `user_config/`.

Dependencies are selected automatically. Setting a dependency to `false` while selecting a
feature that needs it is an error, as are unknown names and features that the platform,
role, or flash size does not support. Optional features are application-only; the paired
Safeboot keeps its own selection. The resolved selection is written to
`build/work/<environment>/config/feature_map.json`.

Every `USE_*` feature is optional where that is technically possible. Role and profile
choices, such as MQTT and the web interface on network-capable applications, are defaults:
set `features.USE_MQTT: false` to leave one out. Turning a feature off also drops features
that are only on by default above it; a feature you explicitly turned on stops the build with
the reason it needs the other one. Not every combination has been built, so a build can still
fail to compile with a default-on feature turned off.

Each dependency's short technical reason is in `DEPENDENCY_REASONS` in
[`feature_docs.py`](../tools/newmota/feature_docs.py), and why a feature cannot be turned on is in
`UNAVAILABLE_REASONS`. Build errors show these reasons. `make validate-catalog` reports a
dependency without one.

For example, set `features.USE_NTP: false` to keep the Time service while excluding network
synchronization, with an already-set external RTC.

| Feature | Default | Notes |
| --- | --- | --- |
| `USE_I2C`, I2C sensors such as `USE_BH1750` and `USE_TC74` | Off | [I2C](../src/app/drivers/i2c/README.md), [sensors](../src/app/README.md) |
| RTC chips (`USE_DS3231`, `USE_DS1307`, `USE_BM8563`, `USE_PCF85063`, `USE_PCF85363`, `USE_RV3028`, `USE_RX8010`, `USE_RX8025`, `USE_RX8130`) | Off | Each selects the generic `USE_RTC_CHIPS` owner, Time, and I2C; application-only on all families. Select one physical model per address. [RTC behavior](../src/app/drivers/rtc/README.md) |
| `USE_GPS` | Off | UBX setup, time and navigation through configured GPS RX/TX and shared UART; excludes BL616/BL618DG. [GPS behavior](../src/app/drivers/gps/README.md) |
| `USE_HA_DISCOVERY` | Off | Home Assistant MQTT discovery, on in the `make test` environments while it is measured; selects `USE_MQTT`. [Discovery](../src/app/core/command/README.md#home-assistant-discovery) |
| `USE_NTP_SERVER` | Off | Custom applications only; IPv4 UDP123 independently serves shared UTC from NTP/GPS or trusted RTC/manual local fallback. [Quality and limits](../src/app/services/time/README.md) |
| `USE_BLE` | Off | Custom ESP, BL602, BL616, AmebaD and Pico W/Pico 2 W applications only; BL618 modules use BL616, while BL618DG BLE is deferred. |
| `USE_BLE_SENSORS` | Off | Selects `USE_BLE` and the AES-CCM primitives; MiBeacon, BTHome, CGD1 and Mi scale sensors. [BLE sensors](../src/app/README.md#ble-sensors) |
| Energy drivers such as `USE_PZEM004T` and `USE_MODBUS_ENERGY` | Off | [Energy drivers](../src/app/drivers/energy/README.md) |
| `USE_I2S`, `USE_AUDIO_MP3`, `USE_AUDIO_AAC`, `USE_AUDIO_OPUS`, `USE_AUDIO_SAM`, `USE_BERRY_AUDIO`, `USE_WEB_AUDIO` | Off | At least 4 MiB flash; not on Beken or BL602. `USE_BERRY_AUDIO` is the Berry `audio` module and needs `USE_BERRY`; `USE_WEB_AUDIO` is the Home player card and needs `USE_WEB`. [Audio](../src/app/drivers/audio/README.md) |
| `USE_DEEPSLEEP` | Off | Tasmota's `DeepSleepTime` and `DeepSleep` pin on every family; selects Time. [Deep sleep](../src/app/core/command/README.md#deep-sleep) |
| `USE_BERRY_ULP` | Off | Tasmota's Berry `ULP` module on ESP chips with a ULP coprocessor (ESP32, S2, S3, C5, C6, P4, S31); selects Berry. [Deep sleep](../src/app/core/command/README.md#deep-sleep) |
| `USE_BERRY_DRIVERS` | Off | Tasmota-style `newmota.add_driver()` hooks, `Driver` class and `newmota.add_cmd()` commands for local automation in place of Rules; selects Berry. [Berry drivers and commands](../src/app/README.md#berry-drivers-and-commands) |
| `USE_ETHERNET` | Off | Wired Ethernet beside Wi-Fi with Tasmota's `Ethernet`, `EthType`, `EthAddress`, `EthClockMode` and `Eth*` IPv4 commands: RMII PHYs on ESP32, P4 and S31, W5500, DM9051 and KSZ8851 over SPI on every ESP chip. [Network](../src/app/services/network/README.md) |
| `USE_WEBCAM` | Off | DVP camera with Berry sensor files, JPEG snapshot and MJPEG on port 81; ESP chips with a DVP controller (S3, P4, S31) on boards with PSRAM. [Camera](../src/app/drivers/camera/README.md) |
| `USE_CSI_WEBCAM` | Off | ESP32-P4 MIPI-CSI camera with ISP: MJPEG, H.264 over RTSP and WebSocket, WebRTC with I2S audio; boards with PSRAM; excludes `USE_WEBCAM`. [Camera](../src/app/drivers/camera/README.md#mipi-csi-on-the-esp32-p4) |
| `USE_BERRY_IMAGE` | Off | Tasmota's Berry `img` class: one image in a kept native buffer, JPEG decoding with JPEGDEC, regions of interest, `cam.get_image(img)`; ESP boards with PSRAM; selects Berry. [Berry images](../src/app/services/berry/modules/img/README.md) |
| `USE_CSI_STILL` | Off | Session 4 of the CSI camera: full-sensor JPEG stills (OV5647 2592x1944) through a software ISP, past the hardware ISP's 1920x1080; selects `USE_CSI_WEBCAM`. [Camera](../src/app/drivers/camera/README.md#full-sensor-stills) |
| `USE_DISPLAY` | Off | [Display](../src/app/drivers/display/README.md), [descriptors](hardware/displaydesc/README.md) |
| `USE_UNIVERSAL_TOUCH` | Off | Selects `USE_DISPLAY`; portable descriptor-driven I2C touch and optional LVGL pointer input. [Touch format and limits](../src/app/drivers/display/README.md#universal-i2c-touch) |
| `USE_DISPLAY_QSPI` | Off | Explicit four-line QSPI opt-in; selects `USE_DISPLAY`. ESP-IDF, BL616/BL618DG and Pico only. [Routes and format](../src/app/drivers/display/README.md#four-line-qspi) |
| `USE_DISPLAY_FONTS` | Off | Berry `font` class with embedded Spleen Latin-1 fonts 6x12 and 8x16; selects `USE_DISPLAY`; needs a host C compiler to build. [Fonts](../src/app/services/berry/modules/font/README.md) |
| `USE_DISPLAY_DRAW` | Off | Berry `draw` class with lines, rectangles, circles and aligned text for a `pixmat`; selects `USE_DISPLAY`. [Drawing](../src/app/drivers/display/README.md#drawing-helpers) |
| `USE_DISPLAY_FONT_5X8`, `USE_DISPLAY_FONT_12X24`, `USE_DISPLAY_FONT_16X32` | Off | Add that size to the `font` class; select `USE_DISPLAY_FONTS` |
| `USE_LVGL` | Off | Selects `USE_DISPLAY`. [LVGL](../src/app/services/berry/modules/lvgl/README.md) |
| `USE_WS2812` | Off | Not on Beken. [LED strip](../src/app/drivers/led_strip/README.md) |
| `USE_BP5758D`, `USE_BP1658CJ`, `USE_SM2335`, `USE_SM2135` | Off | Each selects `USE_LED_DRIVER`. [LED driver chips](../src/app/drivers/led_driver/README.md) |
| `USE_WEB_HTTPS` | On for Large | Off for Compact, Standard, and Safeboot. [Web interface](../src/app/services/web/README.md) |

The complete list, with dependencies and platform limits, is `OPTIONAL_FEATURES` in
[`tools/newmota/features.py`](../tools/newmota/features.py). Each one also needs a group and
summary in [`feature_docs.py`](../tools/newmota/feature_docs.py) for `make config`;
`make validate-catalog` reports a missing entry.

## Crypto

`crypto` selects which crypto APIs are compiled into the image; it is not a runtime setting
and does not affect HTTPS. Use `crypto: none`, `crypto: all`, or select Berry APIs and C
primitives individually; dependencies are added automatically:

```yaml
crypto:
  berry: [SHA256, AES_CTR]
  primitives: [ED25519]
```

When `crypto` is omitted, ESP Standard and Large applications include the Matter crypto APIs
plus SHA256, AES_GCM, MD5, and random. Other applications include SHA256, MD5, and random;
Safeboot includes none. A build that requests random fails if the platform has no entropy
source. The selection names are defined in
[`tools/newmota/features.py`](../tools/newmota/features.py).

Experimental BearSSL hardware providers are independently selectable. For example:

```yaml
features:
  USE_CRYPTO_HW_SHA256: true
```

`USE_CRYPTO_HW_SHA1` selects SHA-1, `USE_CRYPTO_HW_SHA256` selects SHA-224 and SHA-256,
and `USE_CRYPTO_HW_SHA512` selects SHA-384 and SHA-512. `USE_CRYPTO_HW_AES` selects all
AES key sizes and BearSSL CBC, CTR and CTR-CBC modes, including their use by GCM/CCM.
They also include the corresponding BearSSL primitives. Unsupported targets reject the
selection at build time. Each selected provider uses hardware for every block operation,
with no runtime software fallback; unselected providers use BearSSL software. ESP builds that
run WPA3 on BearSSL (see below) turn on `USE_CRYPTO_HW_SHA1`, `USE_CRYPTO_HW_SHA256` and
`USE_CRYPTO_HW_P256` where the target has them; the other options are off by default and need
device qualification before deployment. They do not select the SDK's HTTPS implementation.

| Target | SHA-1, SHA-224/256 | SHA-384/512 | AES-128/192/256 |
| --- | --- | --- | --- |
| ESP32 | No (cannot restore arbitrary hash state) | No | Yes |
| ESP32-S2 | Yes | Yes | Yes |
| ESP32-S3, S31, C5, P4 | Yes | Yes | No (no AES-192) |
| ESP32-C2, C3, C6, C61, H2 | Yes | No | No (no AES-192) |
| Bouffalo BL602 | Yes | No | Yes |
| Bouffalo BL616/BL618, BL618DG | Yes | Yes | Yes |
| Bouffalo BL702 | No | No | No |
| AmebaD | No (no arbitrary hash state restoration) | No | Yes |
| Beken, Pico | No | No | No |

`USE_CRYPTO_HW_P256` independently replaces BearSSL's P-256 point provider (point
multiplication, generator multiplication and two-scalar multiplication/addition) on every ESP
chip except C2, which has no suitable engine. Its two forms:

- ESP32-C5, C61, P4 and S31: the ECC engine performs point validation, multiplication and
  addition, and the field arithmetic for coordinate conversion.
- ESP32, S2, S3, C3, C6 and H2: every field multiplication runs on the MPI (RSA) engine
  through the ROM bigint driver, as in Tasmota, with constant-time point arithmetic in C
  around it. H2 uses this form because its ECC engine ignores constant-time multiplication
  before chip revision 1.2, which a build cannot know.

Both validate points and scalars, reject an infinite result, and use the SDK's ECC or MPI
lock. The default BearSSL EC dispatcher and ECDSA consumers use the provider; other curves
retain their selected implementations. There is no runtime fallback. As with the hash and AES
options, native compilation does not qualify its results or concurrency behavior on hardware.

Which Berry classes each option speeds up:

| Berry class | Hardware option |
| --- | --- |
| SHA256, HMAC_SHA256, HKDF_SHA256, PBKDF2_HMAC_SHA256 | `USE_CRYPTO_HW_SHA256` |
| AES_CBC, AES_CTR, AES_GCM, AES_CCM | `USE_CRYPTO_HW_AES` (GCM's GHASH stays software) |
| EC_P256 | `USE_CRYPTO_HW_P256`; ECDSA also hashes with `USE_CRYPTO_HW_SHA256` |
| SPAKE2P_Matter | `USE_CRYPTO_HW_P256` and `USE_CRYPTO_HW_SHA256` |
| ED25519 | `USE_CRYPTO_HW_SHA512` for its hashing; the curve stays software |
| RSA | `USE_CRYPTO_HW_SHA256` for the message hash; the RSA arithmetic stays software |
| MD5, EC_C25519, CHACHA20_POLY1305, XCHACHA20_POLY1305, BLAKE2S | None (software only) |

No Berry class uses SHA-1 or takes an AES-192 key. To measure the difference, build the same
`crypto` selection with and without these options and run
[`examples/crypto/crypto_bench.be`](../examples/crypto/crypto_bench.be) on both.

Hardware providers retain the BearSSL public context layout and copy/state semantics.
They use the SDK's shared crypto locks. DMA engines use owned aligned buffers; an engine
failure, including an ESP AES result equal to its input (the SDK's fault-injection check),
stops execution while retaining ownership, because BearSSL has no error return.
Hardware AES replaces its software key schedule and rounds together. Hash buffering,
padding and serialization, and AES mode chaining remain in the BearSSL interface layer.
The providers drive the SDK's HAL under its crypto locks themselves and do not need the
SDK's mbedTLS component; only the ESP32's block AES engine still goes through the mbedTLS
port while mbedTLS is linked, because that driver keeps its lock private.

`USE_WIFI_WPA3_BEARSSL` (ESP, needs the Newmota ESP-IDF fork) runs
WPA3-Personal on BearSSL instead of mbedTLS: SAE group 19 with hunting-and-pecking and
hash-to-element, as station and SoftAP, plus SAE-PK (access point keys on P-256 only) and
OWE as station. The point arithmetic, ECDSA verify and ECDH use BearSSL's P-256 provider,
so `USE_CRYPTO_HW_P256` also serves them. With the
fork, an image links the SDK's mbedTLS only when a feature needs it (HTTPS, or WPA3 without
this option); Compact images link none. Builds against an ESP-IDF without the fork, and
environments built without WPA3 (below 4 MB flash) and ESP-Hosted
chips (P4), whose companion runs the supplicant, reject the option.

It is on by default, application and Safeboot, in every ESP environment on ESP-IDF 5.5 that
keeps WPA3 and has its own Wi-Fi radio. The C5, C6 and C61 Safeboot images keep WPA3 only
with this option: with the mbedTLS supplicant they do not fit the 832 KiB recovery slot.
C3, C5 and C6 Safeboot leave out SAE-PK, OWE and WPA3 SoftAP. Set `USE_WIFI_WPA3_BEARSSL: false` in `features` to
build the SDK's mbedTLS supplicant instead.

On Beken (BK7231N, BK7238) the option is off by default while hardware tests are pending.
`USE_WIFI_WPA3_BEARSSL: true` replaces the BDK's `crypto_mbedtls.c` under the prebuilt
supplicant with the same BearSSL backend, plus HMAC, PBKDF2-SHA1 and SHA-384/512; without
HTTPS the image then links no mbedTLS. SAE uses group 19 only: the mbedTLS backend also
offered group 20 (P-384). The supplicant thread gets an 8 KiB stack for BearSSL's P-256 code
and its WPA2 PSK thread 2 KiB instead of 1 KiB for PBKDF2.
Every Beken Wi-Fi build takes the supplicant's random numbers from the hardware TRNG instead
of the prebuilt `os_get_random()`, which uses unseeded libc `rand()`.

## SDK logging

`SDK_LOG_LEVEL` sets the vendor SDK log level for a build: `none`, `error`, `warn` (default),
`info`, `debug`, or `verbose`. It does not affect Newmota's own logging. Beken builds default
to `none` to fit their application and update areas; an explicit `SDK_LOG_LEVEL` still applies.
BL602, BL616, and
BL618DG Safeboot images always use `none` to fit their recovery partitions.
ESP32-C5 Safeboot caps SDK logging at `error` for the same reason; `none` remains
available. These limits preserve Newmota's own logging.

```sh
make build ENV=esp32c2-2m SDK_LOG_LEVEL=none
```
