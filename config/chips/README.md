# Chips

Chip files provide the small public-to-SDK identity boundary. The filename is Newmota's
public chip ID; `sdk_target` is the exact target understood by the selected SDK.

```json
{
  "sdk_target": "bl618dg"
}
```

The only optional fields are:

- `build_status`: `native` (the default) or `planned`. A planned chip remains in the
  catalog, but cannot be selected for a build until its SDK and adapter are integrated.
- `idf_version`: ESP-only `"5.5"` (the default) or `"6.1"`; environment YAML may override it.
- `capability_overrides`: narrowly scoped corrections to SDK-derived capability data.

Newmota derives ESP-IDF capabilities from the pinned target's `soc_caps.h`. Capabilities
for SDKs without a trustworthy machine-readable source remain unknown unless a verified
override is necessary. Capabilities supplied by companion devices belong to the board
recipe, not the SoC. Chip JSON must not copy RAM sizes, peripheral tables, board wiring,
flash topology, prose notes, or source links.

ESP32-P4X is ESP32-P4 revision 3.x silicon: it maps to SDK target `esp32p4` and its board
recipes require revision 3.0 or later. ESP32-P4X and ESP32-S31 default to IDF 6.1. The S31
target is an SDK preview whose bootloader accepts images with an invalid SHA-256 hash; use it
for bring-up only.
