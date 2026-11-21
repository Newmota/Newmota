# Data model: one small class per cluster. Protocol values come from Tasmota's
# Matter_Plugin_1_Root.be and Matter_Plugin_2_Light0.be; their if-chain is split by cluster.
# Adapted from Tasmota lib/libesp32/berry_matter/src/embedded/Matter_Plugin_1_Root.be
# Source pin: f27e4077212dd9ae8e3ada3fa96b6040926e059b.
# Copyright (C) 2023  Stephan Hadinger & Theo Arends
# SPDX-License-Identifier: GPL-3.0-or-later

#@ solidify:Matter_Motion,weak
#@ solidify:Matter_Cluster,weak
#@ solidify:Matter_Descriptor,weak
#@ solidify:Matter_AccessControl,weak
#@ solidify:Matter_BasicInformation,weak
#@ solidify:Matter_GeneralCommissioning,weak
#@ solidify:Matter_NetworkCommissioning,weak
#@ solidify:Matter_GeneralDiagnostics,weak
#@ solidify:Matter_AdminCommissioning,weak
#@ solidify:Matter_OperationalCredentials,weak
#@ solidify:Matter_GroupKeys,weak
#@ solidify:Matter_Identify,weak
#@ solidify:Matter_Groups,weak
#@ solidify:Matter_RelayOutput,weak
#@ solidify:Matter_VirtualOutput,weak
#@ solidify:Matter_LocalLamp,weak
#@ solidify:Matter_OnOff,weak
#@ solidify:Matter_LevelControl,weak
#@ solidify:Matter_ColorControl,weak
#@ solidify:Matter_Measurement,weak

# One value moving at a rate, up to its limits or over a distance. Rates count units per second.
class Matter_Motion
  var value, rate, low, high, wraps, remaining

  def init(value, rate, low, high, wraps, distance)
    self.value = real(value)
    self.rate = real(rate)
    self.low = low
    self.high = high
    self.wraps = wraps
    self.remaining = distance
  end

  # Advances by some seconds and reports whether it is still moving
  def advance(seconds)
    var delta = self.rate * seconds
    var moving = true
    if self.remaining != nil
      var length = delta < 0 ? -delta : delta
      if length >= self.remaining
        delta = self.rate < 0 ? -self.remaining : self.remaining
        moving = false
      else
        self.remaining -= length
      end
    end
    self.value += delta
    if self.wraps
      var span = self.high - self.low + 1
      while self.value > self.high    self.value -= span    end
      while self.value < self.low     self.value += span    end
    elif self.value >= self.high
      self.value = real(self.high)
      moving = false
    elif self.value <= self.low
      self.value = real(self.low)
      moving = false
    end
    return moving
  end

  # Tenths of a second until it stops, for RemainingTime. 0xFFFF means it does not stop by itself.
  def tenths_left()
    var length = self.remaining
    if length == nil
      if self.wraps   return 0xFFFF   end
      length = self.rate < 0 ? self.value - self.low : self.high - self.value
    end
    var tenths = int(length * 10 / (self.rate < 0 ? -self.rate : self.rate) + 0.5)
    return tenths > 0xFFFF ? 0xFFFF : tenths
  end
end

class Matter_Cluster
  static var CLUSTER = 0
  static var ATTRIBUTES = []
  static var ACCEPTED = []
  static var GENERATED = []
  static var FEATURES = 0
  static var REVISION = 1
  static var TIMED = []
  static var GLOBALS = [0xFFF8, 0xFFF9, 0xFFFB, 0xFFFC, 0xFFFD]
  # Privileges from the Matter data model: 1 View, 3 Operate, 4 Manage, 5 Administer.
  # Maps hold only what differs from the default of each operation.
  static var READ_PRIVILEGES = {}
  static var WRITE_PRIVILEGES = {}
  static var INVOKE_PRIVILEGE = 3
  static var FIELD_LIMITS = {'h': 254, 'd': 3, 'b': 255, 'B': 255, 'T': 0xFFFE, 'w': 0xFFFF, 'x': 0xFEFF, 'X': 0xFEFF, 's': 0x7FFF, 'M': 255}

  var endpoint

  def init(endpoint)
    self.endpoint = endpoint
  end

  def id()
    return self.CLUSTER
  end

  def all_attributes()
    return self.ATTRIBUTES + self.GLOBALS
  end

  def features()
    return self.FEATURES
  end

  def revision()
    return self.REVISION
  end

  # Called once a second
  def poll(device)
  end

  # Follows state changed outside Matter; called when the device sees that something changed
  def follow(device)
  end

  # Called on each tick while the device lists this cluster as moving. False ends that.
  def motion_tick(device, now)
    return false
  end

  def read_privilege(attr)
    return self.READ_PRIVILEGES.find(attr, 1)
  end

  def write_privilege(attr)
    return self.WRITE_PRIVILEGES.find(attr, 3)
  end

  def invoke_privilege(command)
    return self.INVOKE_PRIVILEGE
  end

  def needs_timed(command)
    return self.TIMED.find(command) != nil
  end

  static def int_list(w, t, items)
    w.arr(t)
    for v : items
      w.uint(nil, v)
    end
    w.close()
  end

  # Checks fields against a spec of one letter per field, following Tasmota's cc_args. A capital
  # letter marks a field that may be missing or null. Returns a status, or nil when they are fine.
  def bad_fields(fields, spec)
    if !isinstance(fields, map)   return 0x85   end
    for i : 0 .. size(spec) - 1
      var c = spec[i]
      var v = fields.find(i)
      if v == nil && c < 'a'   v = 0   end
      if type(v) != 'int'   return 0x85   end
      if v > self.FIELD_LIMITS[c] || v < (c == 's' ? -0x8000 : 0)    return 0x87   end
    end
    return nil
  end

  # Writes the Options attribute of a lighting cluster and returns a status
  def write_options(device, value, highest)
    if type(value) != 'int' || value < 0 || value > highest   return 0x87   end
    if value != self.options
      self.options = value
      device.changed(self.endpoint.id, self.CLUSTER, 0x0F)
    end
    return 0
  end

  # Whether a lighting command runs. An off light ignores it unless ExecuteIfOff is set, by the
  # Options attribute or by the mask and override fields of the command, from field `tag` on.
  static def executes(lamp, options, fields, tag)
    if lamp.on()    return true   end
    var mask = fields.find(tag, 0)
    return (((options & ~mask) | (fields.find(tag + 1, 0) & mask)) & 1) != 0
  end

  # Reads a global attribute or asks the cluster. False means unsupported.
  def read_global(device, session, attr, w, t)
    if   attr == 0xFFF8   self.int_list(w, t, self.GENERATED)
    elif attr == 0xFFF9   self.int_list(w, t, self.ACCEPTED)
    elif attr == 0xFFFB   self.int_list(w, t, self.all_attributes())
    elif attr == 0xFFFC   w.uint(t, self.features())
    elif attr == 0xFFFD   w.uint(t, self.revision())
    else
      return self.read(device, session, attr, w, t)
    end
    return true
  end

  def read(device, session, attr, w, t)
    return false
  end

  # Returns a status: writable attributes override this
  def write(device, session, attr, value)
    return self.ATTRIBUTES.find(attr) != nil ? 0x88 : 0x86
  end

  # Returns a status, [response command, fields], or nil when answered later
  def invoke(device, session, command, fields, exchange)
    return 0x81
  end
