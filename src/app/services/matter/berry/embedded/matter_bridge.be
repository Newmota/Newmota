# MQTT bridge: remote Tasmota devices become bridged Matter endpoints. Subscribe only and never
# polled, with the epoch rules of Tasmota's Matter_MQTT_remote.be.
# Adapted from Tasmota lib/libesp32/berry_matter/src/embedded/Matter_MQTT_remote.be
# Source pin: f27e4077212dd9ae8e3ada3fa96b6040926e059b.
# Copyright (C) 2024  Stephan Hadinger, Christian Baars & Theo Arends
# SPDX-License-Identifier: GPL-3.0-or-later

#@ solidify:Matter_Remote,weak
#@ solidify:Matter_RemoteOutput,weak
#@ solidify:Matter_RemoteLamp,weak
#@ solidify:Matter_BridgedInfo,weak
#@ solidify:Matter_Bridge,weak

# One remote device: its topics, reachability and last known state
class Matter_Remote
  var device, topic, cmnd_base, tele_base, stat_base
  var reachable, confirmed, state
  var refresh_phase, refresh_status
  var link_up, info_clusters, sensors

  def init(device, topic, info)
    self.device = device
    self.topic = topic
    self.state = {}
    self.sensors = {}
    self.info_clusters = []
    self.reachable = false
    self.confirmed = false
    self.refresh_phase = 0
    self.refresh_status = 0
    self.link_up = false
    self.cmnd_base = self.expand(info, 0)
    self.stat_base = self.expand(info, 1)
    self.tele_base = self.expand(info, 2)
    self.subscribe()
  end

  # Expands a discovered FullTopic for command, stat or tele
  def expand(info, prefix_index)
    import string
    var full = info.find('ft', "%prefix%/%topic%/")
    var prefixes = info.find('tp', ["cmnd", "stat", "tele"])
    full = string.replace(full, "%prefix%", prefixes[prefix_index])
    full = string.replace(full, "%topic%", self.topic)
    full = string.replace(full, "%hostname%", info.find('hn', ""))
    full = string.replace(full, "%id%", info.find('mac', ""))
    if size(full) == 0 || full[-1] != '/'   full += '/'   end
    return full
  end

  # Two filters per remote keep several remotes within the script filter limit
  def subscribe()
    import mqtt
    mqtt.subscribe(self.tele_base + "+", / topic, idx, data, raw -> self.on_tele(topic, data))
    mqtt.subscribe(self.stat_base + "+", / topic, idx, data, raw -> self.on_stat(topic, data))
  end

  def can_send()
    import mqtt
    return mqtt.connected() && self.confirmed && self.reachable
  end

  def send(command, payload)
    import mqtt
    mqtt.publish(self.cmnd_base + command, payload)
  end

  # Follows the broker link: a new link starts a new epoch, because state may have moved on
  def poll()
    import mqtt
    var up = mqtt.connected()
    if up != self.link_up
      self.link_up = up
      if up
        self.begin_epoch()
      else
        self.set_reachable(false)
      end
    end
  end

  # Retained answers may be stale, so an ordinary answer to StatusRetain comes first
  def begin_epoch()
    self.confirmed = false
    self.refresh_phase = 1
    self.refresh_status = 0
    self.send("StatusRetain", "")
  end

  def set_reachable(value)
    if value != self.reachable
      self.reachable = value
      for c : self.info_clusters
        c.reachable_changed(self.device, value)
      end
    end
  end

  static def load(data)
    import json
    return (size(data) > 0 && data[0] == '{') ? json.load(data) : nil
  end

  def on_tele(topic, data)
    var name = topic[size(self.tele_base) ..]
    if name == "LWT"
      if data == "Online"
        self.begin_epoch()
      else
        self.confirmed = false
        self.set_reachable(false)
      end
    elif self.confirmed && (name == "STATE" || name == "SENSOR")
      var j = self.load(data)
      if j != nil
        self.set_reachable(true)
        self.merge(j)
      end
    end
    return nil
  end

  def on_stat(topic, data)
    var j = self.load(data)
    if j == nil   return nil    end
    if !self.confirmed
      if self.refresh_phase == 1
        if j.contains("StatusRetain")
          self.refresh_phase = 2
          self.send("Status", "10")
          self.send("Status", "11")
        end
      elif self.refresh_phase == 2
        if j.contains("StatusSNS")
          self.merge(j["StatusSNS"])
          self.refresh_status |= 1
        elif j.contains("StatusSTS")
          self.merge(j["StatusSTS"])
          self.refresh_status |= 2
        end
        if self.refresh_status == 3
          self.confirmed = true
          self.refresh_phase = 0
          self.set_reachable(true)
        end
      end
      return nil
    end
    self.set_reachable(true)
    self.merge(j.find("StatusSNS", j.find("StatusSTS", j)))
    return nil
  end

  # Keeps state fields and hands sensor values to their measurement clusters
  def merge(j)
    for k : j.keys()
      var v = j[k]
      if isinstance(v, map)
        for quantity : ['Temperature', 'Humidity', 'Pressure', 'Illuminance']
          var value = v.find(quantity)
          var m = value != nil ? self.sensors.find(k + "#" + quantity) : nil
          if m != nil   m.set(self.device, self.device.scale(quantity, value, j))   end
        end
      else
        self.state[k] = v
      end
    end
    self.device.follow_due = true
  end
