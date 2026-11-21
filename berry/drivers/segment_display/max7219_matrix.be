# Chained MAX7219 8x8 LED matrices, adapted from Tasmota's xdsp_19_max7219_matrix.ino and LedMatrix,
# source pin 1e03a6d9a579344a82b3b254461467409c0e2140. Copyright (C) 2021 Michael Beuss.
# SPDX-License-Identifier: GPL-3.0-or-later
# Needs USE_BERRY_PIXMAT.
#
# autoexec.be:  load("segment_display.be")  load("max7219.be")  load("max7219_matrix.be")
#               load("app.be")
# app.be:       var led = Max7219Matrix(DIN_PIN, CS_PIN, CLK_PIN, 4, 1)  led.set_on(2)
#               led.face = font("5x8")  led.text("Hello")
#
# Further values: modules across, modules down, Tasmota's DisplayRotate 0..3 (default 2).
# Draw into led.page and call led.show(), or set face for text() and clock(). A frame takes about
# 400 GPIO calls per module.

#@ solidify:Max7219Matrix
#@ requires:USE_BERRY_PIXMAT

class Max7219Matrix
  static var SCROLL_GAP = "   "

  var chain, columns, rows, orientation, face, page, level
  var scrolled, scroll_at, scroll_span, clock_mode, attached

  def init(din, cs, clk, columns, rows, orientation)
    self.columns = columns != nil ? columns : 4
    self.rows = rows != nil ? rows : 1
    self.orientation = orientation != nil ? orientation : 2
    self.level = 8
    self.clock_mode = 0
    self.attached = false
    self.page = pixmat(self.columns * 8, self.rows * 8, 0)
    self.chain = Max7219Chain(din, cs, clk, self.columns * self.rows)
    self.show()
  end

  def width() return self.columns * 8 end
  def height() return self.rows * 8 end

  def set_on(brightness)
    if brightness == nil || brightness > 8
      brightness = 8
    elif brightness < 0
      brightness = 0
    end
    if brightness self.level = brightness end
    return self.chain.set_on(brightness > 0, self.level)
  end

  # Sends the page. Orientations 2 and 3 count devices from the far end of the chain, and
  # orientations 0 and 3 mirror each module, as in Tasmota's LedMatrix.
  def show()
    var pixels = self.page.buffer()
    var devices = self.chain.devices
    var stride = self.width()
    var from_end = self.orientation >= 2
    var mirrored = self.orientation == 0 || self.orientation == 3
    var values = bytes(-devices)
    for line: 0 .. 7
      var mask = 1 << (mirrored ? 7 - line : line)
      for device: 0 .. devices - 1
        var unit = from_end ? devices - 1 - device : device
        var base = (unit % self.columns) * 8 + (unit / self.columns) * stride
        var value = 0
        for bit: 0 .. 7
          if pixels[base + (mirrored ? bit : 7 - bit)] & mask value |= 1 << bit end
        end
        values[device] = value
      end
      self.chain.send(line + 1, values)
    end
  end

  def font_face()
    if self.face == nil raise "value_error", "set face to a font first" end
    return self.face
  end

  def draw_text(text, x)
    var face = self.font_face()
    face.text(self.page, text, x, (self.height() - face.height()) / 2, 1)
  end

  def centered(text)
    self.page.clear()
    self.draw_text(text, (self.width() - self.font_face().measure(text)) / 2)
    self.show()
  end

  def clear()
    self.stop()
    self.page.clear()
    self.show()
  end

  # Centers a text that fits and scrolls a longer one until something else is shown.
  def text(value)
    self.stop()
    var label = str(value)
    var face = self.font_face()
    if face.measure(label) <= self.width()
      self.centered(label)
      return
    end
    self.scrolled = label
    self.scroll_at = 0
    self.scroll_span = face.measure(label + self.SCROLL_GAP)
    self.attach()
    self.every_100ms()
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

  def stop()
    self.scrolled = nil
    self.clock_mode = 0
    if self.attached
      newmota.remove_driver(self)
      self.attached = false
    end
  end

  def every_100ms()
    if self.scrolled == nil return end
    self.page.clear()
    self.draw_text(self.scrolled, -self.scroll_at)
    # The start of the text follows its end once the gap comes into view.
    if self.scroll_span - self.scroll_at < self.width()
      self.draw_text(self.scrolled, self.scroll_span - self.scroll_at)
    end
    self.show()
    self.scroll_at = (self.scroll_at + 1) % self.scroll_span
  end

  def every_second()
    if !self.clock_mode return end
    var now = newmota.rtc()
    if !now["valid"]
      self.centered("--:--")
      return
    end
    var hour = int(newmota.strftime("%H", now["local"]))
    if self.clock_mode == 1
      hour = hour % 12
      if hour == 0 hour = 12 end
    end
    var minute = int(newmota.strftime("%M", now["local"]))
    var mark = int(newmota.strftime("%S", now["local"])) % 2 == 0 ? ":" : " "
    self.centered(format("%02d%s%02d", hour, mark, minute))
  end
end