end

class Matter_Descriptor : Matter_Cluster
  static var CLUSTER = 0x001D
  static var ATTRIBUTES = [0, 1, 2, 3]
  static var REVISION = 2

  def read(device, session, attr, w, t)
    var ep = self.endpoint
    if attr == 0
      w.arr(t)
      for d : ep.device_types
        w.st(nil)
        w.uint(0, d[0])
        w.uint(1, d[1])
        w.close()
      end
      w.close()
    elif attr == 1
      w.arr(t)
      for c : ep.clusters
        w.uint(nil, c.id())
      end
      w.close()
    elif attr == 2
      w.arr(t)
      if ep.clients != nil
        for id : ep.clients   w.uint(nil, id)   end
      end
      w.close()
    elif attr == 3
      w.arr(t)
      if ep.id == 0
        for e : device.endpoints
          if e.id != 0    w.uint(nil, e.id)    end
        end
      elif ep.parts != nil
        for id : ep.parts
          w.uint(nil, id)
        end
      end
      w.close()
    else
      return false
    end
    return true
  end
end

class Matter_AccessControl : Matter_Cluster
  static var CLUSTER = 0x001F
  static var READ_PRIVILEGES = {0: 5}
  static var WRITE_PRIVILEGES = {0: 5}
  static var ATTRIBUTES = [0, 2, 3, 4]
  static var REVISION = 2
  static var MAX_SUBJECTS = 4
  static var MAX_TARGETS = 3
  static var MAX_ENTRIES = 4

  def read(device, session, attr, w, t)
    if attr == 0
      w.arr(t)
      if session.fabric != nil
        for e : session.fabric.acl
          session.fabric.write_entry(w, e)
        end
      end
      w.close()
    elif attr == 2    w.uint(t, self.MAX_SUBJECTS)
    elif attr == 3    w.uint(t, self.MAX_TARGETS)
    elif attr == 4    w.uint(t, self.MAX_ENTRIES)
    else
      return false
    end
    return true
  end

  # Stored per fabric. Apple removes the fabric if its write is rejected, so only bounds refuse it.
  def write(device, session, attr, value)
    if attr != 0    return super(self).write(device, session, attr, value)    end
    var fabric = session.fabric
    if fabric == nil    return 0x7E   end
    var appending = session.write_path != nil && session.write_path.contains(5)
    var entries = appending ? [value] : (isinstance(value, list) ? value : [])
    var result = appending ? fabric.acl.copy() : []
    for e : entries
      if !isinstance(e, map)    return 0x87   end
      var subjects = e.find(3)
      var targets = e.find(4)
      if (subjects != nil && size(subjects) > self.MAX_SUBJECTS) || (targets != nil && size(targets) > self.MAX_TARGETS)
        return 0x89
      end
      result.push(e)
    end
    if size(result) > self.MAX_ENTRIES    return 0x89   end
    fabric.acl = result
    device.save_fabric(fabric)
    return 0
  end
end

class Matter_BasicInformation : Matter_Cluster
  static var CLUSTER = 0x0028
  static var WRITE_PRIVILEGES = {5: 4, 6: 5}
  static var ATTRIBUTES = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0x0A, 0x0F, 0x12, 0x13, 0x15, 0x16, 0x18]
  static var REVISION = 4

  var label

  def read(device, session, attr, w, t)
    if   attr == 0      w.uint(t, device.DATA_MODEL_REVISION)
    elif attr == 1      w.str(t, "Newmota")
    elif attr == 2      w.uint(t, device.VENDOR_ID)
    elif attr == 3      w.str(t, device.display_name())
    elif attr == 4      w.uint(t, device.PRODUCT_ID)
    elif attr == 5      w.str(t, self.label != nil && self.label != "" ? self.label : device.display_name())
    elif attr == 6      w.str(t, "XX")
    elif attr == 7      w.uint(t, 0)
    elif attr == 8      w.str(t, "0")
    elif attr == 9      w.uint(t, 1)
    elif attr == 0x0A   w.str(t, "1.0")
    elif attr == 0x0F   w.str(t, device.host)
    elif attr == 0x12   w.str(t, device.host)
    elif attr == 0x13
      w.st(t)
      w.uint(0, 3)              # CASE sessions per fabric
      w.uint(1, 3)              # Subscriptions per fabric
      w.close()
    elif attr == 0x15   w.uint(t, device.SPECIFICATION_VERSION)
    elif attr == 0x16   w.uint(t, 1)
    elif attr == 0x18   w.uint(t, 1)
    else
      return false
    end
    return true
  end

  def write(device, session, attr, value)
    if attr == 5
      self.label = value
      return 0
    end
    return attr == 6 ? 0 : super(self).write(device, session, attr, value)
  end
end

class Matter_GeneralCommissioning : Matter_Cluster
  static var CLUSTER = 0x0030
  static var WRITE_PRIVILEGES = {0: 5}
  static var INVOKE_PRIVILEGE = 5
  static var ATTRIBUTES = [0, 1, 2, 3, 4]
  static var ACCEPTED = [0, 2, 4]
  static var GENERATED = [1, 3, 5]

  def read(device, session, attr, w, t)
    if attr == 0      w.uint(t, device.breadcrumb)
    elif attr == 1
      w.st(t)
      w.uint(0, 60)
      w.uint(1, 900)
      w.close()
    elif attr == 2    w.uint(t, 2)
    elif attr == 3    w.uint(t, 2)
    elif attr == 4    w.bool(t, true)     # BLE stays up while Wi-Fi connects
    else
      return false
    end
    return true
  end

  def write(device, session, attr, value)
    if attr == 0
      device.breadcrumb = value
      return 0
    end
    return super(self).write(device, session, attr, value)
  end

  static def answer(command, code)
    var w = matter.Tlv().st(1)
    w.uint(0, code)
    w.str(1, "")
    w.close()
    return [command, w]
  end

  def invoke(device, session, command, fields, exchange)
    if command == 0
      device.arm_failsafe(fields.find(0, 0), session)
      device.breadcrumb = fields.find(1, 0)
      return self.answer(1, 0)
    elif command == 2
      device.breadcrumb = fields.find(2, 0)
      return self.answer(3, 0)
    elif command == 4
      # Completed only over a CASE session of the fabric being added or updated
      if session.fabric != nil && device.updating(session.fabric)
        return device.complete_update() ? self.answer(5, 0) : 0x01
      end
      if session.fabric == nil || session.fabric != device.pending
        return self.answer(5, 2)
      end
      return device.complete_fabric() ? self.answer(5, 0) : 0x01
    end
    return 0x81
  end
end

