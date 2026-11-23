# Node, fabrics, commissioning windows and DNS-SD publication. This owner runs in the Berry
# application context and is entered only through message(), tick() and init().

#@ solidify:Matter_Fabric,weak
#@ solidify:Matter_Session,weak
#@ solidify:Matter_Endpoint,weak
#@ solidify:Matter_Device,weak

class Matter_Fabric
  var index, rcac, noc, icac, ipk, vendor, subject, key, label
  var fabric_id, node_id, compressed
  var acl                 # entries by tag: 1 privilege, 2 authentication, 3 subjects, 4 targets

  # Normalises a node or fabric id: the TLV reader gives bytes only for 8-byte encodings
  static def id64(v)
    if isinstance(v, bytes)   return v    end
    return bytes().add(v == nil ? 0 : v, 4).add(0, 4)
  end

  def root_pub()
    return matter.tlv(self.rcac)[9]
  end

  def group_key()
    import crypto
    return crypto.HKDF_SHA256().derive(self.ipk, self.compressed, bytes().fromstring("GroupKey v1.0"), 16)
  end

  def encode()
    var w = matter.Tlv().st(nil)
    w.uint(1, self.index)
    w.bin(2, self.rcac)
    w.bin(3, self.noc)
    if self.icac    w.bin(4, self.icac)    end
    w.bin(5, self.ipk)
    w.uint(6, self.vendor)
    w.bin(7, self.key)
    w.str(8, self.label)
    w.bin(9, self.fabric_id)
    w.bin(10, self.node_id)
    w.bin(11, self.compressed)
    w.bin(12, self.encode_acl())
    w.close()
    return w.b
  end

  # Writes one entry of the Access Control cluster
  def write_entry(w, e)
    w.st(nil)
    w.uint(1, e[1])
    w.uint(2, e[2])
    var subjects = e.find(3)
    if subjects == nil
      w.null(3)
    else
      w.arr(3)
      for s : subjects    w.u64(nil, self.id64(s))    end
      w.close()
    end
    var targets = e.find(4)
    if targets == nil
      w.null(4)
    else
      w.arr(4)
      for target : targets
        w.st(nil)
        for k : [0, 1, 2]
          if target.find(k) == nil    w.null(k)   else w.uint(k, target[k])    end
        end
        w.close()
      end
      w.close()
    end
    w.uint(0xFE, self.index)
    w.close()
  end

  def encode_acl()
    var w = matter.Tlv().arr(nil)
    for e : self.acl    self.write_entry(w, e)    end
    w.close()
    return w.b
  end

  def decode(v)
    self.index = v[1]
    self.rcac = v[2]
    self.noc = v[3]
    self.icac = v.find(4)
    self.ipk = v[5]
    self.vendor = v[6]
    self.key = v[7]
    self.label = v[8]
    self.fabric_id = v[9]
    self.node_id = v[10]
    self.compressed = v[11]
    self.acl = matter.tlv(v.find(12, bytes("1618")))
    return self
  end
end

class Matter_Session
  var ctx, challenge, fabric, filtered, peer_node, cats, write_path

  def init(ctx, challenge, fabric)
    self.ctx = ctx
    self.challenge = challenge
    self.fabric = fabric
  end
end

class Matter_Endpoint
  var id, device_types, clusters, parts
  var clients             # client cluster ids for the Descriptor, or nil

  def init(id, device_types)
    self.id = id
    self.device_types = device_types
    self.clusters = []
  end
end

