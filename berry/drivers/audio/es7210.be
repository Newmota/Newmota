# ES7210 four-channel microphone ADC on I2C, adapted from Espressif's es7210 driver in Tasmota's
# lib/lib_deprecated/es7210 and its setup in xdrv_42_4_i2s_codecs.ino, source pin
# 1fdca2bdc5b037903c7397d43cb8d86c90c9c08a.
# Copyright (c) 2021 Espressif Systems (Shanghai) Co., Ltd. (MIT); Tasmota contributors.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# autoexec.be:  load("es7210.be")  adc = ES7210()  newmota.add_driver(adc)
# app:          adc.set_mic_gain(30)
#
# ES7210(mics) selects the microphones as a mask of MIC1..MIC4, MIC1 | MIC2 by default; MIC1 and
# MIC2 come out on SDOUT1, MIC3 and MIC4 on SDOUT2. Further values set the sample rate (16000),
# the bits (16 or 32) and the bus, such as wire2. The ADC is the I2S target and takes MCLK at
# 256 x rate, as Newmota's I2S drives it. Selected MIC1 and MIC2 start at 37.5 dB, as on
# Tasmota's ESP32-S3-BOX; MIC3 and MIC4 at 0 dB. Registered as a driver, the clocks follow the
# input rate.

#@ solidify:ES7210
#@ requires:USE_I2C,USE_I2S

class ES7210
  static var ADDRESS = 0x40
  static var MIC1 = 0x01
  static var MIC2 = 0x02
  static var MIC3 = 0x04
  static var MIC4 = 0x08
  static var RATES = [16000, 44100, 48000, 64000]
  # Register and value pairs: reset, clocks off, state cycles, analog power, 2.87 V bias, OSR.
  static var SETUP = bytes("00FF0041011F09300A3040C341704270072002C1")

  var bus, mics, rate, bits

  def init(mics, rate, bits, bus)
    self.mics = mics != nil ? mics & 0x0F : self.MIC1 | self.MIC2
    self.bus = bus != nil ? bus : newmota.wire_scan(self.ADDRESS)
    if self.bus == nil raise "io_error", "ES7210 not found" end
    if !self.write_pairs(self.SETUP) || !self.set_rate(rate, bits) || !self.select() ||
        !self.set_mic_gain(37.5, self.mics & (self.MIC1 | self.MIC2)) || !self.start()
      raise "io_error", "ES7210 setup failed"
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

  # Clock dividers for MCLK = 256 x rate, the word length and the I2S format.
  def set_rate(rate, bits)
    if rate == nil rate = 16000 end
    if bits == nil bits = 16 end
    if self.RATES.find(rate) == nil || [16, 32].find(bits) == nil
      raise "value_error", f"ES7210 has no clock setting for {rate} Hz at {bits} bits"
    end
    self.rate = rate
    self.bits = bits
    # adc_div 1 with the DLL; the clock doubler is off at 64 kHz. LRCK divider 256, OSR 32.
    return self.write(0x02, rate == 64000 ? 0x81 : 0xC1) && self.write(0x07, 0x20) &&
      self.write(0x04, 0x01) && self.write(0x05, 0x00) &&
      self.update(0x11, 0x1C, bits == 16 ? 0x60 : 0x80) && self.write(0x12, 0x00)
  end

  # Driver hook: audio("rate", rate, "rx").
  def audio(cmd, idx, payload)
    if cmd != "rate" || payload != "rx" || idx == self.rate return end
    if self.RATES.find(idx) == nil
      log(format("ES7210: no clock setting for %i Hz", idx), 2)
    else
      self.set_rate(idx, self.bits)
    end
  end

  # Powers the PGA and ADC pairs of the selected microphones.
  def select()
    var ok = true
    for i: 0 .. 3
      ok = self.update(0x43 + i, 0xEF, 0x00) && ok
    end
    ok = self.write(0x4B, 0xFF) && self.write(0x4C, 0xFF) && ok
    if self.mics & (self.MIC1 | self.MIC2)
      ok = self.update(0x01, 0xF4, 0x00) && self.write(0x4B, 0x00) && ok
    end
    if self.mics & (self.MIC3 | self.MIC4)
      ok = self.update(0x01, 0xEA, 0x00) && self.write(0x4C, 0x00) && ok
    end
    for i: 0 .. 3
      if self.mics & (1 << i) ok = self.update(0x43 + i, 0xEF, 0x10) && ok end
    end
    return ok
  end

  def start()
    return self.write(0x06, 0x00) && self.write(0x47, 0x00) && self.write(0x48, 0x00) &&
      self.write(0x49, 0x00) && self.write(0x4A, 0x00) && self.select()
  end

  # Gain 0..37.5 dB for the microphones of @p mics, all selected ones by default: 3 dB steps up
  # to 33 dB, then 34.5, 36 and 37.5 dB.
  def set_mic_gain(db, mics)
    if mics == nil mics = self.mics end
    var step = db <= 33 ? int(db / 3) : 11 + int((db - 33) / 1.5)
    step = step < 0 ? 0 : step > 14 ? 14 : step
    var ok = true
    for i: 0 .. 3
      if mics & (1 << i) ok = self.update(0x43 + i, 0xF0, step) && ok end
    end
    return ok
  end
end
