# Berry origin

This directory contains the portable Berry runtime and host generators carried
by Tasmota development commit
`37fed605bade755234b04e1076afbc31a7699eef` (Berry `1.1.0`).

Imported paths:

- `lib/libesp32/berry/src/`
- `lib/libesp32/berry/default/berry_conf.h`
- `lib/libesp32/berry/berry_port/`
- `lib/libesp32/berry/tools/coc/`
- `lib/libesp32/berry/LICENSE`

Tasmota application modules, examples, tests, executables, and generated
headers are deliberately excluded. Generated constant objects belong below
`build/work/<environment>/generated/berry/`.

Newmota carries these small integration changes:

- `NEWMOTA` selects Tasmota's compact embedded runtime configuration while
  leaving the Tasmota-only aligned allocator and desktop debug/solidify defaults
  disabled. The core configuration wrapper defines that integration identity;
  `default/newmota_conf.h` records the effective values for both the C compiler
  and Berry's intentionally simple COC macro scanner.
- generated-header includes use the configured include path instead of a
  source-tree-relative `generate/` directory.
- `be_bytecode.c` includes `<limits.h>` so its `LONG_MAX` test for 64-bit
  `long` integers does not depend on a transitive SDK include.
- `be_exec.c` and its Python port restore the instruction pointer and close the
  upvalues of abandoned frames after a failed protected call, so a native
  function such as `load()` can return to its Berry caller.

The C runtime, Python port, configuration, and generators must be updated from
one recorded Tasmota revision as a unit.
