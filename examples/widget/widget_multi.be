#################################################################################
# Multi Graph Widget Demo - 8 different graph types
#
# Port of berry-examples/dashboard/multi_widget.be to the Newmota `widget` module.
# Only the API differs: widgets are described as data instead of HTML/DSL strings,
# and a self-rescheduling timer replaces the Tasmota driver tick.
# math.rand() replaces crypto.random(), which Newmota offers only with qualified entropy.
#################################################################################
class MULTI_GRAPH_WIDGET
  var data_buffers
  var update_index
  var gauge_value
  var gauge_dir

  static BUFFER_SIZE = 32
  static NUM_BUFFERS = 3

  def init()
    self.update_index = 0
    self.data_buffers = []
    self.gauge_value = 0
    self.gauge_dir = 1   # 1 = increasing, -1 = decreasing

    # Initialize multiple data buffers
    for buf_idx: 0..(self.NUM_BUFFERS-1)
      var buffer = []
      for i: 0..(self.BUFFER_SIZE-1)
        buffer.push(0)
      end
      self.data_buffers.push(buffer)
    end

    self.every_100ms()
  end

  def every_100ms()
    # update gauge value each tick
    self.gauge_value += self.gauge_dir * 2   # step size = 2
    if self.gauge_value >= 100
      self.gauge_value = 100
      self.gauge_dir = -1
    elif self.gauge_value <= 0
      self.gauge_value = 0
      self.gauge_dir = 1
    end

    self.update_next_widget()
    newmota.set_timer(100, /-> self.every_100ms(), "multi_widget")
  end

  def update_data(idx)
    import math
    var buffer = self.data_buffers[idx]
    buffer.remove(0)
    buffer.push(math.rand() % 256)
  end

  def update_next_widget()
    import widget
    var widget_num = (self.update_index % 8) + 1
    widget.set(format("graph%d_widget", widget_num), self.get_widget_content(widget_num))
    self.update_index += 1
  end

  def get_widget_content(widget_num)
    var part
    var title = ""
    self.update_data(widget_num%3)

    if widget_num == 1
      title = "Line"
      part = self.build_single_series(false, 0xFF6464, 0)
    elif widget_num == 2
      title = "Line+Grid"
      part = self.build_single_series(true, 0x64FF64, 0)
    elif widget_num == 3
      title = "Histogram"
      part = {"hist": self.data_buffers[1], "color": 0x6464FF}
    elif widget_num == 4
      title = "Hist+Grid"
      part = {"hist": self.data_buffers[1], "color": 0xFFFF64, "grid": true}
    elif widget_num == 5
      title = "Multi-Line"
      part = self.build_multi_series(false, 0xFF64FF, 0x64FFFF, 0, 2)
    elif widget_num == 6
      title = "Multi+Grid"
      part = self.build_multi_series(true, 0xFF9632, 0x3296FF, 0, 2)
    elif widget_num == 7
      title = "Gauge"
      part = self.build_gauge()
    elif widget_num == 8
      title = "Gauge+Value"
      part = self.build_gauge()
    end

    return {"title": title, "w": 1, "h": 1, "parts": [part]}
  end

  def build_single_series(grid, color, buffer_idx)
    return {"line": [{"values": self.data_buffers[buffer_idx], "color": color}], "grid": grid}
  end

  def build_multi_series(grid, color1, color2, buffer1_idx, buffer2_idx)
    return {"line": [{"values": self.data_buffers[buffer1_idx], "color": color1},
                     {"values": self.data_buffers[buffer2_idx], "color": color2}], "grid": grid}
  end

  def build_gauge()
    return {"gauge": self.gauge_value, "min": 0, "max": 100, "unit": "%",
            "stops": [[0, 0x00C800], [70, 0xC8A000], [90, 0xC80000]]}
  end

  def stop()
    import widget
    newmota.remove_timer("multi_widget")
    for widget_num: 1..8
      widget.remove(format("graph%d_widget", widget_num))
    end
  end
end

multi_widget = MULTI_GRAPH_WIDGET()