end

class Matter_RemoteOutput
  var remote, relay

  def init(remote, relay)
    self.remote = remote
    self.relay = relay
  end

  def ready()
    return self.remote.can_send()
  end

  def on()
    var state = self.remote.state
    return state.find("POWER" + str(self.relay), self.relay == 1 ? state.find("POWER") : nil) == "ON"
  end

  def set_on(value)
    self.remote.send("Power" + str(self.relay), value ? "1" : "0")
  end
end

# A remote light. The channels list only tells add_light what kind of light this is.
class Matter_RemoteLamp : Matter_RemoteOutput
  var kind

  # A held button must not flood the broker
  def motion_interval()
    return 1000
  end

  def init(remote, relay, kind)
    super(self).init(remote, relay)
    self.kind = kind
  end

  def get()
    import string
    var state = self.remote.state
    var s = {'power': self.on(), 'bri': state.find('Dimmer', 0) * 255 / 100, 'channels': [0]}
    if self.kind == 'light2'
      s['ct'] = state.find('CT', 153)
    elif self.kind == 'light3'
      var hsb = string.split(state.find('HSBColor', "0,0,0"), ",")
      s['rgb'] = ""
      s['hue'] = int(hsb[0])
      s['sat'] = int(hsb[1]) * 255 / 100
    elif self.kind == 'light0'
      s['channels'] = []
    end
    return s
  end

  def set(change)
    if change.contains('power')   self.set_on(change['power'])    end
    if change.contains('bri')     self.remote.send("Dimmer", str(change['bri'] * 100 / 255))    end
    if change.contains('ct')      self.remote.send("CT", str(change['ct']))   end
    if change.contains('hue')     self.remote.send("HSBColor1", str(change['hue']))   end
    if change.contains('sat')     self.remote.send("HSBColor2", str(change['sat'] * 100 / 255))   end
  end
end

class Matter_BridgedInfo : Matter_Cluster
  static var CLUSTER = 0x0039
  static var ATTRIBUTES = [5, 0x11, 0x12]
  static var REVISION = 4

  var remote

  def init(endpoint, remote)
    super(self).init(endpoint)
    self.remote = remote
    remote.info_clusters.push(self)
  end

  def read(device, session, attr, w, t)
    if attr == 5          w.str(t, self.remote.topic)
    elif attr == 0x11     w.bool(t, self.remote.reachable)
    elif attr == 0x12     w.str(t, self.remote.topic)
    else
      return false
    end
    return true
  end

  # ReachableChanged, and the attribute that controllers show
  def reachable_changed(device, value)
    device.changed(self.endpoint.id, self.CLUSTER, 0x11)
    device.add_event(self.endpoint.id, self.CLUSTER, 3, 1, matter.Tlv().st(7).bool(0, value).close().b)
  end
end

