#################################################################################
# Demo Graph Widget - Live data stream
#
# Port of berry-examples/dashboard/linegraph_scroll.be to the Newmota `widget` module.
# Only the API differs: the legacy widget fetched new data from its own script every
# 250 ms while the dashboard was open; here a 250 ms timer updates the widget while
# widget.viewers() reports an open dashboard, and the page redraws it.
# math.rand() replaces crypto.random(), which Newmota offers only with qualified entropy.
#################################################################################
class DEMO_GRAPH_WIDGET
  var data_buffer
  static BUFFER_SIZE = 64

  def init()
    self.data_buffer = []
    # Initialize with some random data
    for i: 0..(self.BUFFER_SIZE-1)
      self.data_buffer.push(0)
    end
    self.send_widget()
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
    self.send_widget(current, format("%.1f", avg), max_val)
  end

  def send_widget(current, avg, max_val)
    import widget
    widget.set("demo_graph", {"title": "Live Data Stream", "w": 2, "h": 2, "parts": [
      {"line": [{"values": self.data_buffer, "color": 0x00FF80}], "grid": true},
      {"value": current != nil ? current : "--", "label": "Current"},
      {"value": avg != nil ? avg : "--", "label": "Average"},
      {"value": max_val != nil ? max_val : "--", "label": "Peak"}]})
  end

  def stop()
    import widget
    newmota.remove_timer("demo_graph")
    widget.remove("demo_graph")
  end
end

demo_widget = DEMO_GRAPH_WIDGET()
