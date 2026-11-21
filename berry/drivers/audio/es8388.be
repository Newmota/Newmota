# ES8388 stereo audio codec (DAC and ADC) on I2C, adapted from ESPHome's
# esphome/components/es8388, source pin 434ba139866a2308d5b70c0e9a95050b153f4e30.
# Copyright (c) 2019 ESPHome.
# SPDX-License-Identifier: GPL-3.0-only
#
# autoexec.be:  load("es8388.be")  codec = ES8388()
# app:          codec.set_volume(60)  codec.set_input(ES8388.LINE2)
#
# ES8388(output, adc_input) selects the outputs, LINE1 (LOUT1/ROUT1), LINE2 (LOUT2/ROUT2) or BOTH
# (default), and the ADC input, LINE1 (LIN1/RIN1, default), LINE2 or DIFFERENCE; then the bus,
# such as wire2. The codec is the I2S target with 16 bits and takes MCLK at 256 x the sample
# rate, as Newmota's I2S drives it. Setup leaves it playing at 0 dB with 0 dB microphone gain and
# the ALC set for voice.

#@ solidify:ES8388
#@ requires:USE_I2C,USE_I2S

class ES8388
  static var ADDRESS = 0x10
  static var LINE1 = 0
  static var LINE2 = 1
  static var BOTH = 2
  static var DIFFERENCE = 2
  # DACPOWER per output and ADCCONTROL2 per input.
  static var OUTPUT_POWER = [0x30, 0x0C, 0x3C]
  static var INPUT_SELECT = [0x00, 0x50, 0xF0]
  # Register and value pairs: I2S target, power and reference, DAC and ADC 16-bit I2S at 256 x
  # rate, DAC to mixer, ADC and line in off, ALC for voice recording, state machine start.
  static var SETUP = bytes("080001500200001217181802260027902A902B802D0003FF0B020C0D0D021000110012E213A01412150616C32B8002F0")
  # Unmute ADC and DAC with fade-in, power on ADC with LIN and RIN.
  static var UNMUTE = bytes("0F6019200309")

  var bus

  def init(output, adc_input, bus)
    self.bus = bus != nil ? bus : newmota.wire_scan(self.ADDRESS)
    if self.bus == nil raise "io_error", "ES8388 not found" end
    var ok = self.mute(true) && self.write_pairs(self.SETUP) && self.set_mic_gain(0)
    newmota.delay(1)
    ok = ok && self.write(0x02, 0x00) && self.mute(false) && self.write_pairs(self.UNMUTE) &&
      self.set_output(output != nil ? output : self.BOTH) &&
      self.set_input(adc_input != nil ? adc_input : self.LINE1) && self.set_volume(100)
    if !ok raise "io_error", "ES8388 setup failed" end
  end

  def write(reg, value)
    return self.bus.write(self.ADDRESS, reg, value, 1)
  end

  def write_pairs(pairs)
    for i: 0 .. pairs.size() / 2 - 1
      if !self.write(pairs[2 * i], pairs[2 * i + 1]) return false end
    end
    return true
  end

  # Volume 0..100 from -96 dB to 0 dB in 0.5 dB steps.
  def set_volume(volume)
    volume = volume < 0 ? 0 : volume > 100 ? 100 : volume
    var step = int((100 - volume) * 1.92 + 0.5)
    return self.write(0x1A, step) && self.write(0x1B, step)
  end

  # Only the DACMute bit; the other bits of DACCONTROL3 hold unrelated settings.
  def mute(on)
    var old = self.bus.read(self.ADDRESS, 0x19, 1)
    return old != nil && self.write(0x19, (old & 0xFB) | (on ? 0x04 : 0x00))
  end

  # Microphone gain 0..24 dB in 3 dB steps for both channels.
  def set_mic_gain(db)
    var step = int(db / 3)
    step = step < 0 ? 0 : step > 8 ? 8 : step
    return self.write(0x09, step * 0x11)
  end

  # LINE1, LINE2 or BOTH at 0 dB output volume.
  def set_output(line)
    if line < self.LINE1 || line > self.BOTH raise "value_error", "ES8388 output not valid" end
    var out1 = line != self.LINE2 ? 0x1E : 0x00
    var out2 = line != self.LINE1 ? 0x1E : 0x00
    return self.write(0x2E, out1) && self.write(0x2F, out1) && self.write(0x30, out2) &&
      self.write(0x31, out2) && self.write(0x04, self.OUTPUT_POWER[line])
  end

  # LINE1, LINE2 or DIFFERENCE.
  def set_input(line)
    if line < self.LINE1 || line > self.DIFFERENCE raise "value_error", "ES8388 input not valid" end
    return self.write(0x0A, self.INPUT_SELECT[line])
  end
end
