# Electrical Sensor clusters for the energy meter: Power Topology, Electrical Power Measurement
# and Electrical Energy Measurement, fed by matter.energy(). Only with USE_MATTER_ENERGY.
# Power attributes follow Tasmota's Matter_Plugin_3_OnOff_Power.be; Tasmota has no energy totals.
# Source pin: f27e4077212dd9ae8e3ada3fa96b6040926e059b.
# Copyright (C) 2023  Stephan Hadinger & Theo Arends, 2026 Christian Baars
# SPDX-License-Identifier: GPL-3.0-or-later

#@ solidify:Matter_PowerTopology,weak
#@ solidify:Matter_ElectricalPower,weak
#@ solidify:Matter_ElectricalEnergy,weak

# The endpoint meters the whole node
class Matter_PowerTopology : Matter_Cluster
  static var CLUSTER = 0x009C
  static var FEATURES = 0x01          # NODE
end

# Live readings, all nullable until the meter measures them
class Matter_ElectricalPower : Matter_Cluster
  static var CLUSTER = 0x0090
  static var REVISION = 3
  static var AC_ATTRIBUTES = [0, 1, 2, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0E, 0x11]
  static var DC_ATTRIBUTES = [0, 1, 2, 0x04, 0x05, 0x08]
  # Attribute: [index in matter.energy(), present bit, MeasurementTypeEnum, low, high]
  static var FIELDS = {
    0x04: [2, 0, 1, 0, 1000000],                # Voltage, mV
    0x05: [3, 1, 2, -100000, 100000],           # ActiveCurrent, mA
    0x08: [4, 2, 5, -50000000, 50000000],       # ActivePower, mW
    0x09: [6, 4, 6, -50000000, 50000000],       # ReactivePower, mvar
    0x0A: [5, 3, 7, 0, 50000000],               # ApparentPower, mVA
    0x0B: [2, 0, 8, 0, 300000],                 # RMSVoltage, mV
    0x0C: [3, 1, 9, 0, 100000],                 # RMSCurrent, mA
    0x0E: [7, 6, 11, 0, 100000],                # Frequency, mHz
    0x11: [8, 5, 12, -10000, 10000],            # PowerFactor, 1/10000
  }
  static var POLL_SECONDS = 5

  var dc, attributes, values, countdown

  # Adds a one-channel meter as an Electrical Sensor on the first relay, or on its own endpoint
  static def add_to(device, e)
    var reading = matter.energy()
    if reading == nil   return    end
    if e == nil
      e = device.add_endpoint(0x0510, 1)
    else
      e.device_types.push([0x0510, 1])
    end
    e.clusters.push(matter.PowerTopology(e))
    e.clusters.push(matter.ElectricalPower(e, reading))
    e.clusters.push(matter.ElectricalEnergy(e, reading))
  end

  def init(endpoint, reading)
    super(self).init(endpoint)
    self.dc = reading[1] != 0
    self.attributes = self.dc ? self.DC_ATTRIBUTES : self.AC_ATTRIBUTES
    self.values = reading
    self.countdown = self.POLL_SECONDS
  end

  def all_attributes()
    return self.attributes + self.GLOBALS
  end

  def features()
    return self.dc ? 0x01 : 0x02        # DIRC or ALTC
  end

  # The value of one attribute, nil when the meter has none
  def value(attr)
    var f = self.FIELDS[attr]
    var v = self.values
    return v != nil && (v[0] & (1 << f[1])) != 0 ? v[f[0]] : nil
  end

  def poll(device)
    self.countdown -= 1
    if self.countdown > 0   return    end
    self.countdown = self.POLL_SECONDS
    var before = {}
    for attr : self.attributes
      if attr > 2   before[attr] = self.value(attr)   end
    end
    self.values = matter.energy()
    for attr : before.keys()
      if self.value(attr) != before[attr]
        device.changed(self.endpoint.id, self.CLUSTER, attr)
      end
    end
  end

  def read(device, session, attr, w, t)
    if attr == 0
      w.uint(t, self.dc ? 1 : 2)
    elif attr == 1
      w.uint(t, size(self.attributes) - 3)
    elif attr == 2
      w.arr(t)
      for a : self.attributes
        if a <= 2   continue    end
        var f = self.FIELDS[a]
        w.st(nil).uint(0, f[2]).bool(1, true).int(2, f[3]).int(3, f[4])
        w.arr(4).st(nil).int(0, f[3]).int(1, f[4]).uint(2, 500).close().close()   # 5.00 %
        w.close()
      end
      w.close()
    elif self.attributes.find(attr) != nil
      var v = self.value(attr)
      if v == nil   w.null(t)   else    w.int(t, v)   end
    else
      return false
    end
    return true
  end
end

