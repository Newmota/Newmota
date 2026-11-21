# JPEGDEC

Larry Bank's JPEGDEC (Apache-2.0, `LICENSE`) from <https://github.com/bitbank2/JPEGDEC>,
commit `ff585f2fd2bdc2c5776a368aaae818071e1b60a5` on `master`. It is version 1.8.5 plus the
eight commits after the `1.8.5` tag: fixes for buffer overflows and invalid component counts,
ESP32-P4 and S31 SIMD, RGB8888 grayscale output and progressive-decoding fixes. Tasmota
vendors the same commit. `USE_BERRY_IMAGE` compiles it for the Berry `img` class.

| Input | SHA-256 |
| --- | --- |
| `JPEGDEC-ff585f2.tar.gz` (`archive/ff585f2fd2bdc2c5776a368aaae818071e1b60a5.tar.gz`) | `47f9010370e4fe97f44ffaa9b18061f17aed0c7340bdd397d5de1fb85e399020` |

`src/` holds these files of the archive's `src/`, unchanged: `JPEGDEC.cpp`, `JPEGDEC.h`,
`jpeg.inl`, `s3_simd_420.S`, `s3_simd_444.S`, `p4_simd_420.S`, `p4_simd_444.S`. Omitted:
`JPEGDisplay.*` (display helper), `my_cm4_simd.h` (Teensy only), `s3_simd_dequant.S` (its
function is declared but never called) and `s3_simd_idct.S_FUTURE`, as well as the examples,
test images and desktop ports.

## Port

JPEGDEC selects its ESP32 SIMD through Arduino board defines and, on the S3, esp-dsp's
`dsps_fft2r_platform.h`. Newmota builds on ESP-IDF without Arduino, so `port/` compiles the
unchanged sources with the defines Arduino would set. The S3 header comes from the vendored
[esp-dsp](../esp_dsp/README.md), whose FFT include folder is on JPEGDEC's include path:

| File | Role |
| --- | --- |
| `port/jpegdec_port.cpp` | Defines `ARDUINO_ARCH_ESP32` on the S3 or `ARDUINO_ESP32P4_DEV` on the P4 for this translation unit only, then includes `JPEGDEC.cpp` |
| `port/jpegdec_simd.S` | Assembles the S3 or P4 colour-conversion files with the same defines; empty on other targets |

Other ESP chips use JPEGDEC's portable C code.
