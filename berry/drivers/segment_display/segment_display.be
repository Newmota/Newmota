# Seven-segment text, numbers, scrolling and a clock, shared by the drivers in this folder.
# Segment font from Tasmota's xdsp_20_tm1650.ino, source pin 1e03a6d9a579344a82b3b254461467409c0e2140.
# Copyright (C) 2021 Stefan Oskamp, Theo Arends, Anatoli Arkhipenko; (C) 2023 Gabriele Lauricella.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Load it before the driver. GPIOs given to a driver must stay unassigned in the configuration.
#
#   set_on(0..8)  print(value)  raw(bytes[, position])  clear()  add_commands()
#   scroll(text[, loops])  clock(0 off, 1 12h, 2 24h), with USE_BERRY_DRIVERS and USE_TIME

#@ solidify:SegmentDisplay

class SegmentDisplay
  # Tasmota's segment font for the characters 0x20 to 0x7F.
  static var FONT = bytes("0082210000000002390F000000408000" +
    "3F065B4F666D7D077F6F000000480053" + "00777C395E79716F76061E003800543F" +
    "7367506D783E0000006E0039000F6308" + "635F7C585E7B716F74021E000600545C" +
    "7367506D781C0000006E0039300F0000")

  var digits, cells, level
  var scrolled, scroll_at, scroll_loops, scroll_done
  var clock_mode, attached

  def init(digits)
    self.digits = digits
    self.cells = bytes(-digits)
    self.level = 8
    self.clock_mode = 0
    self.attached = false
  end

  # Turns the display on at 1 to 8, or off at 0; the level is kept while off.
  def set_on(brightness)
    if brightness == nil || brightness > 8
      brightness = 8
    elif brightness < 0
      brightness = 0
    end
    if brightness self.level = brightness end
    return self.write_control(brightness > 0, self.level)
  end

  # Returns the segments of a text; a dot joins the character before it.
  def segments(text)
    import string
    var result = bytes()
    for i: 0 .. size(text) - 1
      var code = string.byte(text[i])
      if text[i] == "." && i > 0 && text[i - 1] != "."
        result[result.size() - 1] |= 0x80
      else
        result.add(code >= 0x20 && code < 0x80 ? self.FONT[code - 0x20] : 0, 1)
      end
    end
    return result
  end

  def show(cells)
    self.cells = cells
    return self.write_digits(cells)
  end

  # Shows segments right aligned and keeps the leftmost ones that fit.
  def show_right(segments)
    var count = segments.size() < self.digits ? segments.size() : self.digits
    var cells = bytes(-self.digits)
    for i: 0 .. count - 1
      cells[self.digits - count + i] = segments[i]
    end
    return self.show(cells)
  end

  def print(value)
    self.stop()
    return self.show_right(self.segments(str(value)))
  end

  def raw(data, position)
    self.stop()
    var cells = self.cells.copy()
    var at = position != nil ? position : 0
    for i: 0 .. data.size() - 1
      if at + i >= 0 && at + i < self.digits cells[at + i] = data[i] end
    end
    return self.show(cells)
  end

  def clear()
    self.stop()
    return self.show(bytes(-self.digits))
  end

  def scroll(text, loops)
    self.stop()
    self.scrolled = bytes(-self.digits) + self.segments(str(text))
    self.scroll_at = 0
    self.scroll_loops = loops != nil ? loops : 0
    self.scroll_done = 0
    self.attach()
    self.every_250ms()
  end

  def clock(mode)
    self.stop()
    self.clock_mode = mode
    if mode
      self.attach()
      self.every_second()
    end
  end

  def attach()
    if !self.attached
      newmota.add_driver(self)
      self.attached = true
    end
  end

  # Ends scrolling and the clock.
  def stop()
    self.scrolled = nil
    self.clock_mode = 0
    if self.attached
      newmota.remove_driver(self)
      self.attached = false
    end
  end

  def every_250ms()
    if self.scrolled == nil return end
    if self.scroll_at > self.scrolled.size()
      self.scroll_at = 0
      self.scroll_done += 1
      if self.scroll_loops && self.scroll_done >= self.scroll_loops
        self.stop()
        return
      end
    end
    var cells = bytes(-self.digits)
    for i: 0 .. self.digits - 1
      var at = self.scroll_at + i
      if at < self.scrolled.size() cells[i] = self.scrolled[at] end
    end
    self.show(cells)
    self.scroll_at += 1
  end

  # The default places the blinking dot behind the hours, where modules have their colon.
  def clock_text(hour, minute, dot)
    return format("%02d%s%02d", hour, dot ? "." : "", minute)
  end

  def every_second()
    if !self.clock_mode return end
    var now = newmota.rtc()
    if !now["valid"]
      self.show_right(self.segments("--.--"))
      return
    end
    var hour = int(newmota.strftime("%H", now["local"]))
    if self.clock_mode == 1
      hour = hour % 12
      if hour == 0 hour = 12 end
    end
    var minute = int(newmota.strftime("%M", now["local"]))
    var dot = int(newmota.strftime("%S", now["local"])) % 2 == 0
    self.show_right(self.segments(self.clock_text(hour, minute, dot)))
  end

  def add_commands()
    var target = self
    def respond(done)
      if done
        newmota.resp_cmnd_done()
      else
        newmota.resp_cmnd_failed()
      end
    end
    newmota.add_cmd("TmPrint", / name, index, payload -> respond(target.print(payload)))
    newmota.add_cmd("TmBrightness", / name, index, payload -> respond(target.set_on(int(payload))))
    newmota.add_cmd("TmRaw", / name, index, payload -> respond(target.raw(bytes(payload))))
    newmota.add_cmd("TmClear", / name, index, payload -> respond(target.clear()))
    newmota.add_cmd("TmScroll", def (name, index, payload)
      target.scroll(payload, index > 1 ? index : 0)
      respond(true)
    end)
    newmota.add_cmd("TmClock", def (name, index, payload)
      target.clock(int(payload))
      respond(true)
    end)
  end
end
