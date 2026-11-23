#################################################################################
# Air Conditioner Controller Dashboard - Simple Version
#
# Port of berry-examples/dashboard/ac_mockup.be to the Newmota `widget` module.
# Each widget gets its markup once and then only new data; DSL strings become
# data-chart elements. The mode widget drops the original's solid colour bar and colours
# its value instead. A self-rescheduling timer replaces the Tasmota driver tick.
# math.rand() replaces crypto.random(), which Newmota offers only with qualified entropy.
#################################################################################
class AC_CONTROLLER
  var temp_inside_buffer
  var temp_outside_buffer
  var update_index
  var current_temp_inside
  var current_temp_outside
  var target_temp
  var operation_mode
  var time_counter

  static BUFFER_SIZE = 96  # 24 hours * 4 measurements per hour (every 15 min)

  def init()
    self.update_index = 0
    self.time_counter = 0

    # Initialize temperature values
    self.current_temp_inside = 23.0
    self.current_temp_outside = 28.0
    self.target_temp = 22.0
    self.operation_mode = "cooling"

    # Initialize temperature buffers
    self.temp_inside_buffer = []
    self.temp_outside_buffer = []

    self.init_temperature_history()

    self.create_widgets()
    self.every_second()
  end

  def init_temperature_history()
    import math

    for i: 0..(self.BUFFER_SIZE-1)
      var hour_of_day = (i * 15.0) / 60.0

      var outside_temp = 30.0
      if hour_of_day >= 6.0 && hour_of_day <= 18.0
        var hours_from_peak = math.abs(14.0 - hour_of_day)
        outside_temp = 25.0 + 15.0 * math.max(0, 1.0 - (hours_from_peak / 8.0))
      else
        outside_temp = 22.0 + 3.0 * math.sin(hour_of_day * 0.26)
      end

      outside_temp += ((math.rand() % 256) % 20 - 10) / 10.0
      if outside_temp < 20.0
        outside_temp = 20.0
      end
      if outside_temp > 40.0
        outside_temp = 40.0
      end

      var inside_temp = 22.0
      if hour_of_day >= 10.0 && hour_of_day <= 16.0
        inside_temp = 22.0 + 2.0 * math.sin((hour_of_day - 10.0) * 0.5)
      else
        inside_temp = 21.0 + math.sin(hour_of_day * 0.3)
      end

      inside_temp += ((math.rand() % 256) % 10 - 5) / 20.0
      if inside_temp < 20.0
        inside_temp = 20.0
      end
      if inside_temp > 25.0
        inside_temp = 25.0
      end

      self.temp_inside_buffer.push(int(inside_temp * 10))
      self.temp_outside_buffer.push(int(outside_temp * 10))
    end
  end

  def every_second()
    self.time_counter += 1

    if self.time_counter % 15 == 0
      self.measure_temperatures()
    end

    self.update_ac_operation()
    self.update_next_widget()
    newmota.set_timer(1000, /-> self.every_second(), "ac_controller")
  end

  def measure_temperatures()
    import math

    var time_of_day = (self.time_counter / 60.0) % 24.0

    var outside_base = 30.0
    if time_of_day >= 6 && time_of_day <= 18
      var hours_from_peak = math.abs(14.0 - time_of_day)
      outside_base = 25.0 + 15.0 * math.max(0, 1.0 - (hours_from_peak / 8.0))
    else
      outside_base = 22.0 + 3.0 * math.sin(time_of_day * 0.26)
    end

    self.current_temp_outside = outside_base + ((math.rand() % 256) % 40 - 20) / 10.0
    if self.current_temp_outside < 20.0
      self.current_temp_outside = 20.0
    end
    if self.current_temp_outside > 40.0
      self.current_temp_outside = 40.0
    end

    var temp_drift = (self.current_temp_outside - self.current_temp_inside) * 0.03

    if self.operation_mode == "cooling"
      if self.current_temp_inside > self.target_temp
        self.current_temp_inside -= 0.2
      else
        self.current_temp_inside += temp_drift * 0.3
      end
    elif self.operation_mode == "heating"
      if self.current_temp_inside < self.target_temp
        self.current_temp_inside += 0.2
      else
        self.current_temp_inside += temp_drift * 0.3
      end
    else
      self.current_temp_inside += temp_drift * 0.5
    end

    self.current_temp_inside += ((math.rand() % 256) % 10 - 5) / 20.0
    if self.current_temp_inside < 20.0
      self.current_temp_inside = 20.0
    end
    if self.current_temp_inside > 25.0
      self.current_temp_inside = 25.0
    end

    self.temp_inside_buffer.remove(0)
    self.temp_inside_buffer.push(int(self.current_temp_inside * 10))

    self.temp_outside_buffer.remove(0)
    self.temp_outside_buffer.push(int(self.current_temp_outside * 10))
  end

  def update_ac_operation()
    import math

    var temp_diff = self.current_temp_inside - self.target_temp

    if temp_diff > 1.5
      self.operation_mode = "cooling"
    elif temp_diff < -1.5
      self.operation_mode = "heating"
    elif math.abs(temp_diff) < 0.5
      var rand = (math.rand() % 256) % 100
      if rand > 95
        self.operation_mode = "venting"
      elif rand > 90
        self.operation_mode = "drying"
      else
        self.operation_mode = "off"
      end
    end

    if (math.rand() % 256) % 500 == 0
      self.target_temp = 20.0 + ((math.rand() % 256) % 5)
    end
  end

  # Markup once. The first widget keeps the original's coloured markers in its text;
  # the mode widget colours its value from a few lines of its own script.
  def create_widgets()
    import widget
    widget.set("graph1_widget", '<div class="box w2 h1"><h3>🌡️ Temperature (24h)</h3>'
      '<p class="label"><span style="color:#ff9632">●</span> Inside <b data-v="inside"></b> °C'
      ' · <span style="color:#32c8ff">●</span> Outside <b data-v="outside"></b> °C</p>'
      '<div data-chart="line" data-v="history" data-color="#ff9632,#32c8ff" data-grid'
      ' data-labels="00:00,04:00,08:00,12:00,16:00,20:00,24:00"></div></div>')
    widget.set("graph2_widget", '<div class="box w1 h1"><h3>🎯 Target Temperature</h3>'
      '<div data-chart="gauge" data-v="target"></div></div>')
    widget.set("graph3_widget", '<div class="box w1 h1"><h3>⚙️ Operation Mode</h3>'
      '<p class="value ac-mode" data-v="mode" style="text-align:center"></p>'
      '<p class="label" style="text-align:center">Inside <b data-v="inside"></b> °C | Outside <b data-v="outside"></b> °C</p>'
      '<script>nm.on("graph3_widget", (data, card) => {'
      'card.querySelector(".ac-mode").style.color = "#" + (data.color >>> 0).toString(16).padStart(6, "0");'
      '});</script></div>')
  end

  def update_next_widget()
    import widget
    var widget_num = (self.update_index % 3) + 1
    widget.data(format("graph%d_widget", widget_num), self.get_widget_data(widget_num))
    self.update_index += 1
  end

  def get_widget_data(widget_num)
    var inside = format("%.1f", self.current_temp_inside)
    var outside = format("%.1f", self.current_temp_outside)
    if widget_num == 1
      return {"inside": inside, "outside": outside,
              "history": [self.buffer_to_decimal(self.temp_inside_buffer),
                          self.buffer_to_decimal(self.temp_outside_buffer)]}
    elif widget_num == 2
      # The range follows the inside temperature, so min, max and stops travel as data
      var t = self.current_temp_inside
      return {"target": {"value": self.target_temp, "min": int(t - 8), "max": int(t + 8), "unit": "°C",
              "stops": [[int(t - 5), 0x6496FF], [int(t - 2), 0x64FF96], [int(t + 2), 0xFFC864], [int(t + 5), 0xFF6432]]}}
    end
    var modes = {"cooling": ["COOLING", 0x6496FF], "heating": ["HEATING", 0xFF9632],
                 "venting": ["VENTING", 0x64FF96], "drying": ["DRYING", 0xFFC864]}
    var mode = modes.find(self.operation_mode, ["OFF", 0x969696])
    return {"mode": mode[0], "color": mode[1], "inside": inside, "outside": outside}
  end

  def buffer_to_decimal(buffer)
    var values = []
    for val: buffer
      values.push(val / 10)
    end
    return values
  end

  def stop()
    import widget
    newmota.remove_timer("ac_controller")
    for widget_num: 1..3
      widget.remove(format("graph%d_widget", widget_num))
    end
  end
end

ac_controller = AC_CONTROLLER()