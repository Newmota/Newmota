# Adapted from Tasmota lv_tasmota_widgets.be (Stephan Hadinger, GPL-3.0-or-later).
# Newmota owns timer lifecycle; drawing and parent-padding behavior follow Tasmota.
import lv

# The imported callback singleton removes an object's registration in its first
# DELETE callback. Keep the current delete callback list until native dispatch
# finishes, so later DELETE handlers can release their timers. The application
# poll drops these temporary roots after lv_timer_handler returns.
class NewmotaLVGL: LVGL_glob
  var deleted

  def remove_cb(obj)
    if self.deleted == nil self.deleted = {} end
    var ptr = obj._p
    self.deleted[ptr] = [obj, self.cb_event_closure.find(ptr)]
    self.deregister_obj(ptr)
  end

  def dispatch_event(rank, target, code)
    var obj, callbacks
    if code == lv.EVENT_DELETE && rank > 0
      var saved = self.deleted == nil ? nil : self.deleted.find(target)
      if saved == nil return end
      obj = saved[0]
      callbacks = saved[1]
    else
      obj = self.get_object_from_ptr(target)
      callbacks = self.cb_event_closure.find(target)
    end
    if type(callbacks) == 'function'
      callbacks(obj, self.event)
    elif callbacks != nil && rank < size(callbacks)
      callbacks[rank](obj, self.event)
    end
  end

  def lvgl_event_dispatch(rank, event_ptr_i)
    import introspect
    var event_ptr = introspect.toptr(event_ptr_i)
    if self.event self.event._p = event_ptr
    else self.event = lv.lv_event(event_ptr) end
    self.dispatch_event(rank, self.event.get_target(), self.event.get_code())
  end
end

class lv_clock: lv.label
  var hour, minute, sec, timer

  def init(parent)
    super(self).init(parent)
    var f_s7_16 = lv.seg7_font(16)
    if f_s7_16 != nil  self.set_style_text_font(f_s7_16, lv.PART_MAIN | lv.STATE_DEFAULT) end

    self.set_text("--:--")

    self.add_event_cb(/->self.before_del(), lv.EVENT_DELETE, 0)   # register `before_del` to be called when object is deleted
    self.timer = lv.timer_create(/ timer -> self.every_second(), 1000, nil)
  end

  def set_time(hour, minute, sec)
    if hour != self.hour || minute != self.minute || sec != self.sec
      var txt = format("%02d%s%02d", hour, sec % 2 ? ":" : " ", minute)
      self.hour = hour
      self.minute = minute
      self.sec = sec
      # if txt[0] == '0'  txt = '!' .. string.split(txt,1)[1] end  # replace first char with '!'
      self.set_text(txt)
    end
  end

  def every_second()
    import introspect
    if !introspect.contains(newmota, "rtc") return end
    var now = newmota.time_dump(newmota.rtc()['local'])
    if now['year'] > 1970
      self.set_time(now['hour'], now['min'], now['sec'])
    end
  end

  def before_del()
    if self.timer != nil
      _lvgl.deregister_obj(self.timer._p)
      self.timer.delete()
      self.timer = nil
    end
  end
end

#@ solidify:lv_clock_icon,weak
class lv_clock_icon: lv_clock

  def init(parent)
    super(self).init(parent)

    if parent != nil
      var parent_height = parent.get_height()

      self.refr_size()
      var w = self.get_width()
      self.set_y((parent.get_height() - self.get_height()) / 2)   # center vertically

      var pad_right = parent.get_style_pad_right(lv.PART_MAIN | lv.STATE_DEFAULT)
      self.set_x(parent.get_width() - w - pad_right - 3)
      parent.set_style_pad_right(pad_right + w + 6, lv.PART_MAIN | lv.STATE_DEFAULT)

      self.set_style_bg_color(lv.color(lv.COLOR_BLACK), lv.PART_MAIN | lv.STATE_DEFAULT)
    end
  end

end

# ##########################################################################################


