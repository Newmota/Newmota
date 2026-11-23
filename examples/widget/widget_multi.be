#################################################################################
# Multi Graph Widget Demo - 8 different graph types
#
# Port of berry-examples/dashboard/multi_widget.be to the Newmota `widget` module.
# Each widget gets its markup once; every 100 ms one of them receives new data.
# The DSL strings of the original become data-chart elements with the same colours.
# A self-rescheduling timer replaces the Tasmota driver tick.
# math.rand() replaces crypto.random(), which Newmota offers only with qualified entropy.
#################################################################################
class MULTI_GRAPH_WIDGET
  var data_buffers
  var update_index
  var gauge_value
  var gauge_dir

  static BUFFER_SIZE = 32
  static NUM_BUFFERS = 3
  static GAUGE = 'data-chart="gauge" data-v="v" data-min="0" data-max="100" data-unit="%"'
                 ' data-stops="0:#00c800,70:#c8a000,90:#c80000"'

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

    self.create_widgets()
    self.every_100ms()
  end

  # Markup once: a title and one chart bound to the data key "v"
  def create_widgets()
    import widget
    var charts = [
      ["Line", 'data-chart="line" data-v="v" data-color="#ff6464"'],
      ["Line+Grid", 'data-chart="line" data-v="v" data-color="#64ff64" data-grid'],
      ["Histogram", 'data-chart="hist" data-v="v" data-color="#6464ff"'],
      ["Hist+Grid", 'data-chart="hist" data-v="v" data-color="#ffff64" data-grid'],
      ["Multi-Line", 'data-chart="line" data-v="v" data-color="#ff64ff,#64ffff"'],
      ["Multi+Grid", 'data-chart="line" data-v="v" data-color="#ff9632,#3296ff" data-grid'],
      ["Gauge", self.GAUGE],
      ["Gauge+Value", self.GAUGE]]
    for i: 0..7
      var extra = i == 7 ? '<p class="label">Value <b data-v="v"></b> %</p>' : ''
      widget.set(format("graph%d_widget", i + 1),
        format('<div class="box w1 h1"><h3>%s</h3><div %s></div>%s</div>', charts[i][0], charts[i][1], extra))
    end
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
    self.update_data(widget_num % 3)
    var value
    if widget_num <= 2
      value = self.data_buffers[0]
    elif widget_num <= 4
      value = self.data_buffers[1]
    elif widget_num <= 6
      value = [self.data_buffers[0], self.data_buffers[2]]
    else
      value = self.gauge_value
    end
    widget.data(format("graph%d_widget", widget_num), {"v": value})
    self.update_index += 1
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
