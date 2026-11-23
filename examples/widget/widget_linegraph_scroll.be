#################################################################################
# Demo Graph Widget - Live data stream
#
# Port of berry-examples/dashboard/linegraph_scroll.be to the Newmota `widget` module.
# The legacy widget fetched new data from its own script every 250 ms; here the
# markup is set once and a 250 ms timer sends only the data while widget.viewers()
# reports an open dashboard. The page draws it; the widget needs no JS of its own.
# math.rand() replaces crypto.random(), which Newmota offers only with qualified entropy.
#################################################################################
class DEMO_GRAPH_WIDGET
  var data_buffer
  static BUFFER_SIZE = 64

  def init()
    import widget
    self.data_buffer = []
    # Initialize with some random data
    for i: 0..(self.BUFFER_SIZE-1)
      self.data_buffer.push(0)
    end
    widget.set("demo_graph", '<div class="box w2 h2"><h3>Live Data Stream</h3>'
      '<div data-chart="line" data-v="graph" data-color="#00ff80" data-grid data-height="160"></div>'
      '<div class="row">'
        '<p><span class="label">Current</span><br><span class="value" data-v="current">--</span></p>'
        '<p><span class="label">Average</span><br><span class="value" data-v="avg">--</span></p>'
        '<p><span class="label">Peak</span><br><span class="value" data-v="max">--</span></p>'
      '</div></div>')
    self.tick()
  end

  def tick()
    import widget
    if widget.viewers()
      self.send_data()
    end
    newmota.set_timer(250, /-> self.tick(), "demo_graph")
  end

  def update_data()
    import math
    # Remove oldest value and add new random value
    self.data_buffer.remove(0)
    self.data_buffer.push(math.rand() % 256)
  end

  def send_data()
    import widget
    self.update_data()
    var current = self.data_buffer[size(self.data_buffer)-1]
    var sum = 0
    var max_val = 0
    for val: self.data_buffer
      sum += val
      if val > max_val
        max_val = val
      end
    end
    var avg = real(sum) / real(size(self.data_buffer))
    widget.data("demo_graph", {"graph": self.data_buffer, "current": current,
                               "avg": format("%.1f", avg), "max": max_val})
  end

  def stop()
    import widget
    newmota.remove_timer("demo_graph")
    widget.remove("demo_graph")
  end
end

demo_widget = DEMO_GRAPH_WIDGET()