class Matter_Device
  static var DATA_MODEL_REVISION = 21
  static var IM_REVISION = 13
  static var SPECIFICATION_VERSION = 0x01060100
  static var VENDOR_ID = 0xFFF1
  static var PRODUCT_ID = 0x8000
  static var MAX_FABRICS = 5
  static var PBKDF_ITERATIONS = 1000
  static var WINDOW_SECONDS = 900
  static var EVENT_RANGE = 1000
  static var MAX_EVENTS = 8
  static var INVALID_PASSCODES = [0, 11111111, 22222222, 33333333, 44444444, 55555555, 66666666,
    77777777, 88888888, 99999999, 12345678, 87654321]

  var host
  var sessions, handshakes, fabrics
  var endpoints, im
  var discriminator, passcode, salt, w0, L
  var window, window_deadline
  var failsafe_deadline, pending, pending_root, pending_key, breadcrumb
  var pending_for_update, update_backup
  var removals
  var next_poll, next_sensors, measurements
  var seen_outputs, seen_sensors, follow_due
  var events, next_event, event_limit
  var wifi_ssid, wifi_password, wifi_tested
  var bridge
  var client              # commands to controllers, with USE_MATTER_TCP
  var enforce_access
  var boot_count, reset_armed
  var moving

  def init()
    import newmota
    var net = newmota.cmd("Status 5")
    var mac = net['StatusNET']['Mac']
    self.host = ""
    for part : range(0, size(mac) - 1)
      if mac[part] != ":"   self.host += mac[part]    end
    end
    self.sessions = {}
    self.handshakes = {}
    self.fabrics = []
    self.removals = []
    self.breadcrumb = 0
    self.next_poll = 0
    self.next_sensors = 0
    self.moving = []
    self.measurements = {}
    self.events = []
    self.enforce_access = matter.store_load("enforce") != nil
    self.boot_count = self.read_boot_count()
    self.im = matter.IM(self)
    var client_class = matter.Client
    if type(client_class) == 'class'    self.client = client_class(self)    end
    self.build_endpoints()
    self.load_identity()
    self.load_fabrics()
    for f : self.fabrics    self.load_sessions(f)   end
    self.start_events()
    matter.mdns_host(self.host)
    if size(self.fabrics) == 0
      self.open_basic_window(self.WINDOW_SECONDS)
      if !matter.wifi_connected()
        matter.ble_start(self.discriminator, self.VENDOR_ID, self.PRODUCT_ID)
      end
    end
    self.publish()
    log("MTR: Started with " + str(size(self.fabrics)) + " fabrics, pairing code " + self.pairing_code(), 2)
    log("MTR: QR code " + self.qr_code(), 2)
  end

  # Builds the node from the hardware at boot, in a stable order: lights, relays, sensors
  def build_endpoints()
    var root = matter.Endpoint(0, [[0x0016, 3]])
    for c : [matter.Descriptor, matter.AccessControl, matter.BasicInformation,
             matter.GeneralCommissioning, matter.NetworkCommissioning, matter.GeneralDiagnostics,
             matter.AdminCommissioning, matter.OperationalCredentials, matter.GroupKeys]
      root.clusters.push(c(root))
    end
    self.endpoints = [root]
    var bridge_class = matter.Bridge
    if type(bridge_class) == 'class'
      self.bridge = bridge_class(self)
      self.bridge.build_endpoints()
    end
    var lights = self.add_lights()
    # Relays hold the lowest POWER indices, possibly with gaps; lights follow them
    var powers = self.power_indices()
    var relays = size(powers) - lights
    var first_relay
    for i : 0 .. relays - 1
      var e = self.add_relay(powers[i])
      if i == 0   first_relay = e   end
    end
    var meter = matter.ElectricalPower    # nil without USE_MATTER_ENERGY
    if type(meter) == 'class'   meter.add_to(self, first_relay)   end
    var sensors = self.add_sensors()
    var camera = matter.CameraStreams     # nil without USE_MATTER_CAMERA
    if type(camera) == 'class'    camera.add_to(self)    end
    if size(self.endpoints) == 1
      self.add_relay(nil)
    end
  end

  # An endpoint with the clusters every device type needs
  def add_endpoint(device_type, revision)
    var e = matter.Endpoint(size(self.endpoints), [[device_type, revision]])
    e.clusters.push(matter.Descriptor(e))
    e.clusters.push(matter.Identify(e))
    self.endpoints.push(e)
    return e
  end

  # On/Off plug-in unit on a relay, or a virtual output when relay is nil
  def add_relay(relay)
    var e = self.add_endpoint(0x010A, 3)
    e.clusters.push(matter.Groups(e))
    e.clusters.push(matter.OnOff(e, relay != nil ? matter.RelayOutput(relay) : matter.VirtualOutput()))
    return e
  end

  # A dimmable, colour temperature or extended colour light, from what the lamp reports
  def add_light(lamp)
    var s = lamp.get()
    var channels = size(s.find('channels', []))
    var has_color = s.find('rgb') != nil
    var has_ct = s.find('ct') != nil
    var e
    if has_color          e = self.add_endpoint(0x010D, 4)
    elif has_ct           e = self.add_endpoint(0x010C, 4)
    elif channels >= 1    e = self.add_endpoint(0x0101, 3)
    else                  e = self.add_endpoint(0x0100, 3)
    end
    e.clusters.push(matter.Groups(e))
    e.clusters.push(matter.OnOff(e, lamp))
    if channels >= 1    e.clusters.push(matter.LevelControl(e, lamp))    end
    var caps = (has_color ? matter.ColorControl.FEATURE_COLOR : 0) | (has_ct ? matter.ColorControl.FEATURE_CT : 0)
    if caps != 0    e.clusters.push(matter.ColorControl(e, lamp, caps))    end
    return e
  end

  # Adds each light, typed by its channel count. Returns how many.
  def add_lights()
    var index = 0
    while true
      var s
      try
        import light        # Absent on builds without PWM or WS2812
        s = light.get(index)
      except ..
        s = nil
      end
      if s == nil || s.find('power') == nil   break   end
      self.add_light(matter.LocalLamp(index))
      index += 1
    end
    return index
  end

  # Lists the indices named by the POWER fields of Status 11, ascending
  def power_indices()
    import newmota
    var state = newmota.cmd("Status 11")
    state = state != nil ? state.find('StatusSTS') : nil
    var indices = []
    if state == nil   return indices    end
    for i : 1 .. 32
      if state.contains("POWER" + str(i))
        indices.push(i)
      end
    end
    if size(indices) == 0 && state.contains("POWER")    indices.push(1)   end
    return indices
  end

  # Converts one reading to its Matter encoding; units come from the same sensor message
  static def scale(quantity, v, status)
    import math
    if quantity == 'Temperature'
      return int((status.find('TempUnit') == "F" ? (v - 32) / 1.8 : real(v)) * 100)
    elif quantity == 'Humidity'
      return int(real(v) * 100)
    elif quantity == 'Pressure'
      var unit = status.find('PressureUnit')
      return int(v * (unit == "mmHg" ? 1.33322 : (unit == "inHg" ? 33.8639 : 1.0)))
    elif v <= 0
      return 0
    end
    var scaled = int(math.log10(real(v)) * 10000) + 1
    return scaled < 1 ? 1 : scaled
  end

  # Reads every sensor with a known quantity into scaled Matter values
  def sensor_values()
    import newmota
    var answer = newmota.cmd("Status 10")
    var status = answer != nil ? answer.find('StatusSNS') : nil
    var values = []
    if !isinstance(status, map)   return values   end
    for name : status.keys()
      var fields = status[name]
      if !isinstance(fields, map)   continue    end
      for quantity : ['Temperature', 'Humidity', 'Pressure', 'Illuminance']
        var v = fields.find(quantity)
        if v == nil   continue    end
        values.push([name + "#" + quantity, quantity, self.scale(quantity, v, status)])
      end
    end
    return values
  end

  # Adds an endpoint for each measured quantity. Sensors found later wait for a restart.
  def add_sensors()
    var values = self.sensor_values()
    for v : values
      self.measurements[v[0]] = self.add_sensor(v[1], v[2])
    end
    return size(values) > 0
  end

  # Adds one sensor endpoint and returns its measurement cluster
  def add_sensor(quantity, value)
    var e, m
    if quantity == 'Temperature'
      e = self.add_endpoint(0x0302, 2)
      m = matter.Measurement(e, 0x0402, 4, -5000, 15000, true)
    elif quantity == 'Humidity'
      e = self.add_endpoint(0x0307, 2)
      m = matter.Measurement(e, 0x0405, 3, 0, 10000, false)
    elif quantity == 'Pressure'
      e = self.add_endpoint(0x0305, 2)
      m = matter.Measurement(e, 0x0403, 3, 500, 1500, true)
    else
      e = self.add_endpoint(0x0106, 3)
      m = matter.Measurement(e, 0x0400, 3, 1, 0xFFFE, false)
    end
    e.clusters.push(m)
    m.value = value
    return m
  end

  # Event numbers keep rising across restarts, so each start reserves a range in flash
  def start_events()
    import newmota
    var stored = matter.store_load("events")
    var base = 1
    var v = stored != nil ? matter.tlv(stored) : nil
    if isinstance(v, map)   base = v[1]   end
    self.next_event = base
    self.reserve_events()
    var data = matter.Tlv().st(7).uint(0, 1).close()
    self.add_event(0, 0x0028, 0, 2, data.b)         # Basic Information StartUp, critical
  end

  # Stores the end of the next range of event numbers, so a restart starts above it
  def reserve_events()
    self.event_limit = self.next_event + self.EVENT_RANGE
    matter.store_save("events", matter.Tlv().st(nil).uint(1, self.event_limit).close().b)
  end

  # Keeps the latest events for clients that ask; older ones are gone, which the spec allows
  def add_event(ep, cluster, id, priority, data)
    if self.next_event >= self.event_limit    self.reserve_events()   end
    if size(self.events) >= self.MAX_EVENTS   self.events.remove(0)   end
    self.events.push({'number': self.next_event, 'ep': ep, 'cluster': cluster, 'id': id,
                      'priority': priority, 'time': matter.systime(), 'data': data})
    self.next_event += 1
  end

  def find_endpoint(id)
    for e : self.endpoints
      if e.id == id   return e    end
    end
    return nil
  end

  def find_cluster(ep, cluster)
    var e = self.find_endpoint(ep)
    if e != nil
      for c : e.clusters
        if c.id() == cluster    return c    end
      end
    end
    return nil
  end

  # One counter per node is enough: any change also changes the version of each cluster
  var version
  def data_version(ep, cluster)
    if self.version == nil
      import crypto
      self.version = crypto.random(4).get(0, 4) & 0x7FFFFFFF
    end
    return self.version
  end

  def changed(ep, cluster, attr)
    self.data_version(ep, cluster)
    self.version = (self.version + 1) & 0x7FFFFFFF
    self.im.changed(ep, cluster, attr)
  end

  # DeviceName once it was changed, otherwise the hostname, which is unique for each device
  def display_name()
    import newmota
    var answer = newmota.cmd("DeviceName")
    var name = answer != nil ? answer.find('DeviceName', "") : ""
    if name == "" || name == "Newmota"
      answer = newmota.cmd("Status 5")
      var network = answer != nil ? answer.find('StatusNET') : nil
      var hostname = network != nil ? network.find('Hostname', "") : ""
      name = hostname != "" ? hostname : "Newmota"
    end
    return size(name) > 32 ? name[0..31] : name
  end

  # The verifier takes a noticeable time, so it is computed once and stored with the code
  def load_identity()
    import crypto
    var fixed = matter.build_passcode()
    var fixed_discriminator = matter.build_discriminator()
    var stored = matter.store_load("identity")
    var v = stored != nil ? matter.tlv(stored) : nil
    # A code fixed by the build replaces the stored one. Fabrics do not depend on it.
    if !isinstance(v, map) || (fixed != nil && (v[2] != fixed || v[1] != fixed_discriminator))
      var passcode = fixed
      while passcode == nil
        passcode = crypto.random(4).get(0, 4) & 0x7FFFFFF
        if passcode > 99999998 || self.INVALID_PASSCODES.find(passcode) != nil
          passcode = nil
        end
      end
      var salt = crypto.random(16)
      var tv = crypto.PBKDF2_HMAC_SHA256().derive(bytes().add(passcode, 4), salt, self.PBKDF_ITERATIONS, 80)
      var ec = crypto.EC_P256()
      var w = matter.Tlv().st(nil)
      w.uint(1, fixed != nil ? fixed_discriminator : crypto.random(2).get(0, 2) & 0xFFF)
      w.uint(2, passcode)
      w.bin(3, salt)
      w.bin(4, ec.mod(tv[0..39]))
      w.bin(5, ec.public_key(ec.mod(tv[40..79])))
      w.close()
      matter.store_save("identity", w.b)
      v = matter.tlv(w.b)
    end
    self.discriminator = v[1]
    self.passcode = v[2]
    self.salt = v[3]
    self.w0 = v[4]
    self.L = v[5]
  end

  def load_fabrics()
    for index : range(1, self.MAX_FABRICS)
      var stored = matter.store_load("fabric" + str(index))
      var v = stored != nil ? matter.tlv(stored) : nil
      if isinstance(v, map)
        self.fabrics.push(matter.Fabric().decode(v))
      end
    end
  end

  # Stores the CASE sessions and subscriptions of a fabric, so controllers carry on after a restart
  def save_sessions(f)
    if f == self.pending    return    end
    var w = matter.Tlv().st(nil)
    var order = []
    w.arr(1)
    for session : self.sessions
      if session.fabric != f    continue    end
      var record = matter.session_export(session.ctx)
      if record == nil    continue    end
      order.push(session)
      var cats = bytes()
      for cat : session.cats    cats.add(cat, 4)    end
      w.st(nil)
      w.bin(1, record)
      w.bin(2, session.challenge)
      w.bin(3, session.peer_node)
      w.bin(4, cats)
      w.close()
    end
    w.close()
    w.arr(2)
    for sub : self.im.subs
      var position = order.find(sub.session)
      if position == nil || !sub.active   continue    end
      w.st(nil)
      w.uint(1, position)
      w.uint(2, sub.id)
      w.uint(3, sub.floor_ms / 1000)
      w.uint(4, sub.ceiling_ms / 1000)
      w.bool(5, sub.filtered == true)
      w.bin(6, self.im.pack_paths(sub.paths, 2))
      if sub.event_paths != nil   w.bin(7, self.im.pack_paths(sub.event_paths, 1))    end
      w.uint(8, sub.event_min)
      w.close()
    end
    w.close()
    w.close()
    if !matter.store_save("sessions" + str(f.index), w.b)
      log("MTR: Saving sessions of fabric " + str(f.index) + " failed", 1)
    end
  end

  def load_sessions(f)
    var stored = matter.store_load("sessions" + str(f.index))
    var v = stored != nil ? matter.tlv(stored) : nil
    if !isinstance(v, map)    return    end
    var order = []
    for s : v.find(1, [])
      var ctx = matter.session_import(s[1])
      var session = nil
      if ctx >= 0
        session = matter.Session(ctx, s[2], f)
        session.peer_node = s[3]
        session.cats = []
        var cats = s[4]
        for offset : range(0, size(cats) / 4 - 1)
          session.cats.push(cats.get(offset * 4, 4))
        end
        self.sessions[ctx & 0xFF] = session
      end
      order.push(session)
    end
    for r : v.find(2, [])
      var session = order[r[1]]
      if session == nil   continue    end
      var event_paths = r.find(7)
      var sub = matter.Subscription(r[2], session, self.im.unpack_paths(r[6], 2), r[3], r[4], matter.now())
      sub.filtered = r[5]
      sub.event_paths = event_paths != nil ? self.im.unpack_paths(event_paths, 1) : nil
      sub.event_min = r[8]
      sub.active = true
      sub.resuming = true
      self.im.subs.push(sub)
    end
  end

  def save_fabric(f)
    if f != self.pending && !self.updating(f)
      matter.store_save("fabric" + str(f.index), f.encode())
    end
  end

  def open_basic_window(seconds)
    self.open_window(seconds, self.PBKDF_ITERATIONS, self.discriminator, self.salt, self.w0, self.L, nil)
  end

  def open_window(seconds, iterations, discriminator, salt, w0, L, admin)
    import newmota
    self.window = {'iterations': iterations, 'discriminator': discriminator, 'salt': salt,
                   'w0': w0, 'L': L, 'admin': admin}
    self.window_deadline = newmota.millis() + seconds * 1000
    self.changed(0, 0x003C, 0)
    self.publish()
  end

  def close_window()
    self.window = nil
    self.window_deadline = nil
    matter.ble_stop()
    self.changed(0, 0x003C, 0)
    self.publish()
  end

  # The restart count that General Diagnostics reports, within its 16 bits
  static def read_boot_count()
    import newmota
    var answer = newmota.cmd("Status 1")
    var parameters = answer != nil ? answer.find('StatusPRM') : nil
    var count = parameters != nil ? parameters.find('BootCount', 0) : 0
    return count > 0xFFFF ? 0xFFFF : count
  end

  # Shows the state on the Home page. The pairing code appears only while a window is open.
  def update_card()
    try
      import widget
      var committed = size(self.fabrics) - (self.pending != nil ? 1 : 0)
      var html = '<div><h3>Matter</h3><p class="label">Fabrics</p><p class="value" data-v="fabrics"></p>'
      var data = {'fabrics': committed}
      if self.window != nil
        html += '<p class="label">Pairing code</p><p class="value" data-v="code"></p>'
                '<div data-chart="qr" data-v="qr"></div><p class="small" data-v="qr"></p>'
        data['code'] = self.pairing_code()
        data['qr'] = self.qr_code()
      else
        html += '<button data-act="open">Open commissioning window</button>'
      end
      if size(self.fabrics) > 0
        html += self.reset_armed ? '<button data-act="reset">Really remove all fabrics?</button>'
                                 : '<button data-act="reset">Remove all fabrics</button>'
      end
      widget.set("matter", html + '</div>')
      widget.data("matter", data)
      widget.on("matter", / name, value -> self.card_action(name))
    except ..
    end
  end

  # Removing every controller cannot be undone, so that button needs a second press
  def card_action(part)
    if part == "reset" && !self.reset_armed
      self.reset_armed = true
      self.update_card()
      return
    end
    self.reset_armed = false
    if part == "reset"
      self.remove_all_fabrics()
    elif part == "open"
      self.open_basic_window(self.WINDOW_SECONDS)
    else
      self.update_card()
    end
  end

  def remove_all_fabrics()
    for f : self.fabrics.copy()
      self.remove_fabric(f)
    end
    if self.window == nil    self.open_basic_window(self.WINDOW_SECONDS)    end
  end

  # Mtr commands: answers with the JSON for the console
  def command(name, payload)
    import json
    if name == "MtrJoin"
      if payload == "1"       self.open_basic_window(self.WINDOW_SECONDS)
      elif payload == "0"     self.close_window()
      elif payload != ""      return nil
      end
      return json.dump({name: self.window != nil ? 1 : 0})
    elif name == "MtrReset"
      if payload != "1"   return nil    end
      self.remove_all_fabrics()
      return json.dump({name: "Done"})
    end
    var fabrics = []
    for f : self.fabrics
      if f != self.pending    fabrics.push(f.index)   end
    end
    var endpoints = []
    for e : self.endpoints    endpoints.push([e.id, e.device_types[0][0]])    end
    var info = {'Fabrics': fabrics, 'Endpoints': endpoints, 'Window': self.window != nil ? 1 : 0,
                'Enforce': self.enforce_access ? 1 : 0}
    if self.window != nil
      info['PairingCode'] = self.pairing_code()
      info['QRCode'] = self.qr_code()
    end
    return json.dump({name: info})
  end

  # Replaces the published records with the open window and every fabric
  def publish()
    import crypto
    self.update_card()
    matter.mdns_clear()
    var transports = matter.tcp_supported() ? "6" : "0"     # TCP client and server bits
    var window = self.window
    if window != nil
      var long = window['discriminator']
      var mode = window.find('admin') != nil ? 2 : 1
      matter.mdns_add(crypto.random(8).tohex(), true,
        "_L" + str(long) + ",_S" + str(long >> 8) + ",_V" + str(self.VENDOR_ID) + ",_CM",
        "D=" + str(long) + "\nCM=" + str(mode) + "\nVP=" + str(self.VENDOR_ID) + "+" + str(self.PRODUCT_ID) +
        "\nT=" + transports + "\nSII=500\nSAI=300\nDN=" + self.display_name())
    end
    for f : self.fabrics
      var compressed = f.compressed.tohex()
      matter.mdns_add(compressed + "-" + f.node_id.copy().reverse().tohex(), false, "_I" + compressed,
        "SII=500\nSAI=300\nT=" + transports)
    end
    matter.mdns_commit()
  end

  def pairing_code()
    var digit1 = (self.discriminator & 0x0FFF) >> 10
    var digits2 = ((self.discriminator & 0x0300) << 6) | (self.passcode & 0x3FFF)
    var digits3 = self.passcode >> 14
    var code = format("%1i%05i%04i", digit1, digits2, digits3)
    return code + self.verhoeff(code)
  end

  # Check digit of the manual pairing code
  static def verhoeff(digits)
    var d = "0123456789123406789523401789563401289567401239567859876043216598710432765982104387659321049876543210"
    var p = "01234567891576283094580379614289160435279453126870428657390127938064157046913258"
    var inv = "0432156789"
    var c = 0
    var n = size(digits)
    for i : range(0, n - 1)
      var digit = int(digits[n - 1 - i])
      var mixed = int(p[((i + 1) % 8) * 10 + digit])
      c = int(d[c * 10 + mixed])
    end
    return inv[c]
  end

  def qr_code()
    var raw = bytes().resize(11)
    raw.setbits(3, 16, self.VENDOR_ID)
    raw.setbits(19, 16, self.PRODUCT_ID)
    raw.setbits(37, 8, matter.ble_supported() ? 0x06 : 0x04)
    raw.setbits(45, 12, self.discriminator & 0xFFF)
    raw.setbits(57, 27, self.passcode & 0x7FFFFFF)
    return "MT:" + matter.Base38.encode(raw)
  end

  def add_session(from_ctx, local_id, peer_id, i2r, r2i, challenge, peer_node, local_node, fabric)
    var ctx = matter.session(from_ctx, local_id, peer_id, i2r, r2i, peer_node, local_node, fabric != nil ? fabric.index : 0)
    if ctx < 0    return nil    end
    var session = matter.Session(ctx, challenge, fabric)
    session.peer_node = peer_node
    session.cats = []
    self.sessions[ctx & 0xFF] = session
    return session
  end

  # True when a certificate was signed by the holder of a public key
  static def signed_by(certificate, key)
    import crypto
    var data = matter.cert_tbs(certificate)
    var parsed = matter.tlv(certificate)
    if data == nil || !isinstance(parsed, map) || key == nil    return false    end
    return crypto.EC_P256().ecdsa_verify_sha256(key, data, parsed.find(11))
  end

  # Validates NOC -> optional ICAC -> root. Trust in the root comes from AddTrustedRootCertificate.
  def chain_valid(noc, icac, root)
    var root_key = matter.tlv(root)[9]
    var icac_certificate = (icac != nil && size(icac) > 0) ? icac : nil
    if icac_certificate == nil
      return self.signed_by(noc, root_key)
    end
    var parsed = matter.tlv(icac_certificate)
    var extensions = isinstance(parsed, map) ? parsed.find(10) : nil
    var basic = isinstance(extensions, map) ? extensions.find(1) : nil
    if !isinstance(basic, map) || !basic.find(1, false)   return false    end
    return self.signed_by(icac_certificate, root_key) && self.signed_by(noc, parsed[9])
  end

  # Reports a chain failure, and refuses it only once enforcement is on
  def chain_accepted(noc, icac, root)
    if self.chain_valid(noc, icac, root)    return true   end
    log("MTR: NOC chain validation failed" + (self.enforce_access ? "" : ", accepted because enforcement is off"), 2)
    return !self.enforce_access
  end

  # Access enforcement stays off until real controllers have passed without warnings
  def set_enforce_access(value)
    self.enforce_access = value
    if value
      matter.store_save("enforce", bytes("01"))
    else
      matter.store_remove("enforce")
    end
  end

  def find_session(ctx)
    var s = self.sessions.find(ctx & 0xFF)
    return (s != nil && s.ctx == ctx) ? s : nil
  end

  def drop_handshake(ctx)
    self.handshakes.remove(ctx & 0xFF)
  end

  def arm_failsafe(seconds, session)
    import newmota
    self.failsafe_deadline = seconds > 0 ? newmota.millis() + seconds * 1000 : nil
    if seconds == 0   self.failsafe_expired()   end
  end

  # Undoes everything a commissioner did since arming
  def failsafe_expired()
    import newmota
    self.failsafe_deadline = nil
    # A tested network is live but unsaved: a restart returns to the stored one
    if self.wifi_tested
      newmota.cmd("Restart 1")
      return
    end
    if self.update_backup != nil
      var b = self.update_backup
      self.update_backup = nil
      var updated = b[0]
      updated.noc = b[1]
      updated.icac = b[2]
      updated.key = b[3]
      updated.node_id = b[4]
      self.publish()
    end
    self.pending_for_update = nil
    self.wifi_ssid = nil
    self.wifi_password = nil
    self.pending_root = nil
    self.pending_key = nil
    self.breadcrumb = 0
    var f = self.pending
    if f != nil
      self.pending = nil
      self.fabrics.remove(self.fabrics.find(f))
      matter.fabric_remove(f.index)
      if size(self.fabrics) == 0 && self.window == nil
        self.open_basic_window(self.WINDOW_SECONDS)
      else
        self.publish()
      end
    end
  end

  # AddNOC: the fabric serves CASE at once and persists only at CommissioningComplete
  def add_fabric(session, fields)
    import crypto
    if self.pending_root == nil || self.pending_key == nil || self.pending != nil || self.pending_for_update
      return [4, 0]                 # MissingCsr
    end
    if size(self.fabrics) >= self.MAX_FABRICS
      return [5, 0]                 # TableFull
    end
    var noc = fields.find(0)
    var parsed = noc != nil ? matter.tlv(noc) : nil
    if !isinstance(parsed, map) || !isinstance(parsed.find(6), map)
      return [3, 0]                 # InvalidNOC
    end
    if parsed.find(9) != crypto.EC_P256().public_key(self.pending_key)
      return [1, 0]                 # InvalidPublicKey
    end
    var used = {}
    for f : self.fabrics    used[f.index] = true    end
    var index = 1
    while used.contains(index)    index += 1    end
    var f = matter.Fabric()
    f.index = index
    f.rcac = self.pending_root
    f.noc = noc
    var icac = fields.find(1)
    f.icac = (icac != nil && size(icac) > 0) ? icac : nil
    f.ipk = fields.find(2)
    f.vendor = fields.find(4, 0)
    f.key = self.pending_key
    f.label = ""
    f.fabric_id = f.id64(parsed[6].find(21))
    f.node_id = f.id64(parsed[6].find(17))
    # The commissioner's implicit administrator entry, until it writes its own list
    var subject = fields.find(3)
    f.acl = [{1: 5, 2: 2, 3: subject != nil ? [subject] : nil, 4: nil}]
    if !self.chain_accepted(noc, f.icac, f.rcac)
      return [3, 0]                 # InvalidNOC
    end
    var root_pub = f.root_pub()
    f.compressed = crypto.HKDF_SHA256().derive(root_pub[1..], f.fabric_id.copy().reverse(),
      bytes().fromstring("CompressedFabric"), 8)
    self.pending = f
    self.pending_root = nil
    self.pending_key = nil
    self.fabrics.push(f)
    if self.window != nil && self.window.find('admin') == nil
      self.window = nil
      self.window_deadline = nil
    end
    self.publish()
    return [0, index]
  end

  # A commissioner may send one AddNOC or UpdateNOC per fail-safe
  def noc_invoked()
    return self.pending != nil || self.update_backup != nil
  end

  def updating(f)
    return self.update_backup != nil && self.update_backup[0] == f
  end

  # UpdateNOC: new credentials for a fabric, undone by the fail-safe until CommissioningComplete
  def update_fabric(session, fields)
    import crypto
    if self.failsafe_deadline == nil    return 0xCA   end
    if self.noc_invoked()   return 0x87   end
    if self.pending_key == nil    return [4, 0]     end     # MissingCsr
    if !self.pending_for_update   return 0x87   end
    var f = session.fabric
    if f == nil   return 0x7E   end
    var noc = fields.find(0)
    var parsed = noc != nil ? matter.tlv(noc) : nil
    if !isinstance(parsed, map) || !isinstance(parsed.find(6), map)
      return [3, f.index]           # InvalidNOC
    end
    if parsed.find(9) != crypto.EC_P256().public_key(self.pending_key)
      return [1, f.index]           # InvalidPublicKey
    end
    if f.id64(parsed[6].find(21)) != f.fabric_id    return [3, f.index]   end
    var icac = fields.find(1)
    icac = (icac != nil && size(icac) > 0) ? icac : nil
    if !self.chain_accepted(noc, icac, f.rcac)    return [3, f.index]   end
    self.update_backup = [f, f.noc, f.icac, f.key, f.node_id]
    f.noc = noc
    f.icac = icac
    f.key = self.pending_key
    f.node_id = f.id64(parsed[6].find(17))
    self.pending_key = nil
    self.pending_for_update = nil
    # The fabric's other sessions hold the old identity, so they end; this one stays to complete
    for other : self.sessions
      if other.fabric == f && other != session
        self.im.drop_session_subscriptions(other.ctx)
        matter.session_remove(other.ctx)
      end
    end
    self.publish()
    self.changed(0, 0x003E, 0)
    return [0, f.index]
  end

  def complete_update()
    var f = self.update_backup[0]
    if !matter.store_save("fabric" + str(f.index), f.encode())
      return false
    end
    self.update_backup = nil
    self.failsafe_deadline = nil
    self.breadcrumb = 0
    self.save_sessions(f)
    log("MTR: Fabric " + str(f.index) + " updated", 2)
    return true
  end

  # A fabric that could not be saved stays pending, so the fail-safe rolls it back
  def complete_fabric()
    var f = self.pending
    if !matter.store_save("fabric" + str(f.index), f.encode())
      return false
    end
    if self.wifi_ssid != nil && !matter.wifi_commit(self.wifi_ssid, self.wifi_password)
      matter.store_remove("fabric" + str(f.index))
      return false
    end
    self.wifi_ssid = nil
    self.wifi_password = nil
    self.wifi_tested = false
    self.pending = nil
    self.failsafe_deadline = nil
    self.breadcrumb = 0
    self.close_window()
    self.changed(0, 0x003E, 1)
    self.save_sessions(f)
    log("MTR: Fabric " + str(f.index) + " commissioned", 2)
    return true
  end

  # The answer travels on a session of that fabric, so removal waits for it
  def schedule_fabric_removal(index)
    import newmota
    for f : self.fabrics
      if f.index == index
        self.removals.push([f, newmota.millis() + 2000])
        return true
      end
    end
    return false
  end

  def remove_fabric(f)
    # A fabric still being commissioned is undone with everything else since arming
    if f == self.pending
      self.failsafe_expired()
      return
    end
    for session : self.sessions
      if session.fabric == f    self.im.drop_session_subscriptions(session.ctx)   end
    end
    matter.store_remove("fabric" + str(f.index))
    matter.store_remove("sessions" + str(f.index))
    matter.fabric_remove(f.index)
    self.fabrics.remove(self.fabrics.find(f))
    log("MTR: Fabric " + str(f.index) + " removed", 2)
    if size(self.fabrics) == 0
      self.open_basic_window(self.WINDOW_SECONDS)
    else
      self.publish()
    end
  end

  # Native entry point for each delivered message
  def message(ctx, exchange, peer_initiator, protocol, opcode, payload)
    if protocol == 0xFFFF
      self.im.delivery_failed(ctx)
      if self.client != nil   self.client.failed(ctx)   end
    elif protocol == 0
      self.secure_channel(ctx, exchange, opcode, payload)
    elif protocol == 1
      var session = self.find_session(ctx)
      if session != nil
        self.im.message(session, exchange, peer_initiator, opcode, payload)
      end
    end
  end

  def secure_channel(ctx, exchange, opcode, payload)
    var session = self.find_session(ctx)
    if session != nil
      # General 0, protocol 0, code 3 asks to close the session
      if opcode == 0x40 && size(payload) >= 8 && payload.get(6, 2) == 3
        self.im.drop_session_subscriptions(ctx)
        self.sessions.remove(ctx & 0xFF)
        matter.session_remove(ctx)
        if session.fabric != nil    self.save_sessions(session.fabric)    end
      end
      return
    end
    var slot = ctx & 0xFF
    var handshake = self.handshakes.find(slot)
    # Sigma1 and PBKDFParamRequest start over; a newer peer in a slot replaces the older one
    if handshake == nil || handshake.ctx != ctx || opcode == 0x20 || opcode == 0x30
      handshake = matter.Handshake(self, ctx)
      self.handshakes[slot] = handshake
    end
    handshake.message(exchange, opcode, payload)
  end

  # Lists a cluster for motion_tick; it leaves the list by answering false
  def start_motion(cluster)
    if self.moving.find(cluster) == nil   self.moving.push(cluster)   end
  end

  # Native entry point four times a second
  def tick(now)
    var index = size(self.moving) - 1
    while index >= 0
      if !self.moving[index].motion_tick(self, now)   self.moving.remove(index)   end
      index -= 1
    end
    if self.window_deadline != nil && (now - self.window_deadline) >= 0
      self.close_window()
    end
    if self.failsafe_deadline != nil && (now - self.failsafe_deadline) >= 0
      self.failsafe_expired()
    end
    if size(self.removals) > 0 && (now - self.removals[0][1]) >= 0
      var f = self.removals[0][0]
      self.removals.remove(0)
      self.remove_fabric(f)
    end
    var outputs = matter.output_revision()
    if outputs != self.seen_outputs || self.follow_due
      self.seen_outputs = outputs
      self.follow_due = false
      for e : self.endpoints
        for c : e.clusters
          c.follow(self)
        end
      end
    end
    if (now - self.next_poll) >= 0
      self.next_poll = now + 1000
      for e : self.endpoints
        for c : e.clusters
          c.poll(self)
        end
      end
    end
    if self.bridge != nil   self.bridge.poll()    end
    if self.client != nil   self.client.tick(now)   end
    var dirty = matter.sessions_dirty()
    if dirty != 0
      for f : self.fabrics
        if (dirty & (1 << f.index)) != 0    self.save_sessions(f)   end
      end
    end
    # At most every 2 s, because a noisy sensor changes with each reading and Status 10 is not free
    var sensors = matter.sensor_revision()
    if size(self.measurements) > 0 && sensors != self.seen_sensors && (now - self.next_sensors) >= 0
      self.seen_sensors = sensors
      self.next_sensors = now + 2000
      for v : self.sensor_values()
        var m = self.measurements.find(v[0])
        if m != nil   m.set(self, v[2])   end
      end
    end
    self.im.tick(now)
  end
end