# One Wi-Fi network. Credentials stay in RAM until commissioning completes, so a failed
# commissioning leaves the stored network untouched.
class Matter_NetworkCommissioning : Matter_Cluster
  static var CLUSTER = 0x0031
  static var READ_PRIVILEGES = {0: 5, 1: 5, 5: 5, 6: 5, 7: 5}
  static var WRITE_PRIVILEGES = {4: 5}
  static var INVOKE_PRIVILEGE = 5
  static var ATTRIBUTES = [0, 1, 2, 3, 4, 5, 6, 7]
  static var ACCEPTED = [0, 2, 4, 6, 8]
  static var GENERATED = [1, 5, 7]
  static var FEATURES = 1

  var last_status, last_id
  var pending_session, pending_exchange, pending_kind, pending_filter

  # The SSID the node is on, for the Networks attribute
  def current_ssid(device)
    if device.wifi_ssid != nil    return device.wifi_ssid   end
    import newmota
    var state = newmota.cmd("Status 11")
    state = state != nil ? state.find('StatusSTS') : nil
    var wifi = state != nil ? state.find('Wifi') : nil
    var ssid = wifi != nil ? wifi.find('SSId') : nil
    return ssid != nil ? bytes().fromstring(ssid) : nil
  end

  def read(device, session, attr, w, t)
    if attr == 0      w.uint(t, 1)
    elif attr == 1
      w.arr(t)
      var ssid = self.current_ssid(device)
      if ssid != nil
        w.st(nil)
        w.bin(0, ssid)
        w.bool(1, matter.wifi_connected())
        w.close()
      end
      w.close()
    elif attr == 2    w.uint(t, 10)
    elif attr == 3    w.uint(t, 30)
    elif attr == 4    w.bool(t, true)
    elif attr == 5
      if self.last_status != nil    w.uint(t, self.last_status)   else w.null(t)    end
    elif attr == 6
      if self.last_id != nil        w.bin(t, self.last_id)        else w.null(t)    end
    elif attr == 7    w.null(t)
    else
      return false
    end
    return true
  end

  # Status with the debug text every response carries
  static def config_answer(status)
    var w = matter.Tlv().st(1)
    w.uint(0, status)
    w.str(1, "")
    w.uint(2, 0)
    w.close()
    return [5, w]
  end

  def invoke(device, session, command, fields, exchange)
    if self.ACCEPTED.find(command) == nil    return 0x81   end
    if device.failsafe_deadline == nil && command != 0
      return 0xCA                     # FAILSAFE_REQUIRED
    end
    if command == 0
      device.breadcrumb = fields.find(1, device.breadcrumb)
      if self.pending_kind != nil || !matter.wifi_scan_start()
        var w = matter.Tlv().st(1)
        w.uint(0, 12)
        w.close()
        return [1, w]
      end
      self.pending_filter = fields.find(0)
      return self.defer(session, exchange, 'scan')
    elif command == 2
      device.wifi_ssid = fields.find(0)
      device.wifi_password = fields.find(1, bytes())
      device.breadcrumb = fields.find(2, device.breadcrumb)
      return self.config_answer(0)
    elif command == 4
      device.breadcrumb = fields.find(1, device.breadcrumb)
      if device.wifi_ssid == nil || device.wifi_ssid != fields.find(0)
        return self.config_answer(3)
      end
      device.wifi_ssid = nil
      device.wifi_password = nil
      return self.config_answer(0)
    elif command == 6
      device.breadcrumb = fields.find(1, device.breadcrumb)
      self.last_id = fields.find(0)
      if device.wifi_ssid == nil || device.wifi_ssid != self.last_id
        return self.connect_answer(3)
      end
      if self.pending_kind != nil || !matter.wifi_test(device.wifi_ssid, device.wifi_password)
        return self.connect_answer(12)
      end
      device.wifi_tested = true
      return self.defer(session, exchange, 'connect')
    else
      device.breadcrumb = fields.find(2, device.breadcrumb)
      return self.config_answer(device.wifi_ssid != nil && device.wifi_ssid == fields.find(0) ? 0 : 3)
    end
  end

  def defer(session, exchange, kind)
    self.pending_session = session
    self.pending_exchange = exchange
    self.pending_kind = kind
    return nil
  end

  def connect_answer(status)
    self.last_status = status
    var w = matter.Tlv().st(1)
    w.uint(0, status)
    w.str(1, "")
    w.null(2)
    w.close()
    return [7, w]
  end

  # Finishes a deferred scan or connect as soon as the radio has an answer
  def poll(device)
    if self.pending_kind == nil   return    end
    var answer
    if self.pending_kind == 'scan'
      var results = matter.wifi_scan_results()
      if results == nil   return    end
      var w = matter.Tlv().st(1)
      w.uint(0, 0)
      w.str(1, "")
      w.arr(2)
      for r : results
        if self.pending_filter != nil && r[1] != self.pending_filter    continue    end
        w.st(nil)
        w.uint(0, r[0])
        w.bin(1, r[1])
        w.bin(2, r[2])
        w.uint(3, r[3])
        w.uint(4, 0)
        w.int(5, r[4])
        w.close()
      end
      w.close()
      w.close()
      answer = [1, w]
    else
      var state = matter.wifi_test_state()
      if state == 1   return    end
      answer = self.connect_answer(state == 2 ? 0 : (matter.wifi_auth_failed() ? 7 : 9))
    end
    device.im.deferred_response(self.pending_session, self.pending_exchange, self.endpoint.id, self.CLUSTER, answer)
    self.pending_kind = nil
    self.pending_session = nil
  end
end

class Matter_GeneralDiagnostics : Matter_Cluster
  static var CLUSTER = 0x0033
  static var ATTRIBUTES = [0, 1, 2, 8]
  static var ACCEPTED = [0, 1]
  static var GENERATED = [2]

  # TestEventTrigger needs Manage; TimeSnapshot keeps the default
  def invoke_privilege(command)
    return command == 0 ? 4 : self.INVOKE_PRIVILEGE
  end

  def invoke(device, session, command, fields, exchange)
    import newmota
    if command == 0     # Test triggers stay disabled, so no enable key can match
      return 0x87
    elif command == 1
      var w = matter.Tlv().st(1)
      w.u64(0, matter.Fabric.id64(newmota.millis()))
      w.null(1)
      w.close()
      return [2, w]
    end
    return 0x81
  end

  # The IPv4 address the station reports, as four bytes
  static def ipv4_bytes()
    import newmota
    import string
    var wifi = newmota.wifi()
    var ip = wifi != nil ? wifi.find('ip') : nil
    var result = bytes()
    if ip != nil
      for part : string.split(ip, ".")
        result.add(int(part), 1)
      end
    end
    return size(result) == 4 ? result : nil
  end

  def read(device, session, attr, w, t)
    import newmota
    if attr == 0
      w.arr(t)
      w.st(nil)
      w.str(0, "wifi")
      w.bool(1, matter.wifi_connected())
      w.null(2)
      w.null(3)
      w.bin(4, bytes().fromhex(device.host))
      w.arr(5)
      var ipv4 = self.ipv4_bytes()
      if ipv4 != nil    w.bin(nil, ipv4)    end
      w.close()
      w.arr(6)
      for address : matter.ipv6_addresses()
        w.bin(nil, address)
      end
      w.close()
      w.uint(7, 1)
      w.close()
      w.close()
    elif attr == 1    w.uint(t, device.boot_count)
    elif attr == 2    w.uint(t, newmota.millis() / 1000)
    elif attr == 8    w.bool(t, false)
    else
      return false
    end
    return true
  end
end

