# TM1640 on two free GPIOs, adapted from Tasmota's xdsp_13_tm1640.ino, source pin
# 1e03a6d9a579344a82b3b254461467409c0e2140. Copyright (C) 2024 Stefan Oskamp.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# autoexec.be:  load("segment_display.be")  load("tm1640.be")  load("app.be")
# app.be:       var tm = Tm1640(DIN_PIN, CLK_PIN, 8)  tm.set_on(4)  tm.print(1234)

#@ solidify:Tm1640
#@ requires:USE_BERRY_GPIO

class Tm1640 : SegmentDisplay
  static var CMD_DATA_AUTO = 0x40
  static var CMD_DISPLAY = 0x80
  static var CMD_ADDRESS = 0xC0

  var din, clk

  def init(din, clk, digits)
    import gpio
    super(self).init(digits != nil ? digits : 16)
    self.din = din
    self.clk = clk
    gpio.pin_mode(din, gpio.OUTPUT)
    gpio.pin_mode(clk, gpio.OUTPUT)
    gpio.digital_write(clk, 1)
    gpio.digital_write(din, 1)
  end

  def start()
    import gpio
    gpio.digital_write(self.din, 0)
    gpio.digital_write(self.clk, 0)
  end

  def stop_bus()
    import gpio
    gpio.digital_write(self.clk, 1)
    gpio.digital_write(self.din, 1)
  end

  def send(value)
    import gpio
    for position: 0 .. 7
      gpio.digital_write(self.din, (value >> position) & 1)
      gpio.digital_write(self.clk, 1)
      gpio.digital_write(self.clk, 0)
    end
    gpio.digital_write(self.din, 0)
  end

  def command(value)
    self.start()
    self.send(value)
    self.stop_bus()
  end

  def write_digits(cells)
    self.command(self.CMD_DATA_AUTO)
    self.start()
    self.send(self.CMD_ADDRESS)
    for i: 0 .. self.digits - 1
      self.send(cells[i])
    end
    self.stop_bus()
    return true
  end

  def write_control(on, level)
    self.command(on ? self.CMD_DISPLAY | 0x08 | (level - 1) : self.CMD_DISPLAY)
    return true
  end
end