#@ solidify:lv_signal_arcs,weak
class lv_signal_arcs : lv.obj
  var timer
  var percentage          # value to display, range 0..100
  var area, arc_dsc       # instances of objects kept to avoid re-instanciating at each call

  def init(parent)
    super(self).init(parent)
    self.set_style_bg_opa(0, 0)         # transparent background
    self.set_style_border_width(0, 0)   # remove border
    self.set_style_pad_all(0,0)
    # own values
    self.percentage = 100
    # pre-allocate buffers
    self.area = lv.area()
    self.arc_dsc = lv.draw_arc_dsc()
    # we don't need a closure with `add_event_cb` because of special handling
    self.add_event_cb(self.widget_event, lv.EVENT_DRAW_MAIN, 0)
  end

  def widget_event(event)
    var code = event.get_code()

    import math
    def atleast1(x) if x >= 1 return x else return 1 end end
    # the model is that we have 4 bars and inter-bar (1/4 of width)
    var height = self.get_height()
    var width = self.get_width()

    var inter_bar = atleast1(height / 8)
    var bar = atleast1((height - inter_bar * 2) / 3)
    var bar_offset = bar / 2
    #print("inter_bar", inter_bar, "bar", bar, "bar_offset", bar_offset)

    if code == lv.EVENT_DRAW_MAIN
      var arc_dsc = self.arc_dsc
      var param = event.get_param()
      var layer = event.get_layer()

      # get coordinates of object
      self.get_coords(self.area)
      var x_ofs = self.area.x1
      var y_ofs = self.area.y1

      lv.draw_arc_dsc_init(arc_dsc)                  # initialize lv.draw_line_dsc structure
      self.init_draw_arc_dsc(lv.PART_MAIN, arc_dsc)  # copy the current values

      arc_dsc.rounded = 1
      arc_dsc.width = (bar * 3 + 1) / 4
      var on_color = self.get_style_line_color(lv.PART_MAIN | lv.STATE_DEFAULT)
      var off_color = self.get_style_bg_color(lv.PART_MAIN | lv.STATE_DEFAULT)

      # initial calculation, but does not take into account bounding box
      # var angle = int(math.deg(math.atan2(width / 2, height)))

      # better calculation
      var hypotenuse = height - bar           # center if at bar/2 from bottom and circle stops at bar/2 from top
      var adjacent = width / 2 - bar_offset   # stop at bar_offset from side
      var ratio = hypotenuse > 0 ? real(adjacent) / real(hypotenuse) : 1.0
      if ratio > 1.0 ratio = 1.0 end
      if ratio < 0.0 ratio = 0.0 end
      var angle = int(90 - math.deg(math.acos(ratio)))
      if (angle > 45) angle = 45 end

      # print("hypotenuse",hypotenuse,"adjacent",adjacent,"angle",angle)
      arc_dsc.center_x = x_ofs + width / 2
      arc_dsc.center_y = y_ofs + height - 1 - bar_offset

      arc_dsc.color = self.percentage >= 25 ? on_color : off_color
      arc_dsc.radius = 0 * (bar + inter_bar) + bar_offset
      arc_dsc.start_angle = 0
      arc_dsc.end_angle = 306
      lv.draw_arc(layer, arc_dsc)

      arc_dsc.radius = 1 * (bar + inter_bar) + bar_offset - 1
      arc_dsc.start_angle = 270 - angle
      arc_dsc.end_angle = 270 + angle
      arc_dsc.color = self.percentage >= 50 ? on_color : off_color
      lv.draw_arc(layer, arc_dsc)

      arc_dsc.radius = 2 * (bar + inter_bar) + bar_offset - 2
      arc_dsc.start_angle = 270 - angle
      arc_dsc.end_angle = 270 + angle
      arc_dsc.color = self.percentage >= 75 ? on_color : off_color
      lv.draw_arc(layer, arc_dsc)
    end
  end

  def set_percentage(v)
    var old_bars = self.percentage / 25
    if v > 100 v = 100 end
    if v < 0   v = 0 end
    self.percentage = v
    if old_bars != v / 25
      self.invalidate()    # be frugal and avoid updating the widget if it's not needed
    end
  end

  def get_percentage()
    return self.percentage
  end
end

#@ solidify:lv_wifi_arcs,weak
class lv_wifi_arcs: lv_signal_arcs
  def init(parent)
    super(self).init(parent)
    self.timer = lv.timer_create(/ timer -> self.every_second(), 1000, nil)
    self.add_event_cb(/ event -> self.before_del(), lv.EVENT_DELETE, 0)
    self.set_percentage(0)    # we generally start with 0, meaning not connected
  end

  def every_second()
    var wifi = newmota.wifi()
    var quality = wifi.find("quality")
    var ip = wifi.find("ip")
    if ip == nil
      self.set_percentage(0)
    elif quality != nil
      self.set_percentage(quality)
    end
  end

  def before_del()    # called when the widget is being deleted
    if self.timer != nil
      _lvgl.deregister_obj(self.timer._p)
      self.timer.delete()
      self.timer = nil
    end
  end
end

#@ solidify:lv_wifi_arcs_icon,weak
class lv_wifi_arcs_icon: lv_wifi_arcs
  def init(parent)
    super(self).init(parent)
    self.set_style_line_color(lv.color(lv.COLOR_WHITE), lv.PART_MAIN | lv.STATE_DEFAULT)
    self.set_style_bg_color(lv.color(lv.COLOR_BLACK), lv.PART_MAIN | lv.STATE_DEFAULT)
    if parent != nil
      var parent_height = parent.get_height()
      var pad_right = parent.get_style_pad_right(lv.PART_MAIN | lv.STATE_DEFAULT)
      self.set_height(parent_height)
      var w = (parent_height*4)/3
      self.set_width(w)	# 130%
      self.set_x(parent.get_width() - w - pad_right)
      parent.set_style_pad_right(pad_right + w + 1, lv.PART_MAIN | lv.STATE_DEFAULT)
    end
  end
end


lv.clock = lv_clock
lv.clock_icon = lv_clock_icon
lv.signal_arcs = lv_signal_arcs
lv.wifi_arcs = lv_wifi_arcs
lv.wifi_arcs_icon = lv_wifi_arcs_icon

_lvgl = NewmotaLVGL()