class Matter_AdminCommissioning : Matter_Cluster
  static var CLUSTER = 0x003C
  static var INVOKE_PRIVILEGE = 5
  static var ATTRIBUTES = [0, 1, 2]
  static var ACCEPTED = [0, 1, 2]
  static var TIMED = [0, 1, 2]

  def read(device, session, attr, w, t)
    var window = device.window
    var admin = window != nil ? window.find('admin') : nil
    if attr == 0      w.uint(t, window == nil ? 0 : (admin != nil ? 1 : 2))
    elif attr == 1
      if admin != nil   w.uint(t, admin.index)    else w.null(t)    end
    elif attr == 2
      if admin != nil   w.uint(t, admin.vendor)   else w.null(t)    end
    else
      return false
    end
    return true
  end

  def invoke(device, session, command, fields, exchange)
    if command == 0
      var verifier = fields.find(1)
      var salt = fields.find(4)
      if verifier == nil || size(verifier) != 97 || salt == nil || size(salt) < 16 || size(salt) > 32
        return 0x87
      end
      device.open_window(fields.find(0, 900), fields.find(3), fields.find(2), salt, verifier[0..31],
        verifier[32..], session.fabric)
      return 0
    elif command == 1
      device.open_basic_window(fields.find(0, 900))
      return 0
    elif command == 2
      device.close_window()
      return 0
    end
    return 0x81
  end
end

class Matter_OperationalCredentials : Matter_Cluster
  static var CLUSTER = 0x003E
  static var READ_PRIVILEGES = {0: 5}
  static var INVOKE_PRIVILEGE = 5
  static var ATTRIBUTES = [0, 1, 2, 3, 4, 5]
  static var ACCEPTED = [0, 2, 4, 6, 7, 9, 0x0A, 0x0B]
  static var GENERATED = [1, 3, 5, 8]

  def read(device, session, attr, w, t)
    if attr == 0
      # Fabric scoped and sensitive: other fabrics show only their index, if at all
      w.arr(t)
      for f : device.fabrics
        if f != session.fabric && session.filtered    continue    end
        w.st(nil)
        if f == session.fabric
          w.bin(1, f.noc)
          if f.icac   w.bin(2, f.icac)    else w.null(2)    end
        end
        w.uint(0xFE, f.index)
        w.close()
      end
      w.close()
    elif attr == 1
      w.arr(t)
      for f : device.fabrics
        if f != session.fabric && session.filtered    continue    end
        w.st(nil)
        w.bin(1, f.root_pub())
        w.uint(2, f.vendor)
        w.u64(3, f.fabric_id)
        w.u64(4, f.node_id)
        w.str(5, f.label)
        w.uint(0xFE, f.index)
        w.close()
      end
      w.close()
    elif attr == 2    w.uint(t, device.MAX_FABRICS)
    elif attr == 3    w.uint(t, size(device.fabrics))
    elif attr == 4
      w.arr(t)
      for f : device.fabrics
        w.bin(nil, f.rcac)
      end
      w.close()
    elif attr == 5    w.uint(t, session.fabric != nil ? session.fabric.index : 0)
    else
      return false
    end
    return true
  end

  static def noc_answer(status, index)
    var w = matter.Tlv().st(1)
    w.uint(0, status)
    w.uint(1, index)
    w.close()
    return [8, w]
  end

  # Certificate signing request, built as in Tasmota's Matter_Session.be
  static def csr(key)
    import crypto
    var ec = crypto.EC_P256()
    var seg1 = bytes("3070020100300E310C300A060355040A0C034353523059301306072A8648CE3D020106082A8648CE3D030107034200")
    seg1 .. ec.public_key(key)
    seg1 .. bytes("A000")
    var seg2 = bytes("300C06082A8648CE3D0403020500")
    var signature = ec.ecdsa_sign_sha256_asn1(key, seg1)
    var seg3 = bytes(128)
    seg3.add(0x03, 1)
    seg3.add(size(signature) + 1, 1)
    seg3.add(0x00, 1)
    seg3 .. signature
    var result = bytes(208)
    result.add(0x30, 1)
    result.add(0x81, 1)
    result.add((size(seg1) + size(seg2) + size(seg3)) & 0xFF, 1)
    result .. seg1
    result .. seg2
    result .. seg3
    return result
  end

  def invoke(device, session, command, fields, exchange)
    import crypto
    var ec = crypto.EC_P256()
    if command == 0                     # AttestationRequest
      var nonce = fields.find(0)
      if nonce == nil || size(nonce) != 32   return 0x87   end
      var elements = matter.Tlv().st(nil)
      elements.bin(1, matter.Certs.CD_FFF1_8000)
      elements.bin(2, nonce)
      elements.uint(3, 0)
      elements.close()
      var w = matter.Tlv().st(1)
      w.bin(0, elements.b)
      w.bin(1, ec.ecdsa_sign_sha256(matter.Certs.DAC_Priv_FFF1_8000, elements.b + session.challenge))
      w.close()
      return [1, w]
    elif command == 2                   # CertificateChainRequest
      var kind = fields.find(0)
      if kind != 1 && kind != 2   return 0x87   end
      var w = matter.Tlv().st(1)
      w.bin(0, kind == 1 ? matter.Certs.DAC_Cert_FFF1_8000 : matter.Certs.PAI_Cert_FFF1)
      w.close()
      return [3, w]
    elif command == 4                   # CSRRequest
      var nonce = fields.find(0)
      if nonce == nil || size(nonce) != 32   return 0x87   end
      if device.failsafe_deadline == nil    return 0xCA   end
      var for_update = fields.find(1, false)
      if for_update && session.fabric == nil    return 0x85   end
      if device.noc_invoked()   return 0x87   end
      device.pending_for_update = for_update
      device.pending_key = crypto.random(32)
      var elements = matter.Tlv().st(nil)
      elements.bin(1, self.csr(device.pending_key))
      elements.bin(2, nonce)
      elements.close()
      var w = matter.Tlv().st(1)
      w.bin(0, elements.b)
      w.bin(1, ec.ecdsa_sign_sha256(matter.Certs.DAC_Priv_FFF1_8000, elements.b + session.challenge))
      w.close()
      return [5, w]
    elif command == 0x0B                # AddTrustedRootCertificate
      device.pending_root = fields.find(0)
      return 0
    elif command == 6                   # AddNOC
      if device.failsafe_deadline == nil    return 0xCA   end
      var result = device.add_fabric(session, fields)
      return self.noc_answer(result[0], result[1])
    elif command == 7                   # UpdateNOC
      var result = device.update_fabric(session, fields)
      return isinstance(result, list) ? self.noc_answer(result[0], result[1]) : result
    elif command == 9                   # UpdateFabricLabel
      if session.fabric == nil    return 0x7E    end
      session.fabric.label = fields.find(0, "")
      device.save_fabric(session.fabric)
      device.changed(0, self.CLUSTER, 1)
      return self.noc_answer(0, session.fabric.index)
    elif command == 0x0A                # RemoveFabric
      var index = fields.find(0)
      if !device.schedule_fabric_removal(index)
        return self.noc_answer(0x0B, 0)
      end
      return self.noc_answer(0, index)
    end
    return 0x81
  end
end

# Groups fail closed until group transport exists
class Matter_GroupKeys : Matter_Cluster
  static var CLUSTER = 0x003F
  static var WRITE_PRIVILEGES = {0: 4}
  static var INVOKE_PRIVILEGE = 5
  static var ATTRIBUTES = [0, 1, 2, 3]
  static var REVISION = 4

  def read(device, session, attr, w, t)
    if attr == 0 || attr == 1
      w.arr(t)
      w.close()
    elif attr == 2    w.uint(t, 0)
    elif attr == 3    w.uint(t, 3)
    else
      return false
    end
    return true
  end
