# Environments

An environment is one firmware target. Each is a YAML file below a platform and role
directory:

```text
config/env/
├── matrices/
└── <platform>/
    ├── application/
    └── safeboot/       # only on platforms with a Safeboot implementation
```

The environment ID is used unchanged as the filename, `name`, Make target, build manifest,
firmware filename, and deployment-server path. IDs use lowercase kebab-case.

## Fields

```yaml
# config/env/esp-idf/application/esp32s3-qio-qspi.yaml
name: esp32s3-qio-qspi
board: esp32s3-qio-qspi
role: application
profile: Standard
safeboot: esp32s3-qio-qspi-safeboot
language: en_GB
```

| Field | Meaning |
| --- | --- |
| `name` | Environment ID; must match the filename |
| `board` | A recipe in `config/boards/<platform>/` |
| `role` | `application` or `safeboot`; must match the containing directory |
| `profile` | Feature baseline: `Compact`, `Standard`, `Large`, or `Safeboot` |
| `safeboot` | Optional Safeboot environment built and packaged with this application |
| `language` | Build-time web UI locale from `language/<locale>.toml`, such as `en_GB` or `de_DE` |
| `features`, `crypto` | Optional build selections; see [build configuration](../README.md#optional-features) |
| `idf_version` | ESP only: `"5.5"` or `"6.1"`; overrides the chip default |
| `native` | Native SDK settings applied as the final configuration layer |
| `policy` | `true` makes this application the fallback for `make build BOARD=<board>` |

The `language` setting is not a runtime language switch. Commands, JSON keys, and MQTT topics
remain English.

A referenced Safeboot environment must use `role: safeboot`, `profile: Safeboot`, and the same
platform, SDK target, and board as the application. Building an application builds its
Safeboot first. ESP-IDF publishes a factory package only for an application with a Safeboot.
ESP32-P4 Safeboot images have no network recovery page.

`native` values replace checked-in SDK defaults and each replacement is recorded in the
generated configuration manifest:

```yaml
native:
  CONFIG_EXAMPLE_SETTING: y
```

Personal environments in `user_config/` inherit one of these files; see the
[user configuration guide](../../user_config/README.md).

## Board builds

`make build BOARD=<board>` builds any board recipe. It uses the same-named application
environment when one exists. Otherwise it uses the platform's single `policy: true`
application, keeping that environment's language and native settings and taking the profile
and flash size from the board.

## Test matrix

`matrices/test.txt` lists the environments that `make test` builds: one representative per
platform family. `make all` builds every environment.
