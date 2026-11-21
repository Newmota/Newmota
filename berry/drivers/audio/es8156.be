# ES8156 stereo audio DAC on I2C, adapted from ESPHome's esphome/components/es8156, source pin
# 434ba139866a2308d5b70c0e9a95050b153f4e30.
# Copyright (c) 2019 ESPHome.
# SPDX-License-Identifier: GPL-3.0-only
#
# autoexec.be:  load("es8156.be")  dac = ES8156()
# app:          dac.set_volume(60)
#
# ES8156(bus) takes the bus, such as wire2. The DAC is the I2S target with its clocks derived by
# the chip; setup leaves it playing at 0 dB.

#@ solidify:ES8156
#@ requires:USE_I2C,USE_I2S

class ES8156
  static var ADDRESS = 0x08
  # Register and value pairs: software mode, analog system with line out, fast state machine,
  # I2S 24 bits, EQ off, no channel cross, all clocks on, reset and start, analog power-up.
  static var SETUP = bytes("0204202A213C2200240723000A010B01110019200D1409001800083F000200032520")

  var bus

  def init(bus)
    self.bus = bus != nil ? bus : newmota.wire_scan(self.ADDRESS)
    if self.bus == nil raise "io_error", "ES8156 not found" end
    if !self.write_pairs(self.SETUP) || !self.set_volume(75)
      raise "io_error", "ES8156 setup failed"
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

  # Volume 0..100; 75 is 0 dB.
  def set_volume(volume)
    volume = volume < 0 ? 0 : volume > 100 ? 100 : volume
    return self.write(0x14, volume * 255 / 100)
  end

  def mute(on)
    var old = self.bus.read(self.ADDRESS, 0x13, 1)
    return old != nil && self.write(0x13, (old & 0xF9) | (on ? 0x06 : 0x00))
  end
end
