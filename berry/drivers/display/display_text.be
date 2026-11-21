# Tasmota's DisplayText command for scripts and automations written for it.
# Needs USE_DISPLAY_FONTS, USE_DISPLAY_DRAW and USE_BERRY_DRIVERS; [t] and [T] need USE_TIME.
# Upload it and add the command from autoexec.be:
#   load("display_text.be")  DisplayText().add_command()
# or build it in with the environment option `solidify` and only add the command.
#
#   DisplayText [z][x10y20][f1]Hello [Ci2]world
#
# Supported: z x y l c C Ci B Bi p s f h v L r R k K t tS T TU d dc D a o O and ~HH or ~~ in text.
# Not supported, answered with "Unknown Escape": P S G g w b F m i I u U.
#
# Differences from Tasmota: f and s select the closest embedded font, since glyphs are not scaled;
# ~HH bytes from 80 up are Latin-1 characters; a panel without frame memory always draws the text
# background, so D2 and D3 only make text transparent on monochrome panels.

#@ solidify:DisplayText
#@ requires:USE_DISPLAY_FONTS,USE_DISPLAY_DRAW,USE_BERRY_DRIVERS

class DisplayText
  static var FONTS = [[5, 8], [6, 12], [8, 16], [12, 24], [16, 32]]
  # Pixel heights of Tasmota's font numbers 0 to 7.
  static var FONT_HEIGHTS = [8, 12, 24, 8, 16, 8, 8, 8]
  static var COLORS = [0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x07FF, 0xF81F, 0xFFE0, 0x000F,
    0x03E0, 0x03EF, 0x7800, 0x780F, 0x7BE0, 0xC618, 0x7BEF, 0xFD20, 0xAFE5, 0xFC18]

  var panel, page
  var x, y, fg, bg, auto_draw, transparent, font_number, size, face, user_colors
  var data, at

  def init()
    self.panel = display()
    self.x = 0
    self.y = 0
    self.fg = 1
    self.bg = 0
    self.auto_draw = 1
    self.transparent = false
    self.font_number = 0
    self.size = 1
    self.user_colors = {}
    self.attach()
    self.select_font()
  end

  # Draws in the frame memory where the panel offers it; a rotation can take it away.
  def attach()
    var frame = self.panel.framebuffer()
    self.page = frame != nil ? pixmat(frame, self.panel.width(), self.panel.height(), 0) : nil
  end

  # Uses the largest font not taller than wanted, or else the smallest one built.
  def select_font()
    var wanted = self.FONT_HEIGHTS[self.font_number] * self.size
    var index = self.FONTS.size() - 1
    while index >= 0
      if self.FONTS[index][1] <= wanted && self.load_font(index) return end
      index -= 1
    end
    index = 0
    while !self.load_font(index) index += 1 end
  end

  def load_font(index)
    try
      var cell = self.FONTS[index]
      self.face = font(str(cell[0]) + "x" + str(cell[1]))
      return true
    except "value_error"
      return false
    end
  end

  # Tasmota's colors are RGB565, and 1 is white on monochrome panels.
  def rgb565(color)
    return color == 1 ? 0xFFFF : color
  end

  def rgb888(color)
    var c = self.rgb565(color)
    return ((c >> 11) & 31) << 19 | ((c >> 5) & 63) << 10 | (c & 31) << 3
  end

  # The pixel interface draw.line and draw.circle use.
  def set(x, y, color)
    if self.page != nil
      self.page.set(x, y, color != 0 ? 1 : 0)
    elif x >= 0 && y >= 0 && x < self.panel.width() && y < self.panel.height()
      self.panel.pixel(x, y, self.rgb888(color))
    end
  end

  def fill(x, y, width, height, color)
    if width <= 0 || height <= 0 return end
    if self.page != nil
      draw.fill_rect(self.page, x, y, width, height, color != 0 ? 1 : 0)
    else
      self.panel.fill_rect(x, y, width, height, self.rgb888(color))
    end
  end

  def fill_circle(cx, cy, radius, color)
    var dx = radius
    var dy = 0
    var error = 1 - radius
    while dx >= dy
      self.fill(cx - dx, cy + dy, 2 * dx + 1, 1, color)
      self.fill(cx - dx, cy - dy, 2 * dx + 1, 1, color)
      self.fill(cx - dy, cy + dx, 2 * dy + 1, 1, color)
      self.fill(cx - dy, cy - dx, 2 * dy + 1, 1, color)
      dy += 1
      if error < 0
        error += 2 * dy + 1
      else
        dx -= 1
        error += 2 * (dy - dx) + 1
      end
    end
  end

  def draw_text(text, x, y)
    if self.page != nil
      if self.transparent
        self.face.text(self.page, text, x, y, self.fg != 0 ? 1 : 0)
      else
        self.face.text(self.page, text, x, y, self.fg != 0 ? 1 : 0, self.bg != 0 ? 1 : 0)
      end
      return
    end
    var width = self.face.measure(text)
    if width == 0 return end
    var strip = pixmat(width, self.face.height(), 2)
    self.face.text(strip, text, 0, 0, self.rgb565(self.fg), self.rgb565(self.bg))
    self.panel.blit(strip, x, y)
  end

  def more()
    return self.at < self.data.size()
  end

  def is_digit(index)
    return index < self.data.size() && self.data[index] >= 48 && self.data[index] <= 57
  end

  # Reads a number as Tasmota's atoiv does: the value stops at the first other character, while
  # all digits and minus signs are skipped.
  def number()
    var index = self.at
    var negative = index < self.data.size() && self.data[index] == 45
    if negative index += 1 end
    var value = 0
    while self.is_digit(index)
      if value < 32768 value = value * 10 + self.data[index] - 48 end
      index += 1
    end
    while self.is_digit(self.at) || (self.more() && self.data[self.at] == 45) self.at += 1 end
    if value > 32767 value = 32767 end
    return negative ? -value : value
  end

  # Reads a number, the separator after it and a second number.
  def pair()
    var first = self.number()
    self.at += 1
    return [first, self.number()]
  end

  def next_byte()
    var value = self.more() ? self.data[self.at] : 0
    self.at += 1
    return value
  end

  def peek_is(character)
    return self.more() && self.data[self.at] == character
  end

  def indexed_color(index)
    if index >= 0 && index < self.COLORS.size() return self.COLORS[index] end
    var user = self.user_colors.find(index)
    return user != nil ? user : 0
  end

  def color()
    if self.peek_is(105)    # i: a color index
      self.at += 1
      return self.indexed_color(self.number())
    end
    var value = self.number() & 0xFFFF
    while self.more() && (self.data[self.at] == 46 || self.is_digit(self.at)) self.at += 1 end
    return value
  end

  def append(line, text)
    var added = bytes().fromstring(text)
    for index: 0 .. added.size() - 1 line.add(added[index], 1) end
  end

  def add_latin1(out, value)
    if value < 0x80
      out.add(value, 1)
    else
      out.add(0xC0 | (value >> 6), 1)
      out.add(0x80 | (value & 0x3F), 1)
    end
  end

  def hex_value(line, index)
    var value = 0
    for position: index .. index + 1
      var digit = line[position]
      if digit >= 48 && digit <= 57
        digit -= 48
      elif digit >= 65 && digit <= 70
        digit -= 55
      elif digit >= 97 && digit <= 102
        digit -= 87
      else
        break
      end
      value = value * 16 + digit
    end
    return value
  end

  # Replaces ~~ and ~HH as Tasmota's decode_te does; an incomplete ~H is kept.
  def decode_escapes(line)
    var out = bytes()
    var index = 0
    var length = line.size()
    while index < length
      var value = line[index]
      if value != 126
        out.add(value, 1)
        index += 1
      elif index + 1 < length && line[index + 1] == 126
        out.add(126, 1)
        index += 2
      elif index + 2 < length
        self.add_latin1(out, self.hex_value(line, index + 1))
        index += 3
      else
        for rest: index .. length - 1 out.add(line[rest], 1) end
        break
      end
    end
    return out
  end

  # Applies the p field width, counted in characters.
  def fit(line, fill)
    var width = fill < 0 ? -fill : fill
    var kept = bytes()
    var count = 0
    for index: 0 .. line.size() - 1
      var lead = (line[index] & 0xC0) != 0x80
      if lead count += 1 end
      if count > width break end
      kept.add(line[index], 1)
    end
    var padding = bytes()
    while count < width
      padding.add(32, 1)
      count += 1
    end
    return fill < 0 ? padding + kept : kept + padding
  end

  def flush(line, fill, column, row)
    var decoded = self.decode_escapes(line)
    if decoded.size() == 0 return end
    if fill != 0 decoded = self.fit(decoded, fill) end
    var text = decoded.asstring()
    if column > 0 && row > 0
      self.draw_text(text, (column - 1) * self.face.width(), (row - 1) * self.face.height())
    else
      self.draw_text(text, self.x, self.y)
    end
  end

  # Runs one escape; returns false for one this script does not support.
  def escape(kind, line, state)
    if kind == 122          # z: clear
      self.fill(0, 0, self.panel.width(), self.panel.height(), self.bg)
      self.x = 0
      self.y = 0
      state[0] = 0
      state[1] = 0
    elif kind == 120        # x
      self.x = self.number()
    elif kind == 121        # y
      self.y = self.number()
    elif kind == 108        # l: text line
      state[0] = self.number()
    elif kind == 99         # c: text column
      state[1] = self.number()
    elif kind == 67         # C: foreground
      self.fg = self.color()
    elif kind == 66         # B: background
      self.bg = self.color()
    elif kind == 112        # p: field width
      var fill = self.number()
      state[2] = fill > 127 ? 127 : fill < -127 ? -127 : fill
    elif kind == 115        # s: text size
      var scale = self.number()
      self.size = scale < 1 ? 1 : scale > 4 ? 4 : scale
      self.select_font()
    elif kind == 102        # f: font
      self.font_number = self.next_byte() & 7
      if self.font_number != 0 self.size = 1 end
      self.select_font()
    elif kind == 104        # h: horizontal line
      var length = self.number()
      self.fill(length < 0 ? self.x + length : self.x, self.y, length < 0 ? -length : length, 1, self.fg)
      self.x += length
    elif kind == 118        # v: vertical line
      var length = self.number()
      self.fill(self.x, length < 0 ? self.y + length : self.y, 1, length < 0 ? -length : length, self.fg)
      self.y += length
    elif kind == 76         # L: line to a point, then Tasmota adds the point to the position
      var to = self.pair()
      draw.line(self, self.x, self.y, to[0], to[1], self.fg)
      self.x += to[0]
      self.y += to[1]
    elif kind == 114        # r: rectangle
      var box = self.pair()
      if box[0] > 0 && box[1] > 0
        self.fill(self.x, self.y, box[0], 1, self.fg)
        self.fill(self.x, self.y + box[1] - 1, box[0], 1, self.fg)
        self.fill(self.x, self.y, 1, box[1], self.fg)
        self.fill(self.x + box[0] - 1, self.y, 1, box[1], self.fg)
      end
    elif kind == 82         # R: filled rectangle
      var box = self.pair()
      self.fill(self.x, self.y, box[0], box[1], self.fg)
    elif kind == 107        # k: circle
      draw.circle(self, self.x, self.y, self.number(), self.fg)
    elif kind == 75         # K: filled circle
      self.fill_circle(self.x, self.y, self.number(), self.fg)
    elif kind == 116        # t: time, tS with seconds
      var seconds = self.peek_is(83)
      if seconds self.at += 1 end
      self.append(line, newmota.strftime(seconds ? "%H:%M:%S" : "%H:%M", newmota.rtc()["local"]))
    elif kind == 84         # T: date, TU month first
      var us = self.peek_is(85)
      if us self.at += 1 end
      self.append(line, newmota.strftime(us ? "%m-%d-%y" : "%d-%m-%y", newmota.rtc()["local"]))
    elif kind == 100        # d: refresh, dc defines a color index
      if self.peek_is(99)
        self.at += 1
        var index = self.number()
        self.at += 1
        self.user_colors[index < 19 || index > 31 ? 19 : index] = self.color()
      else
        self.panel.refresh()
      end
    elif kind == 68         # D: bit 0 refreshes after each command, bit 1 is transparent text
      self.auto_draw = self.next_byte() & 3
      self.transparent = (self.auto_draw & 2) != 0
    elif kind == 97         # a: rotation
      self.panel.rotation(self.next_byte() & 3)
      self.attach()
    elif kind == 111        # o: display off
      self.panel.power(false)
    elif kind == 79         # O: display on
      self.panel.power(true)
    else
      return false
    end
    return true
  end

  # Adds the DisplayText command, served by this instance.
  def add_command()
    var target = self
    newmota.add_cmd("DisplayText", / name, index, payload, parsed -> target.command(payload))
  end

  def command(payload)
    if payload == "" return end
    self.data = bytes().fromstring(payload)
    self.at = 0
    var line = bytes()
    var state = [0, 0, 0]    # text line, text column, field width
    var in_escape = false
    var known = true
    while known && self.more()
      var value = self.next_byte()
      if in_escape
        if value == 93
          in_escape = false
        else
          known = self.escape(value, line, state)
        end
      elif value == 91
        in_escape = true
        self.flush(line, state[2], state[1], state[0])
        line = bytes()
      elif line.size() < 255
        line.add(value, 1)
      else
        break
      end
    end
    self.flush(line, state[2], state[1], state[0])
    if self.auto_draw & 1 self.panel.refresh() end
    newmota.resp_cmnd_str(known ? payload : "Unknown Escape")
  end
end
