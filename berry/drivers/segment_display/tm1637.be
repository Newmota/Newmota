# TM1637 on two free GPIOs, adapted from Tasmota's tasmota/berry/drivers/tm1637.be and
# xdsp_15_tm1637.ino (C) 2021 Ajith Vasudevan, source pin 1e03a6d9a579344a82b3b254461467409c0e2140.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# autoexec.be:  load("segment_display.be")  load("tm1637.be")  load("app.be")
# app.be:       var tm = Tm1637(DIO_PIN, CLK_PIN)  tm.set_on(4)  tm.print(1234)
#
# Six digits: Tm1637(DIO_PIN, CLK_PIN, 6[, Tm1637.SIX_DIGIT_ORDER])

#@ solidify:Tm1637
#@ requires:USE_BERRY_GPIO

class Tm1637 : SegmentDisplay
  static var CMD_CTRL = 0x80
  static var CMD_DISP_ON = 0x08
  static var CMD_DATA = 0x40
  static var CMD_ADDR = 0xC0
  static var SIX_DIGIT_ORDER = [2, 1, 0, 5, 4, 3]

  var dio, clk, order

  def init(dio, clk, digits, order)
    import gpio
    super(self).init(digits != nil ? digits : 4)
    self.dio = dio
    self.clk = clk
    self.order = order
    gpio.pin_mode(dio, gpio.OUTPUT)
    gpio.pin_mode(clk, gpio.OUTPUT)
    gpio.digital_write(clk, 1)
    gpio.digital_write(dio, 1)
  end

  def start()
    import gpio
    gpio.digital_write(self.dio, 1)
    gpio.digital_write(self.clk, 1)
    gpio.digital_write(self.dio, 0)
  end

  def stop_bus()
    import gpio
    gpio.digital_write(self.clk, 0)
    gpio.digital_write(self.dio, 0)
    gpio.digital_write(self.clk, 1)
    gpio.digital_write(self.dio, 1)
  end

  def ack()
    import gpio
    gpio.digital_write(self.clk, 0)
    gpio.pin_mode(self.dio, gpio.INPUT_PULLUP)
    var acknowledged = gpio.digital_read(self.dio) == 0
    gpio.digital_write(self.clk, 1)
    gpio.digital_write(self.clk, 0)
    gpio.pin_mode(self.dio, gpio.OUTPUT)
    return acknowledged
  end

  def write_byte(value)
    import gpio
    for position: 0 .. 7
      gpio.digital_write(self.clk, 0)
      gpio.digital_write(self.dio, (value >> position) & 1)
      gpio.digital_write(self.clk, 1)
    end
  end

  def send_command(command)
    self.start()
    self.write_byte(command)
    var acknowledged = self.ack()
    self.stop_bus()
    return acknowledged
  end

  def send_data(data)
    var acknowledged = true
    self.start()
    for i: 0 .. data.size() - 1
      self.write_byte(data[i])
      acknowledged = self.ack() && acknowledged
    end
    self.stop_bus()
    return acknowledged
  end

  def write_digits(cells)
    var payload = bytes(-(self.digits + 1))
    payload[0] = self.CMD_ADDR
    for i: 0 .. self.digits - 1
      payload[(self.order != nil ? self.order[i] : i) + 1] = cells[i]
    end
    return self.send_command(self.CMD_DATA) && self.send_data(payload)
  end

  def write_control(on, level)
    return self.send_command(on ? self.CMD_CTRL | self.CMD_DISP_ON | (level - 1) : self.CMD_CTRL)
  end
end
