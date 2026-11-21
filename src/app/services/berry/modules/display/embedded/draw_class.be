# Solidified as the global draw class: shapes and aligned text for a pixmat. Every method takes
# the matrix first and clips to it; color is what pixmat.set accepts for that matrix.

#@ solidify:draw

class draw
  static var LEFT = 0
  static var CENTER = 1
  static var RIGHT = 2

  static def line(pm, x0, y0, x1, y1, color)
    var dx = x1 - x0
    var dy = y1 - y0
    var sx = dx < 0 ? -1 : 1
    var sy = dy < 0 ? -1 : 1
    if dx < 0 dx = -dx end
    if dy > 0 dy = -dy end
    var error = dx + dy
    while true
      pm.set(x0, y0, color)
      if x0 == x1 && y0 == y1 break end
      var twice = 2 * error
      if twice >= dy
        error += dy
        x0 += sx
      end
      if twice <= dx
        error += dx
        y0 += sy
      end
    end
  end

  static def fill_rect(pm, x, y, width, height, color)
    var right = x + width
    var bottom = y + height
    if right > pm.width() right = pm.width() end
    if bottom > pm.height() bottom = pm.height() end
    if x < 0 x = 0 end
    if y < 0 y = 0 end
    while y < bottom
      var column = x
      while column < right
        pm.set(column, y, color)
        column += 1
      end
      y += 1
    end
  end

  static def rect(pm, x, y, width, height, color)
    if width <= 0 || height <= 0 return end
    _class.fill_rect(pm, x, y, width, 1, color)
    _class.fill_rect(pm, x, y + height - 1, width, 1, color)
    _class.fill_rect(pm, x, y, 1, height, color)
    _class.fill_rect(pm, x + width - 1, y, 1, height, color)
  end

  static def circle(pm, cx, cy, radius, color)
    if radius < 0 return end
    var x = radius
    var y = 0
    var error = 1 - radius
    while x >= y
      pm.set(cx + x, cy + y, color)
      pm.set(cx - x, cy + y, color)
      pm.set(cx + x, cy - y, color)
      pm.set(cx - x, cy - y, color)
      pm.set(cx + y, cy + x, color)
      pm.set(cx - y, cy + x, color)
      pm.set(cx + y, cy - x, color)
      pm.set(cx - y, cy - x, color)
      y += 1
      if error < 0
        error += 2 * y + 1
      else
        x -= 1
        error += 2 * (y - x) + 1
      end
    end
  end

  # Draws text in a field one font height tall. A background clears the whole field first, so a
  # shorter value leaves nothing of the previous one. Text wider than the field is not cut.
  static def text(pm, font, label, x, y, width, color, background, align, padding)
    if padding == nil padding = 0 end
    if background != nil _class.fill_rect(pm, x, y, width, font.height(), background) end
    var at = x + padding
    var room = width - 2 * padding - font.measure(label)
    if align == _class.CENTER
      at += room / 2
    elif align == _class.RIGHT
      at += room
    end
    font.text(pm, label, at, y, color)
  end
end
