#################################################################################
# Widget for the BLE scanner
#
# Port of berry-examples/dashboard/scanner.be to the Newmota `widget` and `BLE` modules.
# Only the APIs differ:
# - the widget is described as data; its controls call on_action() instead of
#   requesting /m32 from inline JavaScript, so the filters run here and only
#   matching rows are sent (at most MAX_ROWS, to stay within one widget);
# - widget.viewers() replaces the page's init and loop requests;
# - BLE.adv_cb() takes no buffer: the advertisement is in BLE.buffer, with the
#   payload from offset 8 and its length in BLE.len;
# - "BLEActive" replaces "MI32Option4".
# Deliberate fixes beyond the API: RSSI is read as a signed byte (legacy was 1 dBm high),
# results are collected only while a dashboard is open (legacy never stopped), and at
# most MAX_RESULTS devices are kept, dropping the oldest (legacy grew without bound).
# Needs a firmware with USE_BLE.
#################################################################################
class SCAN_WIDGET

  var scan_timer, scan_result, active_scan
  var update_table, stop_scan
  var ble_down
  var igCC, igCV, igRC, igRV, igTC, igTV, fiSUC, fiSUV, fiSDC, fiSDV

  static MAX_ROWS = 30
  static MAX_RESULTS = 100

  def init()
      import widget
      self.scan_timer = 0
      self.scan_result = []
      self.stop_scan = false
      self.active_scan = newmota.cmd("BLEActive")['BLEActive'] == 1
      self.ble_down = true
      self.igCC = false
      self.igCV = ""
      self.igRC = false
      self.igRV = -99
      self.igTC = false
      self.igTV = 0
      self.fiSUC = false
      self.fiSUV = ""
      self.fiSDC = false
      self.fiSDV = ""

      self.init_widget()
      widget.on("scan1", /part, value -> self.on_action(part, value))
      self.loop()
  end

  def stop()
      import BLE
      import widget
      BLE.adv_cb(nil)
      newmota.remove_timer("scan_widget")
      widget.remove("scan1")
  end

  def cb(svc,man)
      if self.scan_timer > 0
          self.add_to_result()
      end
  end

  # Replaces the page's 'init=1' and 1 s 'loop=1' requests.
  def loop()
      import widget
      if widget.viewers()
        if self.ble_down == true
          import BLE
          import cb
          log("M32: web UI did init",2)
          var cbp = cb.gen_cb(/svc,man -> self.cb(svc,man))
          BLE.adv_cb(cbp)
          self.ble_down = false
        end
        if self.update_table==true
            self.init_widget()
            self.update_table = false
        end
      end
      self.scan_timer = (widget.viewers() && self.stop_scan == false) ? 1 : 0
      newmota.set_timer(1000, /-> self.loop(), "scan_widget")
  end

  # Replaces the page's 'clear', 'act' and filter handlers.
  def on_action(part, value)
      var on = value == "true"
      if part == "clear"
        log("M32: clear",2)
        self.scan_result = [];
      elif part == "act"
        newmota.cmd(on ? "BLEActive 1" : "BLEActive 0")
        self.active_scan = on
      elif part == "igCC"
        self.igCC = on
      elif part == "igCV"
        self.igCV = value
      elif part == "igRC"
        self.igRC = on
      elif part == "igRV"
        self.igRV = int(value)
      elif part == "igTC"
        self.igTC = on
      elif part == "igTV"
        self.igTV = int(value)
      elif part == "fiSUC"
        self.fiSUC = on
      elif part == "fiSUV"
        self.fiSUV = value
      elif part == "fiSDC"
        self.fiSDC = on
      elif part == "fiSDV"
        self.fiSDV = value
      end
      self.init_widget()
  end

 def add_to_result()
      import BLE
      var buf = BLE.buffer
      var entry = {'SVC_UUID':0,'Name':'','SVC_DATA':0,'CID':0,'RSSI':0}
      entry.insert('MAC',buf[0..5])
      entry.insert('Type',buf[6])
      var rssi = buf.geti(7,1)
      entry['RSSI'] = rssi

      var idx = 0
      var match = false

      if size(self.scan_result) > 0
          for i:0..size(self.scan_result)-1
              if buf[0..5] == self.scan_result[i]['MAC']
                  idx = i
                  match = true
                  entry = self.scan_result[i]
                  entry['RSSI'] = rssi
                  self.scan_result.remove(i)
                  break
              end
          end
      end

      var len_p = BLE.len
      var i = 8                       #- start of payload -#
      while i < 8+len_p
          var len = buf.get(i,1)
          var _type = buf.get(i+1,1)
          if _type==2 || _type==3
            entry['SVC_UUID']=buf.get(i+2,2)
          end
          if _type==0x16
            entry['SVC_DATA']=buf.get(i+2,2)
          end
          if _type==0xff
            entry['CID']=buf.get(i+2,2)
          end
          if _type==8 || _type==9
            var _name = buf[i+2..(i+len)]# + bytes('00') # null terminate it
            entry['Name']=_name.asstring()
          end
          i+=len+1
      end
      log(entry,4)
      if match == true
        self.scan_result.insert(idx,entry)
      else
        if size(self.scan_result) >= self.MAX_RESULTS
          self.scan_result.remove(0)
        end
        self.scan_result.push(entry)
      end
      self.update_table=true
  end

  # The filter of the legacy page script, applied before the rows are sent.
  def get_rows()
      import string
      var rows = []
      for entry: self.scan_result
          var svc_d = format("%04x", entry['SVC_DATA'])
          var cid = format("%04x", entry['CID'])
          var svc_u = format("%04x", entry['SVC_UUID'])
          if (self.igTC == true && self.igTV == entry['Type']) || (self.igCC == true && string.find(self.igCV, cid) >= 0) || (self.igRC == true && self.igRV > entry['RSSI'])
            continue
          end
          if (self.fiSDC == true && self.fiSDV != svc_d) || (self.fiSUC == true && self.fiSUV != svc_u)
            continue
          end
          rows.push([entry['MAC'].tohex(), format("%i", entry['Type']), svc_d, cid, svc_u, entry['Name'], entry['RSSI']])
      end
      return rows
  end

  # A widget is at most 3968 bytes of JSON, so long names can lower the row count further.
  def init_widget()
    var rows = self.get_rows()
    var listed = size(rows) < self.MAX_ROWS ? size(rows) : self.MAX_ROWS
    while true
      try
        self.send_widget(rows, listed)
        break
      except 'value_error'
        if listed == 0 raise 'value_error', 'scanner widget too large' end
        listed = listed / 2
      end
    end
    log("M32: Init Widget", 4)
  end

  def send_widget(rows, listed)
    import widget
    var shown = format("Showing %i/%i devices", size(rows), size(self.scan_result))
    if listed < size(rows)
      shown += format(", first %i listed", listed)
    end
    rows = listed > 0 ? rows[0..listed-1] : []
    widget.set("scan1", {"title": "BLE scanner", "w": 2, "h": 2, "parts": [
      {"toggle": "Active scanning", "name": "act", "state": self.active_scan},
      {"text": shown, "small": true},
      {"toggle": "Ignore CID (in HEX)", "name": "igCC", "state": self.igCC},
      {"input": "CID", "name": "igCV", "value": self.igCV},
      {"toggle": "Ignore weaker RSSI", "name": "igRC", "state": self.igRC},
      {"number": "RSSI", "name": "igRV", "value": self.igRV, "min": -99, "max": 0},
      {"toggle": "Ignore (Address) Type", "name": "igTC", "state": self.igTC},
      {"number": "Type", "name": "igTV", "value": self.igTV, "min": 0, "max": 4},
      {"toggle": "Filter Service UUID", "name": "fiSUC", "state": self.fiSUC},
      {"input": "Service UUID", "name": "fiSUV", "value": self.fiSUV},
      {"toggle": "Filter Service Data", "name": "fiSDC", "state": self.fiSDC},
      {"input": "Service Data", "name": "fiSDV", "value": self.fiSDV},
      {"button": "Clear results", "name": "clear"},
      {"table": rows, "columns": ["MAC", "Type", "SVC_D", "CID", "SVC_U", "Name", "RSSI"]}]})
  end
end

sw = SCAN_WIDGET()
