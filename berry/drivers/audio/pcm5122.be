# PCM5122 stereo audio DAC on I2C, adapted from ESPHome's esphome/components/pcm5122, source pin
# 4969f643cd42ee8a9caf2c3649899bcc43a41622.
# Copyright (c) 2019 ESPHome.
# SPDX-License-Identifier: GPL-3.0-only
#
# autoexec.be:  load("pcm5122.be")  dac = PCM5122(4)  newmota.add_driver(dac)
# app:          dac.set_volume(60)  dac.set_mix(PCM5122.LEFT)
#
# PCM5122(xsmt) holds the soft-mute pin XSMT low until setup ends, nil when it is wired high;
# then the bits (16, 24 or 32, default 16) and the I2C address (0x4D) and bus. The DAC is the I2S
# target and runs its PLL from BCLK, so it needs no MCLK. Setup leaves it playing at 0 dB;
# registered as a driver, it is in standby while the output is idle.

#@ solidify:PCM5122
#@ requires:USE_I2C,USE_I2S,USE_BERRY_GPIO

class PCM5122
  static var ADDRESS = 0x4D
  # DAC data path per mix: left and right data to the left and right outputs.
  static var STEREO = 0x11
  static var LEFT = 0x12
  static var RIGHT = 0x21
  static var SWAPPED = 0x22

  var bus, address

  def init(xsmt, bits, address, bus)
    if bits == nil bits = 16 end
    if [16, 24, 32].find(bits) == nil raise "value_error", "PCM5122 takes 16, 24 or 32 bits" end
    if xsmt != nil
      import gpio
      gpio.pin_mode(xsmt, gpio.OUTPUT)
      gpio.digital_write(xsmt, 0)
    end
    self.address = address != nil ? address : self.ADDRESS
    self.bus = bus != nil ? bus : newmota.wire_scan(self.address)
    if self.bus == nil raise "io_error", "PCM5122 not found" end
    # Page 0, reset the audio modules.
    var ok = self.write(0x00, 0x00) && self.write(0x01, 0x10)
    newmota.delay(20)
    # Ignore clock halts, keep the clock divider autoset; I2S with the word length; PLL from BCLK.
    ok = ok && self.write(0x01, 0x00) && self.update(0x25, 0xF5, 0x08) &&
      self.write(0x28, bits == 16 ? 0x00 : bits == 24 ? 0x02 : 0x03) && self.set_mix(self.STEREO) &&
      self.set_analog_gain(0) && self.update(0x0D, 0x8F, 0x10) && self.mute(false) &&
      self.set_volume(100)
    if !ok raise "io_error", "PCM5122 setup failed" end
    if xsmt != nil
      import gpio
      gpio.digital_write(xsmt, 1)
    end
  end

  def write(reg, value)
    return self.bus.write(self.address, reg, value, 1)
  end

  # Keeps the bits of @p keep and sets @p value in the others.
  def update(reg, keep, value)
    var old = self.bus.read(self.address, reg, 1)
    return old != nil && self.write(reg, (old & keep) | value)
  end

  # Volume 0..100 from -52.5 dB to 0 dB in 0.5 dB steps.
  def set_volume(volume)
    volume = volume < 0 ? 0 : volume > 100 ? 100 : volume
    var step = 0x30 + int((100 - volume) * 1.05 + 0.5)
    return self.write(0x3D, step) && self.write(0x3E, step)
  end

  def mute(on)
    return self.write(0x03, on ? 0x11 : 0x00)
  end

  # Analog gain 0 or -6 dB; the register is on page 1.
  def set_analog_gain(db)
    var ok = self.write(0x00, 0x01) && self.write(0x02, db < -3 ? 0x11 : 0x00)
    return self.write(0x00, 0x00) && ok
  end

  # STEREO, LEFT (left data to both outputs), RIGHT or SWAPPED.
  def set_mix(mix)
    if [self.STEREO, self.LEFT, self.RIGHT, self.SWAPPED].find(mix) == nil
      raise "value_error", "PCM5122 mix not valid"
    end
    return self.write(0x2A, mix)
  end

  # Driver hook: audio("power", on, "tx").
  def audio(cmd, idx, payload)
    if cmd == "power" && payload == "tx" self.standby(idx == 0) end
  end

  # Standby stops the DAC and keeps the registers.
  def standby(on)
    return self.write(0x02, on ? 0x10 : 0x00)
  end
end
