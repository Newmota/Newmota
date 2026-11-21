# Tasmota settings import

Normal ESP applications import Wi-Fi credentials from an existing Tasmota installation on
ESP32, ESP32-S2, ESP32-S3, ESP32-C2, ESP32-C3, ESP32-C5, ESP32-C6, and ESP32-P4. The import
reads the 4096-byte settings layout of Tasmota 15.5.0.2 (commit
`37fed605bade755234b04e1076afbc31a7699eef`).

The import runs only when Newmota starts with default settings. It reads `/.settings`, or its
backup `/.settings.lkg`, from the preserved filesystem and copies:

- both Wi-Fi SSIDs and passwords and the preferred slot; and
- the web password, if it is 8–64 bytes long. A password outside that range is skipped
  without blocking the Wi-Fi import.

A source with malformed text or a bad CRC is rejected. The source files are left in place.
If no web password is imported, the web interface stays open until you set one with
`WebPassword`.
GPIO assignments, templates, and all other settings are not imported.

Other Tasmota versions, ESP32-H2, ESP32-S31, ESP32-C61, Safeboot images, and non-ESP platforms
are not supported. ESP32-P4 stores imported credentials, but its application has no Wi-Fi yet.

## Updating after migration

Tasmota's Safeboot stays installed. `Restart 3` (or "Restart into Safeboot" on the Firmware
Update page) enters it when ESP-IDF verifies the image in the `safeboot` partition; its web
page accepts a newer Newmota `.bin` or a Tasmota `.bin`. Tasmota's Safeboot uses its own
preserved `/.settings`, so Wi-Fi changes made under Newmota do not reach it; its recovery
access point remains available. `Upgrade 1` needs a Newmota Safeboot and reports
"Native update path unavailable" on a migrated device.
