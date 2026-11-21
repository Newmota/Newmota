# Build tooling

The build uses Python, Make, and each platform's pinned native SDK build system; there is no
other build framework. `make` is the public interface and forwards to
`PYTHONPATH=tools python -m newmota` (run it with `--help`). Setup, build, and device
commands are described in the [command guide](newmota/README.md).

## Ownership map

```text
tools/
  menuconfig.py             User build configurator and build launcher (`make config`)
  record_compile.py         Standalone compiler recorder for compilation databases
  format_cpp.py             Pinned C/C++ formatting for first-party sources
  p256_comb_table.py        Generates or checks the ESP MPI P-256 generator comb table
  tests/                    Host tests and generated-header fixtures
  newmota/
    __main__.py             Argument parsing and lazy command dispatch
    model.py                Repository paths, immutable data, build adapter contract
    catalog.py              Catalog parsing, board schema, validation orchestration
    config.py               Native config layering and identity
    features.py             Feature selection, dependencies, source ownership
    feature_docs.py         Feature summaries and why each dependency exists
    configurator.py         Personal configuration editing and export
    energy_inventory.py     Frozen energy-driver ID inventory
    crypto.py               Pinned BearSSL identity and selected crypto source sets
    hardware.py             GPIO function/module catalog and generated descriptors
    topology.py             Chip pin topology and generated pin mappings
    i18n.py                 Locale catalogs and compressed web page generation
    berry.py                Berry generation and incremental inputs
    berry_drivers.py        Checks of the Berry driver files a build solidifies
    factory_files.py        Checks of the files a build ships in the factory filesystem
    matter_qr.py            Fixed Matter setup codes printed as a terminal QR code
    lvgl.py                 LVGL source and binding staging
    audio.py                Audio codec source selection
    pipeline.py             Selection, dependency ordering, lifecycle, native manifest
    package.py              Manifest safety, layout facts, factory LittleFS images
    ota.py                  Layout-bound OTA envelope
    device.py               Device contract, port selection, device workflows
    host.py                 Processes, SDK pins, toolchains, common staging helpers
    compdb.py               Compilation database collection and activation
    platforms/
      __init__.py           One explicit, lazy platform registry
      esp/                  Public selector esp; native SDK directory esp-idf
      bouffalo/
      beken/
      amebad/
      pico/                 Public selector pico; native SDK directory pico-sdk
```

Each platform owns these files:

| File | Responsibility |
| --- | --- |
| `__init__.py` | Data-only platform declaration; never imports an SDK or device library |
| `build.py` | Setup, native requirements and target checks, build adapter |
| `images.py` | Native layout resolution, image checks, packaging, paired Safeboot checks |
| `device.py` | Programmer protocol, write bounds, verification, monitoring |

Larger specialist code stays in its own file: Beken has `bdk.py`, `codec.py`, and
`profile.py`; AmebaD has `uploader.py` and `compiler_gate.c`; ESP has `usb_monitor.py`. The
Beken profile is shared by the builder and image code and must not depend on either.

## Dependency rules

- Shared modules select a platform through `platforms`; they never import a platform
  implementation directly.
- A platform may import shared modules and its own helpers, never another platform.
- Common package validation does not import native packagers.
- Device implementations are loaded only for device operations.
- The catalog holds declarative board rules and capability derivation; native target and
  SDK constraint checks belong in the platform's `build.py`.

These rules are enforced by review, not by a test.

## Build lifecycle

```text
resolve → prepare → configure → pre-build → build → post-build → package
```

`pipeline.py` builds a paired Safeboot first, once. Platform adapters translate the resolved
features, configuration, and layout into their SDK's inputs; the SDK owns compilation and
linking. Keep Beken's audited compiler flags and runtime symbol validation.

## Adding a feature

1. Add sources, dependencies, profile defaults, role restrictions, and supported platforms to
   `OPTIONAL_FEATURES` in `features.py`. Required role features stay in `feature_values()`.
2. Add platform sources, SDK components, and native settings in the platform's
   `build.native_requirements()`. Keep generated source order deterministic.
3. Change `images.py` only if the feature changes layout or packaging, and `device.py` only if
   a device operation changes.
4. Follow the [validation ladder](../AGENTS.md#validation-ladder). Use
   `make clangd ENV=<environment>` when you need a compilation database.

## Adding a platform

Start with the four platform files, implement the `PlatformAdapter` and `DeviceAdapter`
contracts, register the platform, and add its public-to-SDK directory mapping in `model.py`.
Add its catalog, SDK pin, native project inputs, and a test-matrix entry. Unimplemented device
operations must fail closed; an adapter does not by itself qualify a platform for flashing.

## Incremental builds and interpreters

Build identity includes every shared Python module, the registry, and the selected platform's
Python files, so any change there reconfigures that platform. SDK pins, partition inputs, and
resolved feature, hardware, and layout identities are tracked separately. Generated inputs are
written only when their content changes.

Setup runs under an isolated host Python, not the project virtual environment. Modules on the
setup path must not import optional dependencies at import time; SDK pins are read with the
small standard-library parser in `host.py`.

`record_compile.py` runs as a standalone subprocess with absolute paths, because native
compilers run from different working directories. ESP's USB monitor runs as a separate script
in the ESP Python environment. AmebaD's uploader is integrity-pinned in
`config/sdk/amebad.toml`; update that hash with every reviewed change to the uploader.

## C/C++ formatting

`make format-cpp` applies the checked-in `.clang-format` with the locked `clang-format`
development dependency to tracked sources under `src/app/`, `src/platform/`, and
`tools/tests/`; `make format-check-cpp` checks them. Vendored code in `src/third_party/` and
build output are excluded. On Windows, run the script through `uv`:

```powershell
uv run --locked --extra dev python tools/format_cpp.py --write
uv run --locked --extra dev python tools/format_cpp.py --check
```

The clangd database is normalized for editor parsing and is not a lint database.

## Safety checks that must survive refactors

Package loading validates path containment, file hashes and sizes, artifact roles,
qualification, flash capacity, and non-overlapping writes before any device access. Native
packagers also enforce platform image formats, paired Safeboot target/SDK/source/layout
identity, and reserved regions. Keep platform differences explicit; similar checks are not
interchangeable (Pico Safeboot, for example, must not embed a partition table).

A browser recovery updater validates the application target, slot bounds, chunk offsets,
native image and checksum rules, and activation state before selecting a new application. It
never writes Safeboot, the bootloader, partition metadata, or LittleFS, and an interrupted or
invalid upload leaves Safeboot usable. Password, session, Host/origin, CSRF, rate-limit, and
fail-closed behavior apply even over HTTP. Uploads use bounded sequential chunks, never a
whole-image RAM buffer.

Application-only updates never build or write Safeboot. Mocked protocol tests and native
builds do not replace probe, backup, write/read-back, boot, and recovery tests on the exact
board.
