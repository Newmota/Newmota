#################################################################################
# Widget for the BLE scanner
#
# Port of berry-examples/dashboard/scanner.be to the Newmota `widget` and `BLE` modules.
# As in the original, the widget brings its own markup and script and filters in the
# browser. What differs:
# - the markup is set once; the device list travels as data, so device names from the
#   radio reach the page only as text (nm.rows), never as markup;
# - "Clear results" and "Active scanning" use data-act instead of fetching /m32;
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

  static MAX_RESULTS = 100
  static MARKUP = '<div class="box w2 h2"><h3>BLE scanner</h3>'
    '<label class="row"><span class="label">Active scanning</span>'
      '<input type="checkbox" class="toggle" data-act="act" data-v="act"></label>'
    '<p class="small scan-log"></p>'
    '<label class="row"><input type="checkbox" data-f="igCC"><span class="label">Ignore CID (in HEX)</span><input type="text" data-f="igCV" size="8"></label>'
    '<label class="row"><input type="checkbox" data-f="igRC"><span class="label">Ignore weaker RSSI</span><input type="number" data-f="igRV" min="-99" max="0"></label>'
    '<label class="row"><input type="checkbox" data-f="igTC"><span class="label">Ignore (Address) Type</span><input type="number" data-f="igTV" min="0" max="4"></label>'
    '<label class="row"><input type="checkbox" data-f="fiSUC"><span class="label">Filter Service UUID</span><input type="text" data-f="fiSUV" size="8"></label>'
    '<label class="row"><input type="checkbox" data-f="fiSDC"><span class="label">Filter Service Data</span><input type="text" data-f="fiSDV" size="8"></label>'
    '<p><button data-act="clear">Clear results</button></p>'
    '<div class="scroll"><table><thead><tr><th>MAC</th><th>Type</th><th>SVC_D</th><th>CID</th>'
      '<th>SVC_U</th><th>Name</th><th>RSSI</th></tr></thead><tbody></tbody></table></div>'
    '<script>(() => {'
      'const card = nm.card("scan1"), f = name => card.querySelector("[data-f=" + name + "]");'
      'let data = {rows: [], total: 0};'
      'const draw = () => {'
        'const on = name => f(name).checked, val = name => f(name).value;'
        'const rows = data.rows.filter(r => !((on("igTC") && +val("igTV") === +r[1]) ||'
          '(on("igCC") && val("igCV").includes(r[3])) || (on("igRC") && +val("igRV") > r[6]) ||'
          '(on("fiSDC") && val("fiSDV") !== r[2]) || (on("fiSUC") && val("fiSUV") !== r[4])));'
        'nm.rows(card.querySelector("tbody"), rows);'
        'card.querySelector(".scan-log").textContent = "Showing " + rows.length + "/" + data.rows.length +'
          '" devices" + (data.total > data.rows.length ? ", newest " + data.rows.length + " of " + data.total + " sent" : "");'
      '};'
      'card.addEventListener("input", draw);'
      'card.addEventListener("change", draw);'
      'nm.on("scan1", value => { data = value; draw(); });'
    '})();</script></div>'

  def init()
      import widget
      self.scan_timer = 0
      self.scan_result = []
      self.stop_scan = false
      self.active_scan = newmota.cmd("BLEActive")['BLEActive'] == 1
      self.ble_down = true
      self.update_table = false

      widget.set("scan1", self.MARKUP)
      self.send_rows()
      widget.on("scan1", /name, value -> self.on_action(name, value))
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
            self.send_rows()
            self.update_table = false
        end
      end
      self.scan_timer = (widget.viewers() && self.stop_scan == false) ? 1 : 0
      newmota.set_timer(1000, /-> self.loop(), "scan_widget")
  end

  # Replaces the page's 'clear' and 'act' requests; the filters stay in the browser.
  def on_action(name, value)
      if name == "clear"
        log("M32: clear",2)
        self.scan_result = []
      elif name == "act"
        var on = value == "true"
        newmota.cmd(on ? "BLEActive 1" : "BLEActive 0")
        self.active_scan = on
      end
      self.send_rows()
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
      log(str(entry),4)
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

  # One widget's data is at most 2816 bytes of JSON, so long lists send only their newest
  # rows: as many as fit, halving the count until they do.
  def send_rows()
    import widget
    var rows = []
    for entry: self.scan_result
      rows.push([entry['MAC'].tohex(), entry['Type'], format("%04x", entry['SVC_DATA']),
                 format("%04x", entry['CID']), format("%04x", entry['SVC_UUID']), entry['Name'], entry['RSSI']])
    end
    var listed = size(rows)
    while true
      try
        var sent = listed > 0 ? rows[size(rows)-listed..size(rows)-1] : []
        widget.data("scan1", {"act": self.active_scan, "rows": sent, "total": size(rows)})
        break
      except 'value_error'
        if listed == 0 raise 'value_error', 'scanner widget too large' end
        listed = listed / 2
      end
    end
  end
end

sw = SCAN_WIDGET()
