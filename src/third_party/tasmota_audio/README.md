# Carried Tasmota audio cores

These sources come from the Tasmota checkout at commit
`c00ece86819fce87a4ad7c440b1995623ec6a5e0`, from
`lib/lib_audio/ESP8266Audio` version **1.9.5** and `lib/lib_audio/mp3_shine_esp32`.
They retain their original notices and licenses. The Opus configuration is the carried
**1.3.1 fixed-point** configuration, including `DISABLE_FLOAT_API`; it has not been
replaced with an upstream library release. Generated object/library files from the
source checkout are excluded.

`USE_AUDIO_MP3`, `USE_AUDIO_AAC`, and `USE_AUDIO_OPUS` select codecs independently
of `USE_I2S`. MP3 uses the carried libmad decoder and Shine encoder. AAC supports
ADTS playback through the carried Helix core, including SBR; it does not provide
AAC encoding or MP4 demuxing. Hardware transport does not require a codec.

Project-owned compatibility headers provide ordinary memory-mapped flash reads for
`PROGMEM` data and standard C types for the Helix `Arduino.h` include. The codec
build uses `ARDUINO` solely to select Helix's generic arithmetic implementation;
there is no Arduino runtime dependency.

One source portability patch is in `libhelix-aac/assembly.h`: the generic
`U64` union selects little endian member order from the compiler's byte-order
macros, in addition to the carried Xtensa/RISC-V detection. This is required on
little endian ARM, including AmebaD and Raspberry Pi Pico. The carried fallback
remains big endian when no compiler endian macro or recognized little endian
architecture is present.

The `libhelix-aac/sbrqmf.c` generic QMF functions are also selected when `ARDUINO`
selects the portable arithmetic build. The carried GNU ARM guard otherwise expects
unavailable `sbrqmfak.s`/`sbrqmfsk.s` assembly functions, preventing native ARM links.

`audio_shine_port.cpp` compiles the unchanged Shine main source through portable
allocation shims. It reserves the encoder's core allocations in one checked arena
before entering its initializer, and releases the arena with a project-owned close
function. This avoids the carried initializer's unchecked suballocations and its
close function's access to `l3loop` after freeing it. The bitstream buffer remains
owned by Shine and is checked before use. Optional upstream cycle counters return
zero. The encoder algorithm and architecture arithmetic headers remain unchanged.

## SAM speech synthesis

`sam/` holds the C sources of `lib/lib_audio/ESP8266SAM` version 1.0.1 from the same
Tasmota commit, unmodified, with the library's own `README.md`. The wrapper
`ESP8266SAM.cpp/.h` is not carried; `audio_sam.cpp` drives the C API instead. **Licensing:**
the SAM core is a reverse-engineered port of abandonware software ("Don't ask Software");
its README states it cannot be placed under any open-source license and relies on fair
use. `USE_AUDIO_SAM` is off by default so it only enters firmware images that select it.
Output is fixed at 22.05 kHz, 8-bit unsigned mono.