end

# Counts down as the specification asks. Nothing on the device blinks yet: IdentifyType reports None.
class Matter_Identify : Matter_Cluster
  static var CLUSTER = 0x0003
  static var INVOKE_PRIVILEGE = 4
  static var ATTRIBUTES = [0, 1]
  static var ACCEPTED = [0, 0x40]
  static var REVISION = 5

  var deadline

  # Whole seconds left, rounded up so a running countdown never reads 0
  def remaining()
    if self.deadline == nil   return 0    end
    var left = self.deadline - matter.now()
    return left > 0 ? (left + 999) / 1000 : 0
  end

  def start(device, seconds)
    self.deadline = seconds > 0 ? matter.now() + seconds * 1000 : nil
    device.changed(self.endpoint.id, self.CLUSTER, 0)
  end

  def read(device, session, attr, w, t)
    if attr == 0      w.uint(t, self.remaining())
    elif attr == 1    w.uint(t, 0)
    else
      return false
    end
    return true
  end

  def write(device, session, attr, value)
    if attr == 0
      self.start(device, value)
      return 0
    end
    return super(self).write(device, session, attr, value)
  end

  def invoke(device, session, command, fields, exchange)
    if command == 0
      self.start(device, fields.find(0, 0))
      return 0
    end
    return command == 0x40 ? 0 : 0x81
  end

  def poll(device)
    if self.deadline != nil && self.remaining() == 0
      self.deadline = nil
      device.changed(self.endpoint.id, self.CLUSTER, 0)
    end
  end
end

class Matter_Groups : Matter_Cluster
  static var CLUSTER = 0x0004
  static var ATTRIBUTES = [0]
  static var REVISION = 4

  def read(device, session, attr, w, t)
    if attr == 0
      w.uint(t, 0)
      return true
    end
    return false
  end
end

# Where an on/off value lives. Clusters talk only to this interface: on(), set_on(value), ready().
class Matter_RelayOutput
  var relay

  def init(relay)
    self.relay = relay
  end

  def ready()
    return true
  end

  def on()
    import newmota
    var answer = newmota.cmd("Power" + str(self.relay))
    var value = answer != nil ? answer.find("POWER" + str(self.relay), answer.find("POWER")) : nil
    return value == "ON"
  end

  def set_on(value)
    import newmota
    newmota.cmd("Power" + str(self.relay) + (value ? " 1" : " 0"))
  end
end

# Without any hardware the state only lives here, enough to test commissioning
class Matter_VirtualOutput
  var state

  def ready()
    return true
  end

  def on()
    return self.state == true
  end

  def set_on(value)
    self.state = value
  end
end

# A local light with Matter's interface: on(), set_on(), ready(), get(), set(change), motion_interval()
class Matter_LocalLamp
  var index

  # How often a gradual change may update the light, in milliseconds
  def motion_interval()
    return 250
  end

  def init(index)
    self.index = index
  end

  def ready()
    return true
  end

  def get()
    import light
    return light.get(self.index)
  end

  def set(change)
    import light
    light.set(change, self.index)
  end

  def on()
    return self.get()['power']
  end

  def set_on(value)
    self.set({'power': value})
  end
end

class Matter_OnOff : Matter_Cluster
  static var CLUSTER = 0x0006
  static var ATTRIBUTES = [0]
  static var ACCEPTED = [0, 1, 2]
  static var REVISION = 6

  var output, state

  def init(endpoint, output)
    super(self).init(endpoint)
    self.output = output
    self.state = output.on()
  end

  def read(device, session, attr, w, t)
    if attr == 0
      w.bool(t, self.state)
      return true
    end
    return false
  end

  def invoke(device, session, command, fields, exchange)
    if command > 2    return 0x81   end
    if !self.output.ready()   return 0x01   end
    self.output.set_on(command == 2 ? !self.state : command == 1)
    self.follow(device)
    return 0
  end

  # Follows changes made by the button, console, web or MQTT
  def follow(device)
    var now = self.output.on()
    if now != self.state
      self.state = now
      device.changed(self.endpoint.id, self.CLUSTER, 0)
    end
  end
end

# Brightness of one light
class Matter_LevelControl : Matter_Cluster
  static var CLUSTER = 0x0008
  static var ATTRIBUTES = [0, 1, 2, 3, 0x0F, 0x11]
  static var ACCEPTED = [0, 1, 2, 3, 4, 5, 6, 7]
  static var FEATURES = 3
  static var REVISION = 6
  static var LOWEST = 1
  static var HIGHEST = 254
  static var DEFAULT_RATE = 50
  static var OPTION_TAGS = [2, 2, 3, 0]   # MoveToLevel, Move, Step and Stop
  static var FIELDS = ["hTMM", "dBMM", "dbTMM", "MM"]

  var lamp, level, options
  var motion, with_onoff, while_off, motion_time

  def init(endpoint, lamp)
    super(self).init(endpoint)
    self.lamp = lamp
    self.options = 0
    self.level = self.current()
  end

  def current()
    return (self.lamp.get().find('bri', 0) * 254 + 127) / 255
  end

  def read(device, session, attr, w, t)
    if attr == 0        w.uint(t, self.level)
    elif attr == 1      w.uint(t, self.motion != nil ? self.motion.tenths_left() : 0)
    elif attr == 2      w.uint(t, 0)
    elif attr == 3      w.uint(t, 254)
    elif attr == 0x0F   w.uint(t, self.options)
    elif attr == 0x11   w.uint(t, self.level)
    else
      return false
    end
    return true
  end

  def write(device, session, attr, value)
    if attr == 0x0F   return self.write_options(device, value, 3)   end
    return super(self).write(device, session, attr, value)
  end

  static def bri(level)
    return (level * 255 + 127) / 254
  end

  # RemainingTime reports only when a change starts or ends, not while it counts down
  def set_motion(device, motion)
    if motion != nil || self.motion != nil    device.changed(self.endpoint.id, self.CLUSTER, 1)   end
    self.motion = motion
  end

  def lowest_current()
    var start = self.current()
    return start < self.LOWEST ? self.LOWEST : start
  end

  def begin(device, motion)
    self.set_motion(device, motion)
    # Bridged lights turn on with each value, so one that was on stops when it is turned off
    self.while_off = self.with_onoff || !self.lamp.on()
    self.motion_time = matter.now()
    device.start_motion(self)
  end

  def invoke(device, session, command, fields, exchange)
    if self.ACCEPTED.find(command) == nil    return 0x81   end
    if !self.lamp.ready()   return 0x01   end
    var bad = self.bad_fields(fields, self.FIELDS[command % 4])
    if bad != nil   return bad    end
    var moves = command == 1 || command == 5 || command == 2 || command == 6
    if moves && fields.find(0) != 0 && fields.find(0) != 1    return 0x87   end
    if command < 4 && !self.executes(self.lamp, self.options, fields, self.OPTION_TAGS[command])    return 0    end
    self.set_motion(device, nil)
    self.with_onoff = command >= 4
    if command == 0 || command == 4
      var level = fields.find(0, 0)
      var tenths = fields.find(1)
      var start = self.lowest_current()
      var target = level < self.LOWEST ? self.LOWEST : (level > self.HIGHEST ? self.HIGHEST : level)
      if tenths == nil || tenths == 0 || target == start
        var change = {'bri': self.bri(level)}
        if command == 4   change['power'] = level > 0   end
        self.lamp.set(change)
        self.follow(device)
        return 0
      end
      if command == 4 && target > start   self.lamp.set({'power': true})    end
      var distance = target > start ? target - start : start - target
      self.begin(device, matter.Motion(start, (target - start) * 10.0 / tenths, self.LOWEST, self.HIGHEST, false, distance))
    elif moves
      var up = fields.find(0) == 0
      if up && self.with_onoff    self.lamp.set({'power': true})    end
      var start = self.lowest_current()
      if command == 1 || command == 5
        var rate = fields.find(1)
        if rate == 0    return 0    end      # Rate 0 moves nothing
        rate = rate != nil ? rate : self.DEFAULT_RATE
        self.begin(device, matter.Motion(start, up ? rate : -rate, self.LOWEST, self.HIGHEST, false, nil))
      else
        var amount = fields.find(1, 0)
        if amount == 0    return 0    end
        var tenths = fields.find(2)
        var instant = tenths == nil || tenths == 0
        var rate = instant ? amount : amount * 10.0 / tenths
        var motion = matter.Motion(start, up ? rate : -rate, self.LOWEST, self.HIGHEST, false, amount)
        if instant
          self.motion = motion
          self.run(device, 1.0)
        else
          self.begin(device, motion)
        end
      end
    end
    return 0
  end

  # Applies one advance. At the bottom, the WithOnOff commands also turn the light off.
  def run(device, seconds)
    var moving = self.motion.advance(seconds)
    var value = int(self.motion.value + 0.5)
    var change = {'bri': self.bri(value)}
    if !moving && self.with_onoff && value <= self.LOWEST   change['power'] = false   end
    self.lamp.set(change)
    self.follow(device)
    if !moving    self.set_motion(device, nil)    end
    return moving
  end

  def motion_tick(device, now)
    if self.motion == nil   return false    end
    var elapsed = now - self.motion_time
    if elapsed < self.lamp.motion_interval()    return true    end
    self.motion_time = now
    if !self.lamp.ready() || (!self.while_off && !self.lamp.on())
      self.set_motion(device, nil)
      return false
    end
    return self.run(device, elapsed / 1000.0)
  end

  def follow(device)
    var now = self.current()
    if now != self.level
      self.level = now
      device.changed(self.endpoint.id, self.CLUSTER, 0)
      device.changed(self.endpoint.id, self.CLUSTER, 0x11)
    end
  end
