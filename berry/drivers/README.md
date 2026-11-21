# Berry drivers

Berry drivers for hardware that has no native driver. A driver is a class a script loads and
uses, not a demo: demos and benchmarks are in [`examples/`](../../examples/).

| Folder | Drivers | Needs |
|---|---|---|
| [`audio/`](audio/) | `es8311.be` (DAC and microphone ADC, amplifier enable pin), `es7210.be` (four-microphone ADC), `es8388.be` (stereo codec), `es8156.be` (stereo DAC), `es7243e.be` (stereo microphone ADC), `pcm5122.be` (stereo DAC, soft-mute pin), `tas58xx.be` (TAS5805M and TAS5825M Class-D amplifiers, power-down pin) | `USE_I2C`, `USE_I2S`; `USE_BERRY_GPIO` for the ES8311, PCM5122 and TAS58xx; see the [audio README](../../src/app/drivers/audio/README.md) |
| [`camera/`](camera/) | `ov2640.be` (DVP), `ov5647.be`, `imx219.be`, `ov02c10.be` (CSI); ISP tunings in `isp/` | `USE_WEBCAM` or `USE_CSI_WEBCAM`; see the [camera README](../../src/app/drivers/camera/README.md) |
| [`display/`](display/) | `display_text.be`: Tasmota's `DisplayText` command | `USE_DISPLAY_FONTS`, `USE_DISPLAY_DRAW`, `USE_BERRY_DRIVERS`; see the [display README](../../src/app/drivers/display/README.md) |
| [`segment_display/`](segment_display/) | `segment_display.be` (shared), `tm1637.be`, `tm1640.be`, `tm1650.be`, `max7219.be`, `max7219_matrix.be` | Free GPIOs or I2C; see the [display README](../../src/app/drivers/display/README.md) |

A driver file only defines classes; `autoexec.be` creates and registers them. Upload the file and
load it first, or build its classes in with a personal environment's
[`solidify`](../../user_config/README.md#built-in-berry-drivers) list and skip the `load()`:

```berry
load("ov2640.be")            # not needed when built in
cam.register(OV2640())
```

Each file's header shows its own lines. A camera ISP tuning is uploaded as `/isp.json`. A
personal environment's [`filesystem`](../../user_config/README.md#files-in-the-factory-filesystem)
list can ship drivers, tunings and an own `autoexec.be` with the factory flash instead.

## Writing a driver

- Only classes and comments stand at the top level. Imports go inside the methods that use them,
  and the instance, `cam.register` or `newmota.add_cmd` belong to `autoexec.be`.
- `#@ solidify:<Class>` marks every class of the file, `#@ requires:<USE_FEATURE>[,...]` the
  features it needs; the build checks both.
- A class may extend a class of another driver file, which is then listed before it.
- The build compiles in strict mode: a local may not take a built-in's name, such as `size`.
