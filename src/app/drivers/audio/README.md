# Native audio

`USE_I2S` selects the native transport, shared pin ownership, bounded audio sessions,
audio commands, and Berry `audio` module. Audio requires a firmware configuration with
at least 4 MiB of flash. Beken support is deferred. Native capabilities and pin routes depend
on the selected SoC; unsupported modes fail before a session starts. Codec selection is
independent.

**Staging driver:** audio is optional and excluded from standard builds. Correctness and
efficiency still need review; real-world hardware testing and qualification are deferred.

```yaml
name: audio-s3
inherit: esp32s3-qio-qspi
features:
  USE_I2S: true
  USE_AUDIO_MP3: true
  USE_AUDIO_AAC: true
  USE_AUDIO_OPUS: true
```

This is a personal environment example; select codecs individually to fit the firmware budget.
Carried codec origins, versions, tuning, and compatibility patches are recorded in the
[audio dependency note](../../../third_party/tasmota_audio/README.md).

| Feature | Playback | Microphone recording |
| --- | --- | --- |
| `USE_AUDIO_MP3` | MP3 through carried libmad | MP3 through carried Shine |
| `USE_AUDIO_AAC` | ADTS AAC, including SBR | Unavailable |
| `USE_AUDIO_OPUS` | Opus through carried fixed-point 1.3.1 | Opus through carried fixed-point 1.3.1 |
| `USE_AUDIO_SAM` | Text-to-speech through carried SAM (`I2SSay`) | Unavailable |

`USE_BERRY_AUDIO` (needs `USE_BERRY`) is the Berry `audio` module: `audio.Session`, the level
meter and histogram (`audio.level`), the third-octave spectrum analyzer (`audio.spectrum`, 20-40
KiB heap while running) and the tone generator (`session.tone()`). Without it, audio is commands only.

`USE_WEB_AUDIO` adds an audio player card to the web Home page for the command session: state,
file name or webradio title, pause/resume, stop, volume, and a file or URL source. It needs
`USE_WEB`; see the [web service](../../services/web/README.md).

