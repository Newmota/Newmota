# A clock page for a small display, in place of Tasmota's DisplayMode 1.
# Needs USE_DISPLAY_FONTS, USE_DISPLAY_DRAW, USE_BERRY_DRIVERS and USE_TIME.
# Upload it, then run it from autoexec.be with: load("display_clock.be")

class DisplayClock
  var panel, page, large, small, top, base, direct, shown

  # Returns the largest available font that fits this many characters across the panel.
  def fitting(characters)
    for name: ["16x32", "12x24", "8x16", "6x12"]
      try
        var candidate = font(name)
        if candidate.width() * characters <= self.panel.width() return candidate end
      except "value_error"
      end
    end
    return font("6x12")
  end

  def init()
    self.panel = display()
    self.large = self.fitting(8)
    self.small = font("6x12")
    var width = self.panel.width()
    var height = self.large.height() + 2 + self.small.height()
    self.top = (self.panel.height() - height) / 2
    var frame = self.panel.framebuffer()
    self.direct = frame != nil
    if self.direct
      # A monochrome panel is drawn directly in its own memory.
      self.page = pixmat(frame, width, self.panel.height(), 0)
      self.base = self.top
    else
      # Other panels get a one-bit strip that is copied after drawing.
      self.page = pixmat(width, height, 0)
      self.base = 0
    end
  end

  def every_second()
    var now = newmota.rtc()
    var time = "--:--:--"
    var date = ""
    if now["valid"]
      time = newmota.strftime("%H:%M:%S", now["local"])
      date = newmota.strftime("%Y-%m-%d", now["local"])
    end
    if time == self.shown return end
    self.shown = time
    var width = self.panel.width()
    draw.text(self.page, self.large, time, 0, self.base, width, 1, 0, draw.CENTER)
    var row = self.base + self.large.height() + 2
    draw.text(self.page, self.small, date, 0, row, width, 1, 0, draw.CENTER)
    if !self.direct self.panel.blit(self.page, 0, self.top) end
    self.panel.refresh()
  end
end

newmota.add_driver(DisplayClock())
