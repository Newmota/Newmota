# CMSIS-DSP

Arm's CMSIS-DSP v1.18.0 (Apache-2.0, `LICENSE`) from
<https://github.com/ARM-software/CMSIS-DSP>, tag `v1.18.0`, commit
`4b6f26593e9d3b24a9bc93b3646a3a859238eae2`. It holds signal-processing routines for Cortex-M
and Cortex-A cores.

| Input | SHA-256 |
| --- | --- |
| `CMSIS-DSP-1.18.0.tar.gz` (`archive/refs/tags/v1.18.0.tar.gz`) | `b60944180f51f9bb50df1a8cccad19d7c1ef8c14ed13da50b9b3f141032c3f56` |

The files here are the archive's, unchanged, at their original paths. Only the float complex
FFT and what it includes are kept: `arm_cfft_f32.c`, `arm_cfft_radix8_f32.c`,
`arm_bitreversal2.c`, `arm_common_tables.c`, `arm_const_structs.c` and their headers. Every
other source and header is omitted, as are `Testing/`, `Examples/`, `Documentation/`, the
Python wrapper and the build files.

## Consumers

| Consumer | Uses |
| --- | --- |
| The [spectrum analyzer](../../app/drivers/audio/README.md#spectrum-analyzer) with `USE_BERRY_AUDIO` on the RP2350 and AmebaD | `arm_cfft_f32` through `src/platform/cmsis_fft.cpp`, with the constant 512, 1024 and 2048 point tables |

The spectrum analyzer derives `USE_CMSIS_DSP` on these Cortex-M33 targets, which compiles
`CMSIS_DSP_SOURCES` (`tools/newmota/features.py`). `cmsis_compiler.h` comes from each SDK's
CMSIS-Core headers.

The AmebaD SDK needs three adjustments:

- It has older copies of `arm_common_tables.h` and `arm_const_structs.h` on its include path, so
  its build puts these folders first with `-iquote`.
- Its CMSIS-Core lacks `__STATIC_FORCEINLINE`, which `src/platform/amebad/arm_dsp_config.h`
  supplies through `ARM_DSP_CUSTOM_CONFIG`.
- Its ROM symbols define `twiddleCoef_1024` and `armBitRevIndexTable1024`, which take the place
  of the copies here. The SDK's source for both tables has the same values and lengths.