Codec and amplifier chips on the board are set up by Berry drivers in
[`berry/drivers/audio/`](../../../../berry/drivers/audio/): the ES8311, ES7210, ES8388 and
ES7243E (I2S targets that need the `I2S MCLK` pin at 256 x the sample rate, 384 x for 24 bits on
the ES8311), the ES8156 and PCM5122 DACs (the PCM5122 clocks from BCLK) and the TAS5805M and
TAS5825M Class-D amplifiers. `autoexec.be` creates them with their power or mute pin, such as
the ES8311 amplifier enable `ES8311(53)`, the PCM5122 soft-mute `PCM5122(4)` or the TAS58xx
power-down pin `TAS5805M(5)`. Registered with `newmota.add_driver()`, they follow the Berry
driver hook `audio` (see the [application README](../../README.md#berry-drivers-and-commands)):
the ES8311 amplifier, TAS58xx deep sleep and PCM5122 standby follow the output, and the ES8311
and ES7210 clocks follow the stream rate. The hook arrives in the next Berry poll, so an
amplifier switched on by it may miss the first milliseconds of a stream.

Codecs allocate their bounded working state when opened. File input streams through
the common filesystem reader; loops seek to the beginning and reset the decoder.
A session does not load the complete file into RAM. File and network audio remains
compressed; native I2S transfers contain complete sample frames. PCM and WAV files
are not provided.

## Direction profiles and mode switching

Input (RX) and output (TX) have separate formats and modes. Both default to
`standard`; select `pdm` explicitly for a PDM microphone. Mode and controller/target
role are never inferred from GPIO assignments. `GPIO_I2S_BCLK` and `GPIO_I2S_WS`
identify signal routes for either role. The old BCLK_IN/WS_IN function IDs remain
reserved but are unavailable as active GPIO functions.

Standard TX uses BCLK, WS, and DOUT. PDM RX uses one clock and DIN: its clock defaults
to the configured WS pad, with an optional `pdm_clock` override. PDM does not require
BCLK or a dummy DOUT. `pdm_pcm: true` requests hardware PDM-to-PCM conversion;
`false` exposes a packed raw PDM bitstream, which cannot feed a PCM codec or PCM output
route directly. Native PDM capability depends on the SoC and direction. Pico and
AmebaD return `Unsupported` for PDM.

One session retains the pads of its input/output profiles and one exclusive controller.
A completed operation can restart with the other profile. Its worker joins and native
channels are disabled/deleted before the new mode opens. Compatible Standard/TDM
profiles can use paired full duplex; incompatible Standard/PDM profiles alternate.
An unrelated session cannot take over these resources. Failed native teardown retains
the lease until cleanup succeeds; `close()` releases ownership after successful cleanup.

For commands, `I2SConfig` reads the separate defaults. Set one named field, or several
fields atomically as JSON:

```text
I2SConfig Tx.mode standard
I2SConfig Rx.rate 16000
I2SConfig {"Rx":{"mode":"pdm","rate":16000,"slot_mask":1}}
I2SConfig reset
```

Additional fields are `role` (`controller`/`target`), `bits`, `slot_bits`,
`dma_frames`, `pdm_clock`, `pdm_format` (`pcm`/`raw`), and `msb_right`
(`automatic`/`enabled`/`disabled`). Setting `bits` above `slot_bits` widens the slot to match.

- `framing`: `philips` (default), `left` (MSB-justified), `right`, or `pcm` (PCM short frame:
  a one-BCLK WS pulse). ESP-IDF has no right-justified mode, Pico only Philips, AmebaD no
  PCM.
- `gain`: Tx is the playback volume in percent (0-100, default 100), Rx the microphone gain
  as a decimal (`2.5`, at most 256, default 1), kept in sixteenths as Tasmota does. The
  microphone gain scales PCM going into an encoder (recording and streaming) and clips;
  raw live routes (level meter, spectrum, custom endpoints) stay unscaled. `I2SGain` alone
  lasts until restart; any `I2SConfig` save stores the live volume and gain, as in Tasmota,
  and they apply again at startup.
- ESP-IDF register options, per direction: `ws_width` (WS width in BCLK cycles, 0 follows
  the framing), `ws_pol` (inverts the framing's WS polarity), `mclk_inv`, `bclk_inv`,
  `ws_inv` (pin inversion; in PDM mode `ws_inv` inverts the PDM clock), `apll` (audio PLL
  clock, ESP32 and ESP32-S2 only), `dma_desc` (DMA descriptors, 2-16, default 4), and on
  the ESP32-C3/S3 generation and later `left_align` (default 1), `big_endian` and
  `lsb_first`. They take 0 or 1 except the two counts. PDM uses only `ws_inv` and `apll`.
  Other backends and SoCs without a feature reject any value but the default, and so does
  `I2SConfig` itself, so a setting is never silently ignored. A full-duplex session needs
  identical options in both profiles.

The JSON form also accepts an outer `"I2SConfig"` key and Tasmota's spellings: `SampleRate`,
`SlotMask`, `SlotWidth`, `DMAFrame`, `DMADesc`, `Gain`, `SlotConfig` (0 MSB, 1 PCM,
2 Philips), `BitShift` (0 MSB, 1 Philips), `WsWidth`, `WsPol`, `LeftAlign`, `BigEndian`,
`LsbOrder`, `MclkInv`, `BclkInv`, `WsInv` and `APLL`, in either direction. Tasmota keeps the
pin inversions in its `Sys` section for port 0; here they belong to a direction. The `Sys`
section, numeric `Mode`, `Channels` (use `slot_mask`), the filter keys (`DCFilterAlpha`,
`LowpassAlpha`) and `Mp3Preallocate` are rejected by name, not ignored. Tasmota's defaults
differ in places (MSB framing on Tx, APLL on, Rx gain 30); Newmota keeps the SDK defaults
above.

A change is staged, checked against the portable limits and this SoC's capabilities,
saved, and only then applied, so a rejected command changes nothing. The profiles live in
the driver's own two slots, `/.newmota/audio.a` and `audio.b`, not in the shared settings
record, so Safeboot never reads them. The driver loads the newest valid slot at startup and
writes the defaults when neither slot is valid; a save writes the slot not holding the newest
record, so an unusable newer slot falls back to the older one. The file holds wire fields and gains, never pins; pins
always come from the hardware template, and `reset` returns to template-derived defaults
and unity gains. The profiles are the starting point for command sessions, Berry
`audio.Session` maps, the level meter and spectrum analyzer, and the WebRTC camera
microphone, so board settings made here apply everywhere. The
query's `Saved` flags show which directions were set explicitly. Changes require the
command operation to have stopped. Routes and controllers are still resolved when a
session opens. `msb_right`
controls ESP32/ESP32-S2 FIFO placement; other backends reject explicit overrides.
The default preserves each SoC's SDK behavior. It is unrelated to serial LSB order.

## Playback

The application loop reads and decodes ahead into a 16 KiB PCM ring; a separate feed
task (cooperative on Pico) is the only writer of I2S TX. Playback waits for 8 KiB of
PCM before starting, and a stalled application loop underruns only once the ring
drains. Compressed input is topped up only when it runs low, so a decode step does not
move the input buffer. `I2SGain` scales decoded PCM (0-100 percent, until the next restart or
`I2SConfig` save) within the
ring depth; raw live routes are unscaled. `I2SPause` stops decoding and feeding of a
file and a bare `I2SPlay`/`I2SLoop` resumes it. After a pause the DMA drains and then
stays silent where the backend clears TX on underrun (verified in code for ESP-IDF only).

## Speech

`I2SSay` and Berry `{'source': 'text', 'codec': 'sam', 'location': 'hello'}` render the
text with the carried SAM synthesizer and play it through the normal playback path, so
`I2SGain`, `I2SPause`, `I2SStop`, and status apply. SAM cannot render incrementally:
`start()` blocks while it renders the whole utterance into up to about 10 s of 8-bit PCM
held in 4 KiB heap chunks (it sleeps 1 ms per 4096 samples so the idle task runs). A
phrase that does not fit the heap fails with `NoMemory`. Only SAM's default voice is
exposed; there is no phonetic or sing mode. `I2STime` spells the time as words for SAM
instead of using Tasmota's recorded Talkie vocabulary, so it carries no extra data but
sounds like SAM. SAM is abandonware without an open-source
license; see the [audio dependency note](../../../third_party/tasmota_audio/README.md).

## Tone generator

The `tone` codec is for Berry, the I2S output of
[`examples/audio/rtttl.be`](../../../../examples/audio/rtttl.be). A session with
`{'source': 'text', 'codec': 'tone'}` plays 48 kHz stereo square waves until `stop()`, which
plays out the queued samples; it ignores `location`. `s.tone(hz, volume)` retunes it while it runs:
0 Hz is silence, `hz` goes up to 24000, and `volume` is 0..100 % of full scale (default 100),
then scaled by `I2SGain`. The phase carries over, so a pitch change does not click. Each change is
heard after the queued PCM, a constant delay of about 0.1 s, so the rhythm of Berry timers is kept.
One tone session exists at a time; a second start reports `Busy`.

## Level meter and histogram

The level meter registers a native endpoint named `level`. It consumes raw microphone PCM
(`source: 'microphone'`, `codec: 'none'`, `sink: 'custom'`, `location: 'level'`), which is
what Berry `audio.level.start()` starts on the `I2SConfig Rx.*` profile. Analysis is Berry
only: it is polling and reaction logic, not a console command. `audio.level` also offers `status()`, `histogram()`,
`reset()`, `offset(dB)`, `alarm(dB)` (`alarm()` disarms) and `stop()`.
One analyzer exists at a time and it excludes recording or streaming on the same input.

- **Level:** RMS of each 125 ms window relative to full scale (AES17 style: a full-scale
  sine reads -3 dB), after removing the window's DC offset. Per-sample work is integer;
  one floating-point step runs per window. `peak` is the window's largest excursion from
  its mean, and `clipped` counts windows with a sample within one 16-bit step of full scale.
- **Statistics since reset:** `Leq` (energy average), `Min`, `Max`, and `L10`/`L50`/`L90`
  (levels exceeded 10/50/90 percent of the time, 1 dB resolution).
- **Histogram:** window counts in 1 dB bins from -120 dBFS, reported by `histogram()` in
  5 dB buckets. Counts are per 125 ms window.
- **Calibration:** `offset(120)` adds that many dB to every level, for example
  120 for a microphone specified as -26 dBFS at 94 dB SPL. Without it the values are dBFS.
- **Alarm:** `alarm(85)` raises `alarm_active` when a window reaches 85 dB
  (calibrated) and clears 3 dB lower; `alarm_count` counts onsets and `alarm()` disables it.
  Poll `status()` for the state; there is no push event yet.

All channels in the profile's slot mask are averaged, so use `slot_mask 1` for one
microphone. 16-bit samples and 24/32-bit left-aligned containers are supported; raw PDM
is rejected. Offset and alarm settings are volatile.

## Spectrum analyzer

The spectrum analyzer registers a second native endpoint, `spectrum`, started by
Berry `audio.spectrum.start()` on the `I2SConfig Rx.*` profile. It excludes the level meter, recording,
and streaming while it holds the input. The analyzer is a Hann-windowed radix-2 FFT in
single-precision float. On the ESP32-S3 and P4 it runs on the vendored
[esp-dsp](../../../third_party/esp_dsp/README.md)'s assembly kernels for their vector units,
and on the RP2350 and AmebaD on the vendored
[CMSIS-DSP](../../../third_party/cmsis_dsp/README.md); every other chip uses the portable
transform in `audio_spectrum.cpp`. The choice is made at build time, and all give the same bins.

- **Frame:** 50 % overlap; the FFT size is the smallest of 512, 1024, or 2048 points whose
  bin width is at most 16 Hz (1024 points at 16 kHz). Heap use is 20 KiB at 1024 points and
  40 KiB at 2048, allocated at start and freed at stop. With CMSIS-DSP the twiddle table stays
  in flash, so it is 16 KiB and 32 KiB.
- **Bands:** ISO third-octave bands from 25 Hz to 20 kHz. Bands reaching past Nyquist, and bands
  too narrow to contain an FFT bin center, are left out, so low bands appear only at higher
  resolution. `bands()` returns the latest levels, `bands("avg")` the energy mean since
  reset, and `bands("max")` the maximum hold, as parallel `hz` and `db` lists.
- **Levels:** mean square relative to full scale per band (a full-scale sine reads -3 dB),
  so the sum of the bands is comparable to a level-meter RMS. They are dBFS; no calibration
  offset is applied.
- **Tone:** `peak_hz` is the strongest bin of the latest frame, refined below one bin by
  parabolic interpolation, with `peak_db` over three bins; useful for hum or alarm tones.
- **Load:** one transform runs at the end of each endpoint write. On a target that cannot
  keep up, older frames are skipped, so `frames` can lag the signal. Single-precision FFT
  without an FPU (RP2040, ESP32-C2) is slow; measure before relying on it there.
- `reset()` clears the average and hold. Results stay readable after `stop()`.

## Microphone capture

ESP-IDF and AmebaD run microphone reads and encoding in a dedicated native task.
Like Tasmota's recording task, it uses delay-until pacing aligned to one codec pass
(20 ms for Opus, 1152 frames for MP3), bounded to two DMA pages so the ring cannot
overflow between wakes. Raise `dma_frames` to stretch the period toward a full pass.
Sample frames are interleaved mono or stereo, and timing uses the sample rate per channel.
Encoded bytes (or raw PCM without a codec) are written straight into the ring.
Task stacks are reserved per codec. Opus retains Tasmota's sample-rate-dependent
reservation, with its variable part scaled for stereo. `worker_stack_free_bytes`
reports the minimum free task stack observed; stereo reservations still need
hardware qualification.

A 16 KiB byte ring carries encoded/raw bytes to the application loop,
which owns filesystem writes and native network/processing callbacks. That is about
one second of 128 kbit/s MP3. A sink slower than that fills the ring; capture then
fails explicitly with `Overrun` instead of silently dropping encoded packets. Native I2S overrun counters separately report
hardware/DMA loss. Stopping a recording finalizes the encoder and drains the queue.
Closing or collecting a session joins its task before releasing buffers, the codec,
or the I2S lease; this can abort an unfinished recording.

Pico runs the same capture step cooperatively on core 0. It has no RTOS task or
independent capture schedule, so application work can delay it. The worker host is
separate from the capture/queue logic to permit a later core-1 host. Opus-enabled
Pico builds reserve a larger shared core-0 stack for the carried codec's variable
length stack arrays. Their `worker_stack_free_bytes` value measures the shared
application stack. Reservation and native compilation do not establish stack margin
or streaming behavior on hardware.

## Berry

Berry configures native sessions and controls their lifecycle. It does not process
samples or invoke the command processor for session operations.

```berry
import audio
s = audio.Session({
  'source': 'file', 'sink': 'output', 'codec': 'mp3',
  'location': '/announcement.mp3', 'loop': false,
  'output': {'bclk': 18, 'ws': 19, 'dout': 21}
})
s.start()
s.status()
```

The analyzers are singletons of the `audio` module over the `I2SConfig Rx.*` profile; values are dB reals:

```berry
import audio
audio.level.offset(120)   # calibrated microphone: SPL at full scale
audio.level.alarm(85)
audio.level.start()
audio.level.status()      # {'level': 52.3, 'leq': 49.8, 'l90': 41.5, 'alarm_active': 0, ...}
audio.spectrum.start()
audio.spectrum.bands('avg')  # {'hz': [...], 'db': [...]}
```

A registered native endpoint can receive raw microphone samples without involving
Berry's command processor or moving sample buffers through the VM:

```berry
mic = audio.Session({
  'source': 'microphone', 'sink': 'custom', 'codec': 'none',
  'location': 'my-inference-endpoint',
  'input': {'bclk': 18, 'ws': 19, 'din': 22, 'rate': 16000, 'slot_mask': 1}
})
mic.start()
```

The endpoint name is resolved by native providers registered in the application.
This example requires such a provider; it is not a built-in TensorFlow feature.

Recording selects `source: 'microphone'`, `sink: 'file'`, a compiled encoder, a
file `location`, and an `input` map. A typical microphone format is
`{'bclk': 18, 'ws': 19, 'din': 22, 'rate': 16000, 'slot_mask': 1}`. Physical
pin examples must be adapted to the board and its native routes.

Input and output maps start from the `I2SConfig` Rx and Tx profiles and override them.
They accept `controller` (-1 chooses automatically), `rate`,
`bits`, `slot_bits`, `slots`, `slot_mask`, `bclk`, `ws`, `din`, `dout`, and `mclk`.
`mode` is `standard`, `tdm`, or `pdm`; `framing` is `philips`, `left`, `right`, or `pcm`;
`role` is `controller` or `target`. `pdm_pcm` selects hardware PCM conversion
rather than raw PDM. `pdm_clock`, `dma_frames`, and `msb_right` are also accepted. The native backend validates combinations against the SoC.
Decoded playback uses the stream's sample rate and channel count. A midstream
format change fails explicitly; it does not discard already queued samples.

`s.stop()` requests native finalization and drains output cooperatively. Poll
`s.status()` until state 2 (`ended`) before `s.close()` releases the endpoint.
State 0 is closed, 1 running, and 3 failed; `result` names the retained outcome.
`close()` and collection release resources immediately and can abort an unfinished
recording. Explicitly stop recording first to retain its final packet/container
trailer. VM teardown closes its sessions. Two sessions may exist simultaneously,
subject to native hardware and pin ownership; a conflict reports `Busy`.

`status()` also reports input/output byte counts, elapsed seconds, loop count,
and native overrun/underrun counters. A playback session reports `ended` after
native output drains; missing external clocks expire that drain after two seconds.
A stopped/completed session retains its hardware lease until explicitly closed.
After it reaches `ended`, `s.configure({...})` updates its next operation; absent fields
retain their values. For example, change `source`, `sink`, `codec`, and `location` to
switch from playback to recording, then call `start()` again. Retained profiles can
change mode/format on their existing pads. Close before changing to new pads or another
controller. Running sessions must be stopped and finalized before `configure()` succeeds.

## Commands

Commands use one session shared by console, web, MQTT, and `newmota.cmd()`. It
cannot take over a running Berry session or a running command session. Assign
I2S pins through the GPIO configuration before using the command defaults.

| Command | Operation |
| --- | --- |
| `I2SPlayN /file` | Streams an encoded file to configured output |
| `I2SLoopN /file` | Streams/reseeks an encoded file repeatedly |
| `I2SRecN /file` | Records microphone input to a compressed file |
| `I2SRec` | Requests recording finalization; reports `Stopping` |
| `I2SRec -?` | Reports elapsed recording seconds |
| `I2SWrN URL` | Starts HTTP/ICY network playback on Wi-Fi builds |
| `I2SWr` | Stops the command session |
| `I2SSay text` | Speaks up to 254 characters with SAM (mono, 22.05 kHz) |
| `I2STime` | Speaks the local 12-hour time ("the time is three forty five p m") with SAM; fails with `Time not set` while the clock is invalid |
| `I2SStop` | Stops the command session (Tasmota name) |
| `I2SGain` / `I2SGain 0..100` | Reads or sets decoded playback volume |
| `I2SPause` | Pauses file playback; bare `I2SPlay`/`I2SLoop` resumes |
| `I2SStream 0` / `1` | Controls the port-81 microphone HTTP listener on Wi-Fi builds |

Playback selector `N` follows Tasmota: 0 AAC, 1 MP3, 2 Opus; an omitted selector
is 0. Recording selectors are 1 MP3 and 2 Opus; an omitted selector is 1.
`?` reports command-session status. Selecting an uncompiled codec returns
`Unsupported`. Wi-Fi builds provide a dedicated outbound audio TCP connection and a separate
one-client port-81 microphone listener. They own their sockets and rings independently
of MQTT and OTA. Webradio accepts plaintext `http://` URLs, HTTP 200 and ICY 200
responses, content-length/connection-close/chunked bodies, and ICY metadata intervals. The
`icy-name` header and the latest `StreamTitle` (from the first 256 bytes of each metadata
block, cut to 127 bytes) are kept for the web player card.
HTTPS and redirects are not implemented; they fail explicitly, without a downgrade.
A stalled source or client expires after 15 seconds.

Enable `I2SStream 1` and request `/stream.mp3` (or `/stream.m3a`) for MP3, or
`/stream.webm` for Opus/WebM. The selected encoder and microphone input must be
available; otherwise the listener returns HTTP 503. Streams use plaintext HTTP with
chunked transfer framing. `I2SStream 0` stops capture and releases the listener.
Listener startup is asynchronous; a native bind failure disables the stream on the
next application poll. Builds without Wi-Fi do not provide these network endpoints.

The native session has one input, one output, and at most one codec. A TensorFlow
provider can register a bounded native endpoint for `source: 'custom'` or
`sink: 'custom'`; its `location` selects the endpoint. A raw custom route can
feed microphone PCM to an inference consumer or route custom PCM to I2S. A codec
can encode microphone/custom PCM to a custom endpoint, or decode a custom
compressed source to I2S. Endpoint callbacks run cooperatively, do not retain
the session buffer, and must return promptly. No TensorFlow or BLE endpoint is
registered by this driver. Camera synchronization, resampling, and echo
cancellation also remain separate consumers.

File and network audio paths require an explicit codec. Files are treated as
compressed media; a codec-free file-to-I2S route is rejected, and no PCM/WAV
file path is provided.
