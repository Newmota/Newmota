# MAX7219 seven-segment modules on three free GPIOs, adapted from Tasmota's xdsp_15_tm1637.ino,
# source pin 1e03a6d9a579344a82b3b254461467409c0e2140. Copyright (C) 2021 Ajith Vasudevan.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# autoexec.be:  load("segment_display.be")  load("max7219.be")  load("app.be")
# app.be:       var seg = Max7219(DIN_PIN, CS_PIN, CLK_PIN)  seg.set_on(4)  seg.print(1234)
#
# A fourth value gives the digits of a chain, a multiple of 8.

#@ solidify:Max7219Chain
#@ solidify:Max7219
#@ requires:USE_BERRY_GPIO

class Max7219Chain
  static var DECODE_MODE = 9
  static var INTENSITY = 10
  static var SCAN_LIMIT = 11
  static var SHUTDOWN = 12
  static var DISPLAY_TEST = 15

  var din, cs, clk, devices

  def init(din, cs, clk, devices)
    import gpio
    self.din = din
    self.cs = cs
    self.clk = clk
    self.devices = devices
    gpio.pin_mode(din, gpio.OUTPUT)
    gpio.pin_mode(clk, gpio.OUTPUT)
    gpio.pin_mode(cs, gpio.OUTPUT)
    gpio.digital_write(cs, 1)
    self.fill(self.DISPLAY_TEST, 0)
    self.fill(self.SCAN_LIMIT, 7)
    self.fill(self.DECODE_MODE, 0)
    self.fill(self.SHUTDOWN, 0)
  end

  def shift(value)
    import gpio
    for position: range(7, 0, -1)
      gpio.digital_write(self.din, (value >> position) & 1)
      gpio.digital_write(self.clk, 1)
      gpio.digital_write(self.clk, 0)
    end
  end

  # Writes one register of every device; the first value is for the device nearest the pins.
  def send(register, values)
    import gpio
    gpio.digital_write(self.cs, 0)
    for device: range(self.devices - 1, 0, -1)
      self.shift(register)
      self.shift(values[device])
    end
    gpio.digital_write(self.cs, 1)
  end

  def fill(register, value)
    var values = bytes(-self.devices)
    for device: 0 .. self.devices - 1
      values[device] = value
    end
    self.send(register, values)
  end

  def set_on(on, level)
    self.fill(self.INTENSITY, level * 2 - 1)
    self.fill(self.SHUTDOWN, on ? 1 : 0)
    return true
  end
end

class Max7219 : SegmentDisplay
  var chain

  def init(din, cs, clk, digits)
    var count = digits != nil ? digits : 8
    if count < 8 || count % 8 raise "value_error", "digits must be a multiple of 8" end
    super(self).init(count)
    self.chain = Max7219Chain(din, cs, clk, count / 8)
  end

  # The chip orders its bits DP A B C D E F G.
  def wired(cell)
    var value = cell & 0x80
    for segment: 0 .. 6
      if cell & (1 << segment) value |= 0x40 >> segment end
    end
    return value
  end

  # Register 8 of the first device holds the leftmost digit.
  def write_digits(cells)
    var values = bytes(-self.chain.devices)
    for register: 1 .. 8
      for device: 0 .. self.chain.devices - 1
        values[device] = self.wired(cells[device * 8 + 8 - register])
      end
      self.chain.send(register, values)
    end
    return true
  end

  def write_control(on, level)
    return self.chain.set_on(on, level)
  end
end
