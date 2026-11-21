# Personal build configurations

This directory is for local firmware configurations. Git tracks this README only; personal
YAML files and any other files placed here are ignored.

`make config` writes these user configurations interactively: pick the platform family, chip,
and base environment, then the profile, language, and features. `w` writes the file; `b` writes
it and builds it. Dependencies are added automatically and left out of the file; unavailable
features show why. The name follows the chosen environment until you set one.
`make config ENV=<name>` starts from a checked-in environment or a user configuration. The same
editor runs without a terminal:

```bash
uv run python tools/menuconfig.py --env esp32c3-4m --enable USE_BLE --name my-c3 --write
uv run python tools/menuconfig.py --env my-c3 --build
uv run python tools/menuconfig.py --env esp32c3-4m --list-features
```

Or create a custom environment by hand, inheriting a checked-in application environment:

```yaml
# user_config/home-c2.yaml
name: home-c2
inherit: esp32c2-2m
crypto: none
```

Build it explicitly with `make build ENV=home-c2` or `make home-c2`. `make list` displays
personal environments and their base. `make validate-catalog` validates personal YAML files
alongside the supported catalog; `make all`, the test matrix, and board selection use only
checked-in environments.

The supported overrides are `profile`, `language`, `native`, `features`, `crypto`, `device_name`,
`template`, [`solidify`](#built-in-berry-drivers), [`filesystem`](#files-in-the-factory-filesystem),
and, for ESP, `idf_version`. The board, platform, role, and Safeboot pairing come from the
checked-in base. `features` and `native` merge by key, `solidify` and `filesystem` append; other
overrides replace the inherited value. A feature's
[build settings](../config/README.md#optional-features) go under its `USE_` key. Build-time
choices such as `features.USE_WEB_HTTPS` cannot be changed on the device. Use device commands
or the supported web UI for mutable settings and credentials.

## Built-in Berry drivers

`solidify` lists Berry driver files whose classes the build puts into flash, so no upload is
needed and the classes take no heap. Only the classes a file marks with `#@ solidify:<Class>`
are built in; nothing runs at startup. `autoexec.be` still creates and registers them, which lets
one firmware carry competing drivers and each board pick its own:

```yaml
name: eye-cam
inherit: esp32s3-qio-opi-8m-8m
features:
  USE_WEBCAM: true
solidify:
  - berry/drivers/camera/ov2640.be
```

```berry
# autoexec.be
cam.register(OV2640())
```

Paths are relative to the repository; files of your own can live in `user_config/`. List every
file a class needs, bases first: `segment_display.be` before `tm1637.be`. The build refuses a
file whose `#@ requires:` features are off, a class whose base is missing or listed later, and
code other than classes at a file's top level. A file loaded at runtime replaces a built-in class
of the same name, so a fixed driver can be tried without a rebuild.

## Files in the factory filesystem

`filesystem` lists files the build writes into the factory LittleFS image. A path lands at
`/<file name>`; a mapping with `source` and `target` puts it elsewhere or renames it:

```yaml
filesystem:
  - berry/drivers/camera/ov2640.be            # lands at /ov2640.be
  - source: berry/drivers/camera/isp/ov5647.json
    target: /isp.json
  - source: user_config/eye-cam-autoexec.be
    target: /autoexec.be
```

Only a factory flash writes these files; OTA updates and Safeboot never touch the filesystem.
They are defaults: after the first boot they belong to you and may be edited or deleted. The
build refuses a file that is also in `solidify`, two entries with one target, targets inside
`/.newmota` or `/sd`, `.be` files whose `#@ requires:` features are off, and files that do not
fit the factory filesystem.

See [configuration ownership](../config/README.md#where-to-set-configuration) for where to
place supported defaults and local overrides.
