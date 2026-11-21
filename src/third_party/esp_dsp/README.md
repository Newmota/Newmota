# esp-dsp

Espressif's esp-dsp v1.8.2 (Apache-2.0, `LICENSE`) from <https://github.com/espressif/esp-dsp>,
tag `v1.8.2`, commit `a53a0756833c045311ea1d79a2badf495cdfde4c`. It holds the DSP routines
with ESP32, S3 and P4 assembly: FFT, DCT, FIR, IIR, convolution, dot product, matrix and math.

| Input | SHA-256 |
| --- | --- |
| `esp-dsp-v1.8.2.tar.gz` (`archive/refs/tags/v1.8.2.tar.gz`) | `8f257ac6348f0312c178631afa93fc115a141c40ccacb497605e8531ac22a46d` |

`modules/` is the archive's, unchanged, without its `test/` and `test_sim/` folders. Omitted:
`applications/`, `examples/`, `docs/`, `test/`, `test_app/`, the component files
(`CMakeLists.txt`, `idf_component.yml`, `component.mk`) and `Kconfig`. Without that Kconfig no
`CONFIG_DSP_*` option is set; the spectrum analyzer's build defines the one it needs.

## Consumers

| Consumer | Uses |
| --- | --- |
| [JPEGDEC](../jpegdec/README.md) with `USE_BERRY_IMAGE` | `modules/fft/include/dsps_fft2r_platform.h` only: its `dsps_fft2r_sc16_aes3_enabled` turns on JPEGDEC's ESP32-S3 SIMD |
| The [spectrum analyzer](../../app/drivers/audio/README.md#spectrum-analyzer) with `USE_BERRY_AUDIO` on the S3 and P4 | The float radix-2 FFT through `src/platform/esp-idf/fft.cpp`: `dsps_fft2r_fc32_aes3_` or `dsps_fft2r_fc32_arp4_`, the twiddle generator and the bit reversal |

The spectrum analyzer derives `USE_ESP_DSP` on the S3 and P4, which compiles the sources in
`ESP_DSP_SOURCES` (`tools/newmota/platforms/esp/build.py`) and sets `CONFIG_DSP_OPTIMIZED=1`,
the Kconfig option that enables the P4 kernel and the optimized bit-reversal dispatch.
`fft.cpp` builds the twiddle table in the analyzer's buffer instead of calling
`dsps_fft2r_init_fc32()`, which keeps one table in globals.
