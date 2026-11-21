# Embedded LVGL fonts

These generated font sources were imported from the local Tasmota checkout at
`c00ece86819fce87a4ad7c440b1995623ec6a5e0` on 2026-10-17:

- `lib/libesp32_lvgl/LVGL_assets/src/fonts/montserrat/`: Montserrat 10/14/20/28
  with Tasmota's symbol repertoire.
- `lib/libesp32_lvgl/LVGL_assets/src/fonts/seg7_*.c`: DSEG7 faces.
- `lib/libesp32_lvgl/LVGL_assets/src/fonts/icons/`: icon-only 12/16/18/22/24 faces.

The generated notices, generation arguments, upstream asset library metadata, and
DSEG license are preserved. The only source adaptation changes the nested LVGL
header include to `lvgl.h`, matching Newmota's SDK include paths. The source font
inputs include Montserrat Medium, DSEG7 Classic Bold, Material Design Icons and
Font Awesome as listed in each generated file. No logo images or HASPmota assets
are included.

Newmota selects these files only with `USE_LVGL`. Font lookup and family aliases
are owned by the application LVGL runtime; generated bindings remain unchanged.
