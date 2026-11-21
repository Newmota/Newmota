# A sensor page for a small display, in place of Tasmota's DisplayMode 2. It shows the numbers of
# the same document as STATE and SENSOR, one per line, refreshed every second.
# Needs USE_DISPLAY_FONTS, USE_DISPLAY_DRAW and USE_BERRY_DRIVERS.
# Upload it, then run it from autoexec.be with: load("display_sensors.be")

import json
import string

class DisplaySensors
  var panel, page, strip, text_font, rows

  def init()
    self.panel = display()
    self.text_font = font("6x12")
    var width = self.panel.width()
    var height = self.text_font.height()
    self.rows = self.panel.height() / height
    var frame = self.panel.framebuffer()
    if frame != nil
      # A monochrome panel is drawn directly in its own memory.
      self.page = pixmat(frame, width, self.panel.height(), 0)
    else
      # Other panels get one line at a time, which needs little memory on large screens.
      self.strip = pixmat(width, height, 0)
    end
  end

  # Returns [label, value] pairs for every number, one level inside each sensor.
  def readings()
    var raw = newmota.read_sensors()
    var document = raw != nil ? json.load(raw) : nil
    var found = []
    if document == nil return found end
    for sensor: document.keys()
      var fields = document[sensor]
      if classname(fields) == "map"
        for field: fields.keys()
          var value = fields[field]
          if type(value) == "real"
            found.push([field, string.format("%.1f", value)])
          elif type(value) == "int"
            found.push([field, str(value)])
          end
        end
      end
    end
    return found
  end

  def show_row(row, label, value)
    var width = self.panel.width()
    var height = self.text_font.height()
    var target = self.page != nil ? self.page : self.strip
    var y = self.page != nil ? row * height : 0
    draw.text(target, self.text_font, label, 0, y, width, 1, 0, draw.LEFT, 1)
    draw.text(target, self.text_font, value, 0, y, width, 1, nil, draw.RIGHT, 1)
    if self.page == nil self.panel.blit(target, 0, row * height) end
  end

  def every_second()
    var lines = self.readings()
    if lines.size() == 0 lines = [["No sensors", ""]] end
    for row: 0 .. self.rows - 1
      var line = row < lines.size() ? lines[row] : ["", ""]
      self.show_row(row, line[0], line[1])
    end
    self.panel.refresh()
  end
end

newmota.add_driver(DisplaySensors())
