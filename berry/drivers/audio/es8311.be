# ES8311 mono audio codec (DAC and microphone ADC) on I2C, adapted from Tasmota's
# tasmota/berry/audio/es8311.be and Espressif's es8311 driver in lib/lib_deprecated/es8311,
# source pin 1fdca2bdc5b037903c7397d43cb8d86c90c9c08a.
# Copyright (c) 2019 Espressif Systems (Shanghai) Co., Ltd. (MIT); Tasmota contributors.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# autoexec.be:  load("es8311.be")  codec = ES8311(53)  newmota.add_driver(codec)
# app:          codec.set_volume(60)  codec.set_mic_gain(24)
#
# ES8311(pa) drives the amplifier enable on GPIO pa, nil for none; further values set the sample
# rate (16000), the bits (16) and the bus, such as wire2. The codec is the I2S target and takes
# MCLK at 256 x rate, 384 x rate for 24 bits, as Newmota's I2S drives it. Setup switches the
# amplifier on; registered as a driver, the amplifier follows the output and the clocks follow
# the rate of either direction.

#@ solidify:ES8311
#@ requires:USE_I2C,USE_I2S,USE_BERRY_GPIO

class ES8311
  static var ADDRESS = 0x18
  static var RATES = [8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000]
  static var RATES_24 = [8000, 16000, 32000, 48000]
  # Register and value pairs: clock and system defaults, target mode, clocks on from MCLK.
  static var SETUP = bytes("0130020003101624041005000B000C00101F117F0080013F")
  # Analog and ADC setup, then the start of ADC and DAC with the analog microphone.
  static var START = bytes("13101B0A1C6A17BF0E021200141A0D01154037484500")

  var bus, pa, rate, bits

  def init(pa, rate, bits, bus)
    self.pa = pa
    self.bus = bus != nil ? bus : newmota.wire_scan(self.ADDRESS)
    if self.bus == nil raise "io_error", "ES8311 not found" end
    if !self.write_pairs(self.SETUP) || !self.set_rate(rate, bits) ||
        !self.write_pairs(self.START) || !self.set_volume(75)
      raise "io_error", "ES8311 setup failed"
    end
    if pa != nil
      import gpio
      gpio.pin_mode(pa, gpio.OUTPUT)
      self.pa_power(true)
    end
  end

  def write(reg, value)
    return self.bus.write(self.ADDRESS, reg, value, 1)
  end

  # Keeps the bits of @p keep and sets @p value in the others.
  def update(reg, keep, value)
    var old = self.bus.read(self.ADDRESS, reg, 1)
    return old != nil && self.write(reg, (old & keep) | value)
  end

  def write_pairs(pairs)
    for i: 0 .. pairs.size() / 2 - 1
      if !self.write(pairs[2 * i], pairs[2 * i + 1]) return false end
    end
    return true
  end

  # Clock dividers for MCLK = 256 x rate, or 384 x rate with 24 bits, and the I2S word length.
  def set_rate(rate, bits)
    if rate == nil rate = 16000 end
    if bits == nil bits = 16 end
    var wide = bits == 24
    if (wide ? self.RATES_24 : self.RATES).find(rate) == nil || [16, 24, 32].find(bits) == nil
      raise "value_error", f"ES8311 has no clock setting for {rate} Hz at {bits} bits"
    end
    self.rate = rate
    self.bits = bits
    # 384 x rate / 3 x 2 gives the internal 256 x rate; 8 kHz runs at 384 x rate undivided.
    var pre = wide && rate != 8000 ? (2 << 5) | (1 << 3) : 0
    var length = bits == 16 ? 0x0C : bits == 32 ? 0x10 : 0x00
    return self.update(0x02, 0x07, pre) && self.write(0x05, 0x00) &&
      self.update(0x03, 0x80, 0x10) && self.update(0x04, 0x80, rate <= 16000 ? 0x20 : 0x10) &&
      self.update(0x07, 0xC0, 0x00) && self.write(0x08, 0xFF) && self.update(0x06, 0xC0, 0x03) &&
      self.update(0x09, 0xA0, length) && self.update(0x0A, 0xA0, length)
  end

  # Volume 0..100; 75 is 0 dB, 100 is the codec's +32 dB.
  def set_volume(volume)
    volume = volume < 0 ? 0 : volume > 100 ? 100 : volume
    return self.write(0x32, volume * 255 / 100)
  end

  def mute(on)
    return self.update(0x31, 0x9F, on ? 0x60 : 0x00)
  end

  # Microphone gain 0..42 dB in 6 dB steps.
  def set_mic_gain(db)
    var step = int(db / 6)
    return self.write(0x16, step < 0 ? 0 : step > 7 ? 7 : step)
  end

  # Driver hook: audio("power", on, "tx") and audio("rate", rate, dir).
  def audio(cmd, idx, payload)
    if cmd == "power" && payload == "tx"
      self.pa_power(idx != 0)
    elif cmd == "rate" && idx != self.rate
      if (self.bits == 24 ? self.RATES_24 : self.RATES).find(idx) == nil
        log(format("ES8311: no clock setting for %i Hz", idx), 2)
      else
        self.set_rate(idx, self.bits)
      end
    end
  end

  def pa_power(on)
    if self.pa == nil return end
    import gpio
    gpio.digital_write(self.pa, on ? 1 : 0)
  end
end