# Lifetime totals of the meter. Imported always, exported when the meter reports export or had
# exported at the start. A total that drops, as when a Berry driver sets lower totals, marks a reset:
# its system time until the next restart and, with valid time, its epoch time in the Matter store.
class Matter_ElectricalEnergy : Matter_Cluster
  static var CLUSTER = 0x0091
  static var REVISION = 2
  static var POLL_SECONDS = 60
  static var EVENT_SECONDS = 900
  static var MAX_ENERGY = bytes("0000000000000040")     # 2^62 mWh, the spec's limit
  static var MATTER_EPOCH = 946684800                   # 2000-01-01 in Unix seconds

  var exports, imported, exported, at, countdown, event_countdown, reported
  var imported_reset, exported_reset, imported_epoch, exported_epoch

  def init(endpoint, reading)
    super(self).init(endpoint)
    var stored = matter.store_load("energy")
    var v = stored != nil ? matter.tlv(stored) : nil
    if isinstance(v, map)
      self.imported_epoch = v.find(0)
      self.exported_epoch = v.find(1)
    end
    self.exports = reading[11] != 0
    self.imported = reading[9]
    self.exported = reading[10]
    self.at = matter.systime()
    self.countdown = self.POLL_SECONDS
    self.event_countdown = 1
  end

  def all_attributes()
    return (self.exports ? [0, 1, 2, 5] : [0, 1, 5]) + self.GLOBALS
  end

  def features()
    return self.exports ? 0x07 : 0x05   # IMPE and CUME, with EXPE
  end

  # Signed 64-bit integer from 8 little-endian bytes
  static def i64(w, t, v)
    w.tag(t, 0x03)
    w.b .. v
  end

  # Whether unsigned 8-byte little-endian a is below b
  static def below(a, b)
    var i = 7
    while i >= 0
      if a[i] != b[i]   return a[i] < b[i]    end
      i -= 1
    end
    return false
  end

  # EnergyMeasurementStruct with the reading's system time
  def measurement(w, t, energy)
    w.st(t)
    self.i64(w, 0, energy)
    w.u64(4, self.at)
    w.close()
  end

  def poll(device)
    self.countdown -= 1
    self.event_countdown -= 1
    if self.countdown > 0   return    end
    self.countdown = self.POLL_SECONDS
    var v = matter.energy()
    if v != nil && (v[9] != self.imported || v[10] != self.exported)
      self.at = matter.systime()
      if v[9] != self.imported    device.changed(self.endpoint.id, self.CLUSTER, 1)   end
      if self.exports && v[10] != self.exported   device.changed(self.endpoint.id, self.CLUSTER, 2)   end
      var imported_reset = self.below(v[9], self.imported)
      var exported_reset = self.exports && self.below(v[10], self.exported)
      if imported_reset   self.imported_reset = self.at   end
      if exported_reset   self.exported_reset = self.at   end
      if imported_reset || exported_reset
        self.save_epoch(imported_reset, exported_reset)
        device.changed(self.endpoint.id, self.CLUSTER, 5)
      end
      self.imported = v[9]
      self.exported = v[10]
      self.reported = false
    end
    if self.event_countdown <= 0 && !self.reported
      self.event_countdown = self.EVENT_SECONDS
      self.reported = true
      var w = matter.Tlv().st(7)
      self.measurement(w, 0, self.imported)
      if self.exports   self.measurement(w, 1, self.exported)    end
      device.add_event(self.endpoint.id, self.CLUSTER, 0, 1, w.close().b)   # CumulativeEnergyMeasured, info
    end
  end

  # Keeps the epoch time of a reset across restarts; a reset without valid time keeps the old one
  def save_epoch(imported, exported)
    import newmota
    if !newmota.rtc('valid')   return    end
    var now = newmota.rtc('utc') - self.MATTER_EPOCH
    if imported   self.imported_epoch = now   end
    if exported   self.exported_epoch = now   end
    var w = matter.Tlv().st(nil)
    if self.imported_epoch != nil   w.uint(0, self.imported_epoch)   end
    if self.exported_epoch != nil   w.uint(1, self.exported_epoch)   end
    matter.store_save("energy", w.close().b)
  end

  def read(device, session, attr, w, t)
    if attr == 0
      w.st(t).uint(0, 14).bool(1, true).int(2, 0)
      self.i64(w, 3, self.MAX_ENERGY)
      w.arr(4).st(nil).int(0, 0)
      self.i64(w, 1, self.MAX_ENERGY)
      w.uint(2, 500).close().close().close()                                         # 5.00 %
    elif attr == 1
      self.measurement(w, t, self.imported)
    elif attr == 2 && self.exports
      self.measurement(w, t, self.exported)
    elif attr == 5
      if self.imported_reset == nil && self.exported_reset == nil &&
         self.imported_epoch == nil && self.exported_epoch == nil
        w.null(t)
      else
        w.st(t)
        if self.imported_epoch != nil   w.uint(0, self.imported_epoch)   end
        if self.exported_epoch != nil   w.uint(1, self.exported_epoch)   end
        if self.imported_reset != nil   w.u64(2, self.imported_reset)   end
        if self.exported_reset != nil   w.u64(3, self.exported_reset)   end
        w.close()
      end
    else
      return false
    end
    return true
  end
end
