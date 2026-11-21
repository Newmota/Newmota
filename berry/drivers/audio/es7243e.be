# ES7243E stereo microphone ADC on I2C, adapted from ESPHome's esphome/components/es7243e, source
# pin 0cd16df16fc42409c7008e51e00d345343157464.
# Copyright (c) 2019 ESPHome.
# SPDX-License-Identifier: GPL-3.0-only
#
# autoexec.be:  load("es7243e.be")  adc = ES7243E()
# app:          adc.set_mic_gain(30)
#
# ES7243E(db) sets the microphone gain, 24 dB by default; then the bus, such as wire2. The ADC
# is the I2S target and takes MCLK at 256 x the sample rate, as Newmota's I2S drives it.

#@ solidify:ES7243E
#@ requires:USE_I2C,USE_I2S

class ES7243E
  static var ADDRESS = 0x10
  # Register and value pairs: reset and clock setup, I2S format, ADC and analog setup.
  static var SETUP = bytes("013A0080F90004020401F901001E01000200032004010D0005000603070008FF09CA0A850B000EBF0F80140C150C1702182619771AF41B661C441E001F0C201A211A0080013A163F1600F90004011701")
  # Restart after the gain is set.
  static var START = bytes("0080013A163F1600")

  var bus

  def init(db, bus)
    self.bus = bus != nil ? bus : newmota.wire_scan(self.ADDRESS)
    if self.bus == nil raise "io_error", "ES7243E not found" end
    if !self.write_pairs(self.SETUP) || !self.set_mic_gain(db != nil ? db : 24) ||
        !self.write_pairs(self.START)
      raise "io_error", "ES7243E setup failed"
    end
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

  # Gain 0..37.5 dB for both channels: 3 dB steps up to 33 dB, then 34.5, 36 and 37.5 dB.
  def set_mic_gain(db)
    db = (db < 0 ? 0 : db > 37.5 ? 37.5 : db) + 0.5
    var step = db <= 33 ? int(db / 3) : db < 36 ? 12 : db < 37 ? 13 : 14
    return self.write(0x20, 0x10 | step) && self.write(0x21, 0x10 | step)
  end
end
