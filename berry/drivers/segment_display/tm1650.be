# Four-digit TM1650 on I2C, adapted from Tasmota's xdsp_20_tm1650.ino, source pin
# 1e03a6d9a579344a82b3b254461467409c0e2140.
# Copyright (C) 2021 Stefan Oskamp, Theo Arends, Anatoli Arkhipenko; (C) 2023 Gabriele Lauricella.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# autoexec.be:  load("segment_display.be")  load("tm1650.be")  load("app.be")
# app.be:       var tm = Tm1650()  tm.set_on(4)  tm.print(1234)
#
# Tm1650(Tm1650.WIFI_303) selects the 303WifiLC01 wiring, a second value the bus, such as wire2.

#@ solidify:Tm1650
#@ requires:USE_I2C

class Tm1650 : SegmentDisplay
  static var XY_CLOCK = 0
  static var WIFI_303 = 2
  static var CONTROL_BASE = 0x24
  static var DISPLAY_BASE = 0x34

  var board, bus

  def init(board, bus)
    super(self).init(4)
    self.board = board != nil ? board : self.XY_CLOCK
    self.bus = bus != nil ? bus : newmota.wire_scan(self.CONTROL_BASE)
    if self.bus == nil raise "io_error", "TM1650 not found" end
  end

  def put(address, value)
    self.bus._begin_transmission(address)
    self.bus._write(value)
    return self.bus._end_transmission() == 0
  end

  # The 303WifiLC01 connects its segments as B F A E D C G P from bit 7 down.
  def wired(n)
    if self.board != self.WIFI_303 return n end
    return (n & 0x1C) | ((n & 0x01) << 5) | ((n & 0x02) << 6) | ((n & 0x20) << 1) |
      ((n & 0x40) >> 5) | ((n & 0x80) >> 7)
  end

  def write_digits(cells)
    var done = true
    for i: 0 .. 3
      done = self.put(self.DISPLAY_BASE + i, self.wired(cells[i])) && done
    end
    return done
  end

  # Brightness field 0 is the chip's brightest level.
  def write_control(on, level)
    var done = true
    for i: 0 .. 3
      done = self.put(self.CONTROL_BASE + i, ((level % 8) << 4) | (on ? 1 : 0)) && done
    end
    return done
  end

  # On the XY-Clock the dots of the last two digits are the halves of the colon.
  def clock_text(hour, minute, dot)
    if self.board != self.XY_CLOCK return super(self).clock_text(hour, minute, dot) end
    var mark = dot ? "." : ""
    return format("%02d%d%s%d%s", hour, minute / 10, mark, minute % 10, mark)
  end
end