end

# Colour of one light: hue and saturation with XY when it has RGB, colour temperature when it has CT.
class Matter_ColorControl : Matter_Cluster
  static var CLUSTER = 0x0300
  static var WRITE_PRIVILEGES = {0x4010: 4}
  static var REVISION = 6
  static var CT_MIN = 153
  static var CT_MAX = 500
  static var FEATURE_COLOR = 0x09
  static var FEATURE_CT = 0x10
  static var HUE_HIGH = 254
  static var SAT_HIGH = 254
  static var XY_HIGH = 0xFEFF
  # Where the options mask and the transition time sit in each command
  static var OPTION_TAGS = {0: 3, 1: 2, 2: 3, 3: 2, 4: 2, 5: 3, 6: 3, 7: 3, 8: 2, 9: 3, 0x0A: 2, 0x47: 0, 0x4B: 4, 0x4C: 5}
  static var TIME_TAGS = {0: 2, 3: 1, 6: 2, 7: 2, 0x0A: 1}
  static var FIELDS = {0: "hdTMM", 1: "dbMM", 2: "dbBMM", 3: "hTMM", 4: "dbMM", 5: "dbBMM", 6: "hhTMM", 7: "xxTMM", 8: "ssMM", 9: "ssTMM", 0x0A: "xTMM", 0x47: "MM", 0x4B: "dwXXMM", 0x4C: "dwTXXMM"}

  var lamp, caps, options, mode, x, y, last
  var motions, while_off, motion_time

  def init(endpoint, lamp, caps)
    super(self).init(endpoint)
    self.lamp = lamp
    self.caps = caps
    self.options = 0
    self.x = 0
    self.y = 0
    self.last = self.snapshot()
    self.mode = (caps & self.FEATURE_COLOR) == 0 || self.last[3] == 'ct' ? 2 : 0
  end

  # Rounds to and from the light's units. 360 degrees would wrap to red, so the top stays below it.
  static def scale(value, from, to)
    return (value * to + from / 2) / from
  end

  def degrees(hue)
    var d = self.scale(hue, 254, 360)
    return d > 359 ? 359 : d
  end

  def hue_of(s)
    return self.scale(s[0], 360, 254)
  end

  def sat_of(s)
    return self.scale(s[1], 255, 254)
  end

  def features()
    return self.caps
  end

  # The commands of what this light can do; both kinds share StopMoveStep
  def accepted()
    var commands = [0x47]
    if (self.caps & self.FEATURE_COLOR) != 0    commands = commands + [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]    end
    if (self.caps & self.FEATURE_CT) != 0       commands = commands + [0x0A, 0x4B, 0x4C]    end
    return commands
  end

  def read_global(device, session, attr, w, t)
    if attr == 0xFFF9
      self.int_list(w, t, self.accepted())
      return true
    end
    return super(self).read_global(device, session, attr, w, t)
  end

  def all_attributes()
    var attrs = [2, 8, 0x0F, 0x10, 0x4001, 0x400A]
    if (self.caps & self.FEATURE_COLOR) != 0    attrs = attrs + [0, 1, 3, 4]    end
    if (self.caps & self.FEATURE_CT) != 0       attrs = attrs + [7, 0x400B, 0x400C, 0x400D, 0x4010]   end
    return attrs + self.GLOBALS
  end

  def snapshot()
    var s = self.lamp.get()
    return [s.find('hue', 0), s.find('sat', 0), s.find('ct', self.CT_MIN), s.find('colormode')]
  end

  # Converts hue and saturation at full brightness to CIE xy scaled to 65536
  def to_xy(hue, sat)
    import math
    var s = sat / 255.0
    var h = hue / 60.0
    var sector = int(h) % 6
    var f = h - int(h)
    var p = 1.0 - s
    var q = 1.0 - s * f
    var u = 1.0 - s * (1.0 - f)
    var rgb = [[1.0, u, p], [q, 1.0, p], [p, 1.0, u], [p, q, 1.0], [u, p, 1.0], [1.0, p, q]][sector]
    for i : 0 .. 2
      var c = rgb[i]
      rgb[i] = c > 0.04045 ? math.pow((c + 0.055) / 1.055, 2.4) : c / 12.92
    end
    var X = 0.4124 * rgb[0] + 0.3576 * rgb[1] + 0.1805 * rgb[2]
    var Y = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]
    var Z = 0.0193 * rgb[0] + 0.1192 * rgb[1] + 0.9505 * rgb[2]
    var sum = X + Y + Z
    return sum > 0 ? [int(X / sum * 65536), int(Y / sum * 65536)] : [0, 0]
  end

  # Converts CIE xy scaled to 65536 to hue and saturation
  def from_xy(x, y)
    import math
    var fx = x / 65536.0
    var fy = y / 65536.0
    if fy <= 0    return [0, 0]   end
    var X = fx / fy
    var Z = (1.0 - fx - fy) / fy
    var rgb = [3.2406 * X - 1.5372 - 0.4986 * Z, -0.9689 * X + 1.8758 + 0.0415 * Z, 0.0557 * X - 0.2040 + 1.0570 * Z]
    var top = 0.0
    for i : 0 .. 2
      var c = rgb[i] < 0 ? 0.0 : rgb[i]
      rgb[i] = c > 0.0031308 ? 1.055 * math.pow(c, 1 / 2.4) - 0.055 : 12.92 * c
      if rgb[i] > top   top = rgb[i]    end
    end
    if top <= 0   return [0, 0]   end
    var r = rgb[0] / top
    var g = rgb[1] / top
    var b = rgb[2] / top
    var low = r < g ? (r < b ? r : b) : (g < b ? g : b)
    var span = 1.0 - low
    var hue = 0.0
    if span > 0
      if r == 1.0       hue = 60.0 * (((g - b) / span) % 6)
      elif g == 1.0     hue = 60.0 * ((b - r) / span + 2)
      else              hue = 60.0 * ((r - g) / span + 4)
      end
    end
    if hue < 0    hue += 360    end
    return [int(hue), int(span * 255)]
  end

  def read(device, session, attr, w, t)
    var s = self.last
    if attr == 0        w.uint(t, self.hue_of(s))
    elif attr == 1      w.uint(t, self.sat_of(s))
    elif attr == 2      w.uint(t, self.remaining_time())
    elif attr == 0x10   w.uint(t, 0)
    elif attr == 3      w.uint(t, self.mode == 1 ? self.x : self.to_xy(s[0], s[1])[0])
    elif attr == 4      w.uint(t, self.mode == 1 ? self.y : self.to_xy(s[0], s[1])[1])
    elif attr == 7      w.uint(t, s[2])
    elif attr == 8 || attr == 0x4001    w.uint(t, self.mode)
    elif attr == 0x0F   w.uint(t, self.options)
    elif attr == 0x400A w.uint(t, self.caps)
    elif attr == 0x400B || attr == 0x400D   w.uint(t, self.CT_MIN)
    elif attr == 0x400C w.uint(t, self.CT_MAX)
    elif attr == 0x4010 w.null(t)
    else
      return false
    end
    return true
  end

  def write(device, session, attr, value)
    if attr == 0x0F   return self.write_options(device, value, 1)   end
    return attr == 0x4010 ? 0 : super(self).write(device, session, attr, value)
  end

  # The longest time left of the moving values
  def remaining_time()
    var longest = 0
    if self.motions != nil
      for m : self.motions
        var left = m.tenths_left()
        if left > longest   longest = left    end
      end
    end
    return longest
  end

  # RemainingTime reports only when a change starts or ends, not while it counts down
  def set_motions(device, motions)
    if motions != nil || self.motions != nil    device.changed(self.endpoint.id, self.CLUSTER, 2)   end
    self.motions = motions
  end

  def invoke(device, session, command, fields, exchange)
    if self.accepted().find(command) == nil    return 0x81   end
    if !self.lamp.ready()   return 0x01   end
    var bad = self.bad_fields(fields, self.FIELDS[command])
    if bad != nil   return bad    end
    if command >= 0x4B
      var low = fields.find(command == 0x4B ? 2 : 3, 0)
      var high = fields.find(command == 0x4B ? 3 : 4, 0)
      if low != 0 && high != 0 && low > high    return 0x87   end
    end
    var runs = self.executes(self.lamp, self.options, fields, self.OPTION_TAGS[command])
    if command == 0x47
      if runs   self.set_motions(device, nil)   end
      return 0
    end
    var gradual = self.start_gradual(device, command, fields, runs)
    if gradual != nil   return gradual    end
    if !runs    return 0    end
    self.set_motions(device, nil)
    if self.start_timed(device, command, fields)    return 0    end
    var change
    var mode = self.mode
    if command == 0
      change = {'hue': self.degrees(fields.find(0, 0))}
      mode = 0
    elif command == 3
      change = {'sat': self.scale(fields.find(0, 0), 254, 255)}
      mode = 0
    elif command == 6
      change = {'hue': self.degrees(fields.find(0, 0)), 'sat': self.scale(fields.find(1, 0), 254, 255)}
      mode = 0
    elif command == 7
      self.x = fields.find(0, 0)
      self.y = fields.find(1, 0)
      var hs = self.from_xy(self.x, self.y)
      change = {'hue': hs[0], 'sat': hs[1]}
      mode = 1
    elif command == 0x0A
      var ct = fields.find(0, self.CT_MIN)
      change = {'ct': ct < self.CT_MIN ? self.CT_MIN : (ct > self.CT_MAX ? self.CT_MAX : ct)}
      mode = 2
    end
    if change != nil    self.lamp.set(change)    end
    self.set_mode(device, mode)
    self.publish(device, self.snapshot())
    return 0
  end

  def set_mode(device, mode)
    if mode != self.mode
      self.mode = mode
      device.changed(self.endpoint.id, self.CLUSTER, 8)
      device.changed(self.endpoint.id, self.CLUSTER, 0x4001)
    end
  end

  # Signed distance from one hue to another: direction 0 shortest, 1 longest, 2 up, 3 down
  static def hue_path(start, target, direction)
    var up = (target - start + 255) % 255
    if up == 0    return 0    end
    var down = 255 - up
    if direction == 2   return up   end
    if direction == 3   return -down    end
    return (up <= down) == (direction == 0) ? up : -down
  end

  # Adds a value moving by a distance over a time, unless it is already there
  static def glide(motions, name, start, delta, tenths, low, high, wraps)
    if delta != 0
      motions[name] = matter.Motion(start, delta * 10.0 / tenths, low, high, wraps, delta < 0 ? -delta : delta)
    end
  end

  # Starts a command that moves to a value over its transition time. False leaves it to be set directly.
  def start_timed(device, command, fields)
    var tag = self.TIME_TAGS.find(command)
    var tenths = tag != nil ? fields.find(tag) : nil
    if tenths == nil || tenths == 0   return false    end
    var s = self.snapshot()
    var motions = {}
    var mode = 0
    if command == 0 || command == 6
      var start = self.hue_of(s)
      var delta = self.hue_path(start, fields.find(0, 0), command == 0 ? fields.find(1, 0) : 0)
      self.glide(motions, 'hue', start, delta, tenths, 0, self.HUE_HIGH, true)
    end
    if command == 3 || command == 6
      var start = self.sat_of(s)
      self.glide(motions, 'sat', start, fields.find(command == 3 ? 0 : 1, 0) - start, tenths, 0, self.SAT_HIGH, false)
    elif command == 7
      var xy = self.mode == 1 ? [self.x, self.y] : self.to_xy(s[0], s[1])
      self.x = xy[0]
      self.y = xy[1]
      self.glide(motions, 'x', xy[0], fields.find(0, 0) - xy[0], tenths, 0, self.XY_HIGH, false)
      self.glide(motions, 'y', xy[1], fields.find(1, 0) - xy[1], tenths, 0, self.XY_HIGH, false)
      mode = 1
    elif command == 0x0A
      var ct = fields.find(0, self.CT_MIN)
      ct = ct < self.CT_MIN ? self.CT_MIN : (ct > self.CT_MAX ? self.CT_MAX : ct)
      self.glide(motions, 'ct', s[2], ct - s[2], tenths, self.CT_MIN, self.CT_MAX, false)
      mode = 2
    end
    if size(motions) == 0   return false    end
    self.begin(device, motions, mode, false)
    return true
  end

  # Starts a Move or Step command, or gives nil for any other command. The status comes back.
  def start_gradual(device, command, fields, runs)
    var motions = {}
    var mode = 0
    var s = self.snapshot()
    var mode_field = fields.find(0)
    # Hue, saturation and colour temperature: mode 1 up, 3 down, and for Move 0 stops
    if command == 1 || command == 4 || command == 0x4B || command == 2 || command == 5 || command == 0x4C
      var is_move = command == 1 || command == 4 || command == 0x4B
      if is_move && mode_field == 0
        if runs   self.set_motions(device, nil)   end
        return 0
      end
      var amount = fields.find(1, 0)
      if amount == 0 && (!is_move || mode_field == 1 || mode_field == 3)    return 0x85   end
      if mode_field != 1 && mode_field != 3   return 0x87   end
      var sign = mode_field == 1 ? 1 : -1
      var tenths = is_move ? nil : fields.find(2)
      var instant = !is_move && (tenths == nil || tenths == 0)
      var rate = is_move ? amount : (instant ? amount : amount * 10.0 / tenths)
      var distance = is_move ? nil : amount
      if command == 1 || command == 2
        motions['hue'] = matter.Motion(self.hue_of(s), sign * rate, 0, self.HUE_HIGH, true, distance)
      elif command == 4 || command == 5
        motions['sat'] = matter.Motion(self.sat_of(s), sign * rate, 0, self.SAT_HIGH, false, distance)
      else
        var low = fields.find(is_move ? 2 : 3, 0)
        var high = fields.find(is_move ? 3 : 4, 0)
        low = low < self.CT_MIN ? self.CT_MIN : low
        high = high == 0 || high > self.CT_MAX ? self.CT_MAX : high
        motions['ct'] = matter.Motion(s[2], sign * rate, low, high, false, distance)
        mode = 2
      end
      if !runs    return 0    end
      return self.begin(device, motions, mode, instant)
    elif command == 8 || command == 9
      var ax = fields.find(0, 0)
      var ay = fields.find(1, 0)
      var xy = self.mode == 1 ? [self.x, self.y] : self.to_xy(s[0], s[1])
      if ax == 0 && ay == 0
        if command == 8
          if runs   self.set_motions(device, nil)   end
          return 0
        end
        return 0x85
      end
      if !runs    return 0    end
      var tenths = command == 9 ? fields.find(2) : nil
      var instant = command == 9 && (tenths == nil || tenths == 0)
      var scale = (command == 8 || instant) ? 1.0 : 10.0 / tenths
      var stepping = command == 9
      # An axis that does not move has nothing to finish, and keeps its value in self.x or self.y
      self.x = xy[0]
      self.y = xy[1]
      if ax != 0    motions['x'] = matter.Motion(xy[0], ax * scale, 0, self.XY_HIGH, false, stepping ? (ax < 0 ? -ax : ax) : nil)    end
      if ay != 0    motions['y'] = matter.Motion(xy[1], ay * scale, 0, self.XY_HIGH, false, stepping ? (ay < 0 ? -ay : ay) : nil)    end
      return self.begin(device, motions, 1, instant)
    end
    return nil
  end

  def begin(device, motions, mode, instant)
    self.set_motions(device, motions)
    self.set_mode(device, mode)
    if instant
      self.run(device, 1.0)
    else
      # Bridged lights turn on with each value, so one that was on stops when it is turned off
      self.while_off = !self.lamp.on()
      self.motion_time = matter.now()
      device.start_motion(self)
    end
    return 0
  end

  # Applies one advance of every moving value, as one change to the light
  def run(device, seconds)
    var change = {}
    var finished = []
    for name : self.motions.keys()
      var m = self.motions[name]
      if !m.advance(seconds)    finished.push(name)   end
      var v = int(m.value + 0.5)
      if name == 'hue'        change['hue'] = self.degrees(v % 255)    # 254.6 rounds up past the top
      elif name == 'sat'      change['sat'] = self.scale(v, 254, 255)
      elif name == 'ct'       change['ct'] = v
      elif name == 'x'        self.x = v
      elif name == 'y'        self.y = v
      end
    end
    if self.motions.contains('x') || self.motions.contains('y')
      var hs = self.from_xy(self.x, self.y)
      change['hue'] = hs[0]
      change['sat'] = hs[1]
    end
    self.lamp.set(change)
    self.publish(device, self.snapshot())
    for name : finished   self.motions.remove(name)   end
    if size(self.motions) == 0    self.set_motions(device, nil)   end
    return self.motions != nil
  end

  def motion_tick(device, now)
    if self.motions == nil    return false    end
    var elapsed = now - self.motion_time
    if elapsed < self.lamp.motion_interval()    return true    end
    self.motion_time = now
    if !self.lamp.ready() || (!self.while_off && !self.lamp.on())
      self.set_motions(device, nil)
      return false
    end
    return self.run(device, elapsed / 1000.0)
  end

  # Follows a change made elsewhere, as Tasmota does: XY stays until the light turns white, and
  # its point then comes from the new colour. A running motion owns the mode and the point.
  def follow(device)
    var now = self.snapshot()
    if self.motions == nil
      if now[3] == 'ct'
        self.set_mode(device, 2)
      elif now[3] == 'rgb' && self.mode == 2
        self.set_mode(device, 0)
      end
      if self.mode == 1 && (now[0] != self.last[0] || now[1] != self.last[1])
        var xy = self.to_xy(now[0], now[1])
        self.x = xy[0]
        self.y = xy[1]
      end
    end
    self.publish(device, now)
  end

  def publish(device, now)
    var id = self.endpoint.id
    var before = self.last
    self.last = now
    if now[0] != before[0] || now[1] != before[1]
      for attr : [0, 1, 3, 4]
        device.changed(id, self.CLUSTER, attr)
      end
    end
    if now[2] != before[2]
      device.changed(id, self.CLUSTER, 7)
    end
  end
end

# One measured value. The device reads the sensor list and hands each value to set().
class Matter_Measurement : Matter_Cluster
  static var ATTRIBUTES = [0, 1, 2]

  var cluster, revision_number, low, high, is_signed, value

  def init(endpoint, cluster, revision_number, low, high, is_signed)
    super(self).init(endpoint)
    self.cluster = cluster
    self.revision_number = revision_number
    self.low = low
    self.high = high
    self.is_signed = is_signed
  end

  def id()
    return self.cluster
  end

  def revision()
    return self.revision_number
  end

  def number(w, t, v)
    if v == nil
      w.null(t)
    elif self.is_signed
      w.int(t, v)
    else
      w.uint(t, v)
    end
  end

  def read(device, session, attr, w, t)
    if attr == 0      self.number(w, t, self.value)
    elif attr == 1    self.number(w, t, self.low)
    elif attr == 2    self.number(w, t, self.high)
    else
      return false
    end
    return true
  end

  def set(device, v)
    if v != self.value
      self.value = v
      device.changed(self.endpoint.id, self.cluster, 0)
    end
  end
end