# Owns the remotes, their stored configuration and discovery while configuring
class Matter_Bridge
  static var MAX_REMOTES = 7
  static var DISCOVERY_FILTER = "tasmota/discovery/#"

  var device, remotes, config, discovered

  def init(device)
    import json
    self.device = device
    self.remotes = []
    var stored = matter.store_load("bridge")
    self.config = stored != nil ? json.load(stored.asstring()) : nil
    if !isinstance(self.config, map)    self.config = {}    end
  end

  # Controllers expect the aggregator first, so it takes the first endpoint after the root
  def build_endpoints()
    if size(self.config) == 0   return    end
    var aggregator = matter.Endpoint(size(self.device.endpoints), [[0x000E, 2]])
    aggregator.clusters.push(matter.Descriptor(aggregator))
    aggregator.clusters.push(matter.Identify(aggregator))
    aggregator.parts = []
    self.device.endpoints.push(aggregator)
    for topic : self.config.keys()
      var entry = self.config[topic]
      var remote = matter.Remote(self.device, topic, entry.find('info', {}))
      self.remotes.push(remote)
      for d : entry.find('devices', [])
        var e = self.add_device(remote, d)
        if e != nil
          e.device_types.push([0x0013, 3])
          e.clusters.push(matter.BridgedInfo(e, remote))
          aggregator.parts.push(e.id)
        end
      end
    end
  end

  def add_device(remote, d)
    var kind = d.find('type')
    var relay = d.find('relay', 1)
    if kind == 'relay'
      var e = self.device.add_endpoint(0x010A, 3)
      e.clusters.push(matter.Groups(e))
      e.clusters.push(matter.OnOff(e, matter.RemoteOutput(remote, relay)))
      return e
    elif kind == 'light0' || kind == 'light1' || kind == 'light2' || kind == 'light3'
      return self.device.add_light(matter.RemoteLamp(remote, relay, kind))
    elif d.contains('filter')
      var m = self.device.add_sensor(kind, nil)
      remote.sensors[d['filter']] = m
      return m.endpoint
    end
    return nil
  end

  def poll()
    for remote : self.remotes
      remote.poll()
    end
  end

  # Changes apply at the next restart, because controllers read the node once
  def save()
    import json
    return matter.store_save("bridge", bytes().fromstring(json.dump(self.config)))
  end

  def add(topic, devices, info)
    if !self.config.contains(topic) && size(self.config) >= self.MAX_REMOTES
      return false
    end
    self.config[topic] = {'info': info != nil ? info : {}, 'devices': devices}
    return self.save()
  end

  def remove(topic)
    self.config.remove(topic)
    return self.save()
  end

  # Discovery delivers the retained messages of the whole fleet, so it runs only on request
  def discover(on)
    import mqtt
    if on
      self.discovered = {}
      mqtt.subscribe(self.DISCOVERY_FILTER, / topic, idx, data, raw -> self.on_discovery(topic, data))
    else
      mqtt.unsubscribe(self.DISCOVERY_FILTER)
    end
  end

  def on_discovery(topic, data)
    import json
    import string
    var j = (size(data) > 0 && data[0] == '{') ? json.load(data) : nil
    if j == nil || self.discovered == nil   return nil    end
    var parts = string.split(topic, "/")
    if size(parts) < 4    return nil    end
    var entry = self.discovered.find(parts[2], {})
    entry[parts[3]] = j
    self.discovered[parts[2]] = entry
    return nil
  end

  # Adds a discovered device under its own topic. Shutters have no bridged form and are skipped.
  def add_discovered(id)
    var entry = self.discovered != nil ? self.discovered.find(id) : nil
    var config = entry != nil ? entry.find('config') : nil
    if config == nil    return false    end
    var devices = []
    var rl = config.find('rl', [])
    var lt_st = config.find('lt_st', 0)
    var so = config.find('so', {})
    for i : 0 .. size(rl) - 1
      if rl[i] == 1
        devices.push({'type': 'relay', 'relay': i + 1})
      elif rl[i] == 2
        var kind = 'light0'
        if so.find('68', 0) != 0 || lt_st == 1    kind = 'light1'
        elif lt_st == 2                           kind = 'light2'
        elif lt_st >= 3                           kind = 'light3'
        end
        devices.push({'type': kind, 'relay': i + 1})
      end
    end
    var sensors = entry.find('sensors', {}).find('sn', {})
    for name : sensors.keys()
      var fields = sensors[name]
      if !isinstance(fields, map)   continue    end
      for quantity : ['Temperature', 'Humidity', 'Pressure', 'Illuminance']
        if fields.contains(quantity)
          devices.push({'type': quantity, 'filter': name + "#" + quantity})
        end
      end
    end
    var info = {}
    for k : ['ft', 'tp', 'hn', 'mac']
      if config.find(k) != nil    info[k] = config[k]    end
    end
    return self.add(config['t'], devices, info)
  end
end
