# Source tree

Newmota keeps portable behavior and platform integration in separate source areas. The
native SDK remains the build authority for every firmware target; source files are never
compiled through a host fallback to claim platform support.

## Layout

| Directory | Current responsibility |
| --- | --- |
| [`app/`](app/README.md) | Portable application loop, commands, services, drivers, storage, and support code |
| [`platform/`](platform/README.md) | Native adapters for ESP-IDF, Beken, Bouffalo, AmebaD, Pico SDK, and shared lwIP helpers |
| [`third_party/`](third_party/) | Pinned or reviewed external components used by the application |

The public boundary between `app/` and `platform/` is declared in
`platform/include/newmota/platform/`. Vendor headers, SDK handles, native error types, and
platform task mechanisms stay below that boundary.

## Current ownership

`src/app/` owns the common command processor, cooperative scheduler, message routing,
settings transactions, LittleFS owner, Berry runtime, network-facing services, telemetry,
basic I/O, and the shared web representation. `src/platform/` supplies clocks, networking,
filesystem block devices, GPIO, web handoff, MQTT transport, boot selection, console details,
and native entry points.

The externally visible compatibility decisions are in
[Tasmota differences](../TASMOTA_DIFFERENCES.md).
