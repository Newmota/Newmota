# LVGL import

The LVGL 9.6.0 core, Berry binding, generic Berry mapping library, and ctypes
support were copied from the local Tasmota checkout at revision
`c00ece86819fce87a4ad7c440b1995623ec6a5e0` on 2026-10-16:

| Newmota directory | Tasmota source |
| --- | --- |
| `lvgl/` | `lib/libesp32_lvgl/lvgl/` |
| `lv_binding_berry/` | `lib/libesp32_lvgl/lv_binding_berry/` |
| `berry_mapping/` | `lib/libesp32/berry_mapping/` |
| `berry_ctypes/` | `lib/libesp32/berry_tasmota/{include/be_ctypes.h,src/be_ctypes.c}` |
| `lv_conf.h` | `tasmota/lvgl_berry/tasmota_lv_conf.h` |

Upstream notices and license files are retained. The ctypes source library
declares MIT licensing and Stephan Hadinger as its author in `library.json`.

Local adaptations remove Tasmota custom widgets and asset/theme hooks from the
generator inputs and generated binding tables. The compact string/signature
tables otherwise retain the imported offsets. The extra bar indicator accessor
is omitted because its implementation lived outside the binding. Generic Berry
custom-widget infrastructure remains part of the callback support; Tasmota status widgets are implemented separately in the application extension
layer. Callback failure cleanup preserves the enclosing
Berry stack. Callback generated includes use Newmota's build include path.
The Berry ctypes source explicitly includes its mapping API declaration, and
generated variable descriptors are marked unused because two generated layout
constant accessors are not referenced by the LVGL module table.
The optional Chinese calendar binding is removed because its implementation is
disabled in the selected LVGL configuration. The LVGL source include directory
is provided for the binding's private bar-area accessor. Pointer and color
formatting use portable format specifiers.

The configuration retains the layout-sensitive values used by the imported
ctypes tables, uses libc allocation and standard Montserrat 14, enables shared
logging, and omits FreeType. Tasmota font lookup assets are imported separately under
`../lvgl_assets/` and registered by the application extension layer. It uses cooperative LVGL
operation with `LV_OS_NONE`. `lv_binding_conf.h` selects the standard widgets
represented by the generated binding.

Binding generation inputs are kept alongside the output:

- `lv_binding_berry/tools/{preprocessor.py,convert.py,lvgl_config.py}` produce
  the `mapping/` and `generate/` files. Run the scripts from their `tools/`
  directory when intentionally updating the binding.
- `lv_binding_berry/src/embedded/{berry_ctypes.py,lvgl_ctypes.py}` define the
  32-bit structure layouts. The latter runs from `src/embedded/`.
- `lv_binding_berry/src/embedded/*.be` and `solidify_all.be` are the sources
  for the checked-in `src/solidify/` headers. Solidification needs a matching
  Berry executable with compact maps/constants enabled, available as `berry`
  on `PATH`.
- Newmota's normal Berry prebuild scans the imported binding and support
  sources together with the runtime to create one coherent constant/string
  table. It does not regenerate the LVGL API or solidified bytecode.

The imported source, Berry binding and display integration compile for ESP32-S3
with LVGL enabled and for ESP32-P4 with display support enabled. These are native
compile checks only; LVGL runtime behavior, panel operation and hardware
qualification remain open.

Newmota's LVGL extension implements lazy `lv.start()`, the SetOption135 startup
policy, and a text splash. Its Berry clock and Wi-Fi arcs widgets are adapted from
`lib/libesp32/berry_tasmota/src/embedded/lv_tasmota_widgets.be` at the same Tasmota
revision. LVGL timers replace Tasmota driver registration; time and station signal
data come from Newmota. The production Berry source is embedded by the normal
prebuild and compiled on first start.
