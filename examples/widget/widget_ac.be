#################################################################################
# Air Conditioner Controller Dashboard - Simple Version
#
# Port of berry-examples/dashboard/ac_mockup.be to the Newmota `widget` module.
# Only the API differs: widgets are described as data instead of HTML/DSL strings,
# and a self-rescheduling timer replaces the Tasmota driver tick. Inline HTML colour
# markers in the first title become a text part.
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

  def update_next_widget()
    import widget
    var widget_num = (self.update_index % 3) + 1
    widget.set(format("graph%d_widget", widget_num), self.get_widget_content(widget_num))
    self.update_index += 1
  end

  def get_widget_content(widget_num)
    var parts = []
    var title = ""
    var w = 1

    if widget_num == 1
      # Temperature History (2x1)
      title = "🌡️ Temperature (24h)"
      w = 2
      parts.push({"text": format("Inside %.1f°C · Outside %.1f°C",
                                 self.current_temp_inside, self.current_temp_outside)})

      # Show labels every 4 hours across the 96 data points (24 hours, 15-min intervals)
      var time_labels = ["00:00", "04:00", "08:00", "12:00", "16:00", "20:00", "24:00"]

      parts.push({"line": [{"values": self.buffer_to_decimal(self.temp_inside_buffer), "color": 0xFF9632},
                           {"values": self.buffer_to_decimal(self.temp_outside_buffer), "color": 0x32C8FF}],
                  "labels": time_labels, "grid": true})

    elif widget_num == 2
      # Target Temperature Gauge (1x1)
      title = "🎯 Target Temperature"

      var inside = self.current_temp_inside
      var target = self.target_temp

      var stop1 = int(inside - 5)
      var stop2 = int(inside - 2)
      var stop3 = int(inside + 2)
      var stop4 = int(inside + 5)

      parts.push({"gauge": target, "min": int(inside - 8), "max": int(inside + 8), "unit": "°C",
                  "stops": [[stop1, 0x6496FF], [stop2, 0x64FF96], [stop3, 0xFFC864], [stop4, 0xFF6432]]})

    elif widget_num == 3
      # Operation Mode (1x1)
      title = "⚙️ Operation Mode"

      var mode_text = ""
      var mode_color = 0

      if self.operation_mode == "cooling"
        mode_text = "COOLING"
        mode_color = 0x6496FF
      elif self.operation_mode == "heating"
        mode_text = "HEATING"
        mode_color = 0xFF9632
      elif self.operation_mode == "venting"
        mode_text = "VENTING"
        mode_color = 0x64FF96
      elif self.operation_mode == "drying"
        mode_text = "DRYING"
        mode_color = 0xFFC864
      else
        mode_text = "OFF"
        mode_color = 0x969696
      end

      var mode_values = []
      for i: 0..29
        mode_values.push(100)
      end

      parts.push({"hist": mode_values, "color": mode_color})
      parts.push({"value": mode_text})
      parts.push({"text": format("Inside: %.1f°C | Outside: %.1f°C",
                                 self.current_temp_inside, self.current_temp_outside)})
    end

    return {"title": title, "w": w, "h": 1, "parts": parts}
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
