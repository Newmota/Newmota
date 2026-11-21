# Interaction Model with one decoder: requests are read straight from the parsed TLV, for
# simple and complex requests alike, and reports are written attribute by attribute.

#@ solidify:Matter_Report,weak
#@ solidify:Matter_Subscription,weak
#@ solidify:Matter_IM,weak

# One report in progress, resumable across chunks through its cursor
class Matter_Report
  static var MESSAGE_OVERHEAD = 64
  var im, session
  var exchange, initiator
  var sub
  var paths, path_i, ep_i, cluster_i, attr_i
  var pending             # element that did not fit the last chunk
  var queued              # the rest of a list that travels item by item
  var more                # the last chunk announced another
  var filtered, version_filters
  var event_paths, event_i, event_min, pending_event

  def init(im, session, exchange, initiator, sub, paths)
    self.im = im
    self.session = session
    self.exchange = exchange
    self.initiator = initiator
    self.sub = sub
    self.paths = paths
    self.path_i = 0
    self.ep_i = 0
    self.cluster_i = 0
    self.attr_i = 0
    self.more = false
    self.queued = []
    self.filtered = false
    self.event_i = 0
    self.event_min = 0
  end

  # Takes the filters of a read or subscribe request
  def set_request(filtered, version_filters, event_paths, event_filters)
    self.filtered = filtered
    self.version_filters = version_filters
    self.event_paths = event_paths
    for f : event_filters
      var least = f.find(1, 0)
      self.event_min = isinstance(least, bytes) ? least.get(0, 4) : least
    end
    return self
  end

  # A client already holding this version does not need the cluster again
  def known_version(ep, cluster)
    if self.version_filters == nil    return false    end
    var version = self.im.device.data_version(ep, cluster)
    for f : self.version_filters
      var p = f.find(0, {})
      if p.find(1) == ep && p.find(2) == cluster && f.find(1) == version
        return true
      end
    end
    return false
  end

  # Tests whether a requested event path covers an event
  def wants_event(event)
    for p : self.event_paths
      if (p.find(1) == nil || p[1] == event['ep']) && (p.find(2) == nil || p[2] == event['cluster']) &&
         (p.find(3) == nil || p[3] == event['id'])
        return true
      end
    end
    return false
  end

  # Encodes the next event for this report, or nil when none is left
  def next_event()
    if self.event_paths == nil    return nil    end
    var events = self.im.device.events
    while self.event_i < size(events)
      var event = events[self.event_i]
      self.event_i += 1
      if event['number'] >= self.event_min && self.wants_event(event)
        var w = matter.Tlv().st(nil)
        w.st(1)
        w.lst(0)
        w.uint(1, event['ep'])
        w.uint(2, event['cluster'])
        w.uint(3, event['id'])
        w.close()
        w.u64(1, matter.Fabric.id64(event['number']))
        w.uint(2, event['priority'])
        w.u64(4, matter.Fabric.id64(event['time']))
        w.raw(event['data'])
        w.close()
        w.close()
        return w.b
      end
    end
    if self.sub != nil    self.sub.event_min = self.im.device.next_event    end
    return nil
  end

  # Writes the path of one attribute
  static def path(w, t, ep, cluster, attr)
    w.lst(t)
    w.uint(2, ep)
    w.uint(3, cluster)
    w.uint(4, attr)
    w.close()
  end

  # One attribute report element around an encoded value. A null list index appends an item.
  def data_element(ep, cluster, attr, value, append)
    var w = matter.Tlv().st(nil)
    w.st(1)
    w.uint(0, self.im.device.data_version(ep, cluster))
    w.lst(1)
    w.uint(2, ep)
    w.uint(3, cluster)
    w.uint(4, attr)
    if append   w.null(5)   end
    w.close()
    w.raw(value)
    w.close()
    w.close()
    return w.b
  end

  # A list too long for one message goes as an empty list followed by appended items
  def split_list(ep, cluster, attr, value, limit)
    var items = matter.tlv_items(value)
    if items == nil   return nil    end
    for item : items
      if size(item) > limit   return nil    end
    end
    for item : items
      self.queued.push(self.data_element(ep, cluster, attr, item, true))
    end
    return self.data_element(ep, cluster, attr, matter.Tlv().arr(2).close().b, false)
  end

  # Encodes one attribute report element, or nil when a wildcard skips it
  def encode(ep, cluster, attr, concrete)
    var device = self.im.device
    var w = matter.Tlv().st(nil)
    var obj = device.find_cluster(ep, cluster)
    var value = matter.Tlv()
    var status = 0
    if obj != nil && self.known_version(ep, cluster)    return nil    end
    self.session.filtered = self.filtered
    if obj == nil
      status = device.find_endpoint(ep) == nil ? 0x7F : 0xC3
    elif !self.im.allowed(self.session, ep, cluster, obj.read_privilege(attr))
      status = 0x7E
    elif !obj.read_global(device, self.session, attr, value, 2)
      status = 0x86
    elif size(value.b) + self.MESSAGE_OVERHEAD > matter.max_message(self.session.ctx)
      var first = self.split_list(ep, cluster, attr, value.b, matter.max_message(self.session.ctx) - self.MESSAGE_OVERHEAD)
      if first != nil   return first    end
      status = 0x89
    end
    if status != 0
      if !concrete    return nil    end
      w.st(0)
      self.path(w, 0, ep, cluster, attr)
      w.st(1)
      w.uint(0, status)
      w.close()
      w.close()
    else
      w.st(1)
      w.uint(0, device.data_version(ep, cluster))
      self.path(w, 1, ep, cluster, attr)
      w.raw(value.b)
      w.close()
    end
    w.close()
    return w.b
  end

  # Returns the next element, advancing the cursor over paths and wildcards
  def next_element()
    var device = self.im.device
    if size(self.queued) > 0
      return self.queued.pop(0)
    end
    while self.path_i < size(self.paths)
      var p = self.paths[self.path_i]
      var ep_want = p.find(2)
      var cluster_want = p.find(3)
      var attr_want = p.find(4)
      if ep_want != nil && cluster_want != nil && attr_want != nil
        self.path_i += 1
        var e = self.encode(ep_want, cluster_want, attr_want, true)
        if e != nil   return e   end
        continue
      end
      var endpoints = device.endpoints
      if self.ep_i >= size(endpoints)
        self.path_i += 1
        self.ep_i = 0
        continue
      end
      var ep = endpoints[self.ep_i]
      if ep_want != nil && ep.id != ep_want
        self.ep_i += 1
        continue
      end
      if self.cluster_i >= size(ep.clusters)
        self.ep_i += 1
        self.cluster_i = 0
        continue
      end
      var cluster = ep.clusters[self.cluster_i]
      if cluster_want != nil && cluster.id() != cluster_want
        self.cluster_i += 1
        continue
      end
      var attrs = cluster.all_attributes()
      if self.attr_i >= size(attrs)
        self.cluster_i += 1
        self.attr_i = 0
        continue
      end
      var attr = attrs[self.attr_i]
      self.attr_i += 1
      if attr_want == nil || attr == attr_want
        var e = self.encode(ep.id, cluster.id(), attr, false)
        if e != nil   return e   end
      end
    end
    return nil
  end

  # Sends the next chunk. Returns true when more chunks remain.
  def send_chunk()
    var limit = matter.max_message(self.session.ctx) - 16
    var w = matter.Tlv().st(nil)
    if self.sub != nil    w.uint(0, self.sub.id)    end
    w.arr(1)
    var more = false
    while true
      var e = self.pending
      self.pending = nil
      if e == nil   e = self.next_element()   end
      if e == nil   break   end
      if size(w.b) + size(e) > limit
        if size(w.b) > 16
          self.pending = e
          more = true
          break
        end
        continue                    # A value this large already became a status in encode()
      end
      w.raw(e)
    end
    w.close()
    if self.event_paths != nil && !more
      w.arr(2)
      while true
        var e = self.pending_event
        self.pending_event = nil
        if e == nil   e = self.next_event()   end
        if e == nil   break   end
        if size(w.b) + size(e) > limit
          self.pending_event = e
          more = true
          break
        end
        w.raw(e)
      end
      w.close()
    end
    self.more = more
    if more
      w.bool(3, true)
    elif self.sub == nil
      w.bool(4, true)               # A read ends without a status response
    end
    w.uint(0xFF, matter.Device.IM_REVISION)
    w.close()
    matter.send(self.session.ctx, self.exchange, self.initiator, 1, 0x05, w.b, true)
    return more
  end
end

class Matter_Subscription
  var id, session, paths, floor_ms, ceiling_ms
  var dirty, last_report, active, event_min, filtered, event_paths
  var resuming            # restored after a restart, and owes the client a full report

  def init(id, session, paths, floor_s, ceiling_s, now)
    self.id = id
    self.session = session
    self.paths = paths
    self.floor_ms = floor_s * 1000
    self.ceiling_ms = ceiling_s * 1000
    self.dirty = []
    self.last_report = now
    self.active = false
    self.event_min = 0
  end

  # Tests whether a changed attribute falls under a requested path
  def covers(ep, cluster, attr)
    for p : self.paths
      if (p.find(2) == nil || p[2] == ep) && (p.find(3) == nil || p[3] == cluster) &&
         (p.find(4) == nil || p[4] == attr)
        return true
      end
    end
    return false
  end
end

class Matter_IM
  static var MAX_SUBSCRIPTIONS = 9
  static var TIMED_REQUEST = 0x0A

  var device
  var reports             # by context and exchange
  var subs
  var timed               # by context and exchange: deadline
  var next_exchange, next_sub

  def init(device)
    import crypto
    self.device = device
    self.reports = {}
    self.subs = []
    self.timed = {}
    self.next_exchange = crypto.random(2).get(0, 2)
    self.next_sub = crypto.random(4).get(0, 4) & 0x7FFFFFFF
  end

  static def key(ctx, exchange)
    return (ctx << 16) | exchange
  end

  # True when an entry of the fabric grants this subject the privilege on this path
  def granted(session, ep, cluster, privilege)
    var fabric = session.fabric
    for e : fabric.acl
      if e[1] < privilege || e[2] != 2    continue    end
      var subjects = e.find(3)
      var subject_ok = subjects == nil || size(subjects) == 0
      if !subject_ok
        for s : subjects
          var id = matter.Fabric.id64(s)
          if id == session.peer_node    subject_ok = true   end
          # CASE Authenticated Tags: FFFFFFFD in the upper half, the tag below
          if id[4..7] == bytes("FDFFFFFF")
            for cat : session.cats
              if id[0..3] == bytes().add(cat, 4)    subject_ok = true   end
            end
          end
        end
      end
      if !subject_ok    continue    end
      var targets = e.find(4)
      if targets == nil || size(targets) == 0   return true   end
      for target : targets
        if (target.find(0) == nil || target[0] == cluster) && (target.find(1) == nil || target[1] == ep)
          return true
        end
      end
    end
    return false
  end

  # The single access decision for every read, write and invoke. PASE is implicitly administrator.
  # A denial only logs until enforcement is on, so real controllers can prove the policy first.
  def allowed(session, ep, cluster, privilege)
    if session == nil   return false    end
    if session.fabric == nil    return true   end
    if self.granted(session, ep, cluster, privilege)    return true    end
    if self.device.enforce_access   return false    end
    log("MTR: ACL would deny fabric " + str(session.fabric.index) + " on " + str(ep) + "/" + str(cluster) +
      " needing privilege " + str(privilege), 3)
    return true
  end

  def send_status(session, exchange, initiator, code)
    var w = matter.Tlv().st(nil)
    w.uint(0, code)
    w.uint(0xFF, matter.Device.IM_REVISION)
    w.close()
    matter.send(session.ctx, exchange, initiator, 1, 0x01, w.b, true)
  end

  def message(session, exchange, peer_initiator, opcode, payload)
    var initiator = !peer_initiator
    var v = matter.tlv(payload)
    if !isinstance(v, map)
      return self.send_status(session, exchange, initiator, 0x80)
    end
    # The answer to a command this device sent
    var client = self.device.client
    if !peer_initiator && client != nil && client.response(session, exchange, opcode, v)
      return
    end
    if   opcode == 0x01   self.status_response(session, exchange, initiator, v)
    elif opcode == 0x02   self.read(session, exchange, initiator, v)
    elif opcode == 0x03   self.subscribe(session, exchange, initiator, v)
    elif opcode == 0x06   self.write(session, exchange, initiator, v)
    elif opcode == 0x08   self.invoke(session, exchange, initiator, v)
    elif opcode == self.TIMED_REQUEST
      self.timed[self.key(session.ctx, exchange)] = matter.now() + v.find(0, 0)
      self.send_status(session, exchange, initiator, 0)
    else
      self.send_status(session, exchange, initiator, 0x80)
    end
  end

  # A peer continues a chunked report or finishes a subscription's priming
  def status_response(session, exchange, initiator, v)
    var k = self.key(session.ctx, exchange)
    var report = self.reports.find(k)
    if report == nil    return    end
    if v.find(0, 0) != 0
      self.reports.remove(k)
      # A client can refuse a resumed subscription it no longer knows
      if report.sub != nil && self.subs.find(report.sub) != nil
        self.subs.remove(self.subs.find(report.sub))
        log("MTR: Subscription refused", 2)
        if session.fabric != nil    self.device.save_sessions(session.fabric)   end
      end
      return
    end
    if report.more
      if report.send_chunk()    return    end
      if report.sub == nil
        self.reports.remove(k)
      end
      return
    end
    self.reports.remove(k)
    var sub = report.sub
    if sub != nil && !sub.active
      var w = matter.Tlv().st(nil)
      w.uint(0, sub.id)
      w.uint(2, sub.ceiling_ms / 1000)
      w.uint(0xFF, matter.Device.IM_REVISION)
      w.close()
      matter.send(session.ctx, exchange, initiator, 1, 0x04, w.b, true)
      sub.active = true
      sub.last_report = matter.now()
      log("MTR: Subscription established with " + str(size(sub.paths)) + " paths, reports at least every " +
        str(sub.ceiling_ms / 1000) + " s", 2)
      if session.fabric != nil    self.device.save_sessions(session.fabric)   end
    end
  end

  def read(session, exchange, initiator, v)
    var report = matter.Report(self, session, exchange, initiator, nil, v.find(0, []))
    report.set_request(v.find(3, false), v.find(4), v.find(1), v.find(2, []))
    if report.send_chunk()
      self.reports[self.key(session.ctx, exchange)] = report
    end
  end

  def subscribe(session, exchange, initiator, v)
    if !v.find(0, false)
      self.drop_session_subscriptions(session.ctx)
    end
    if size(self.subs) >= self.MAX_SUBSCRIPTIONS
      return self.send_status(session, exchange, initiator, 0x89)
    end
    var floor_s = v.find(1, 0)
    var ceiling_s = v.find(2, 60)
    if ceiling_s < floor_s    ceiling_s = floor_s   end
    self.next_sub = (self.next_sub + 1) & 0x7FFFFFFF
    var sub = matter.Subscription(self.next_sub, session, v.find(3, []), floor_s, ceiling_s, matter.now())
    self.subs.push(sub)
    var report = matter.Report(self, session, exchange, initiator, sub, sub.paths)
    report.set_request(v.find(7, false), v.find(8), v.find(4), v.find(5, []))
    sub.filtered = report.filtered
    sub.event_paths = report.event_paths
    sub.event_min = report.event_min
    self.reports[self.key(session.ctx, exchange)] = report
    report.send_chunk()
  end

  def write(session, exchange, initiator, v)
    var device = self.device
    var timed = self.consume_timed(session, exchange)
    var w = matter.Tlv().st(nil)
    w.arr(0)
    for item : v.find(2, [])
      var p = item.find(1, {})
      var ep = p.find(2)
      var cluster = p.find(3)
      var attr = p.find(4)
      var obj = device.find_cluster(ep, cluster)
      var status
      if obj == nil
        status = device.find_endpoint(ep) == nil ? 0x7F : 0xC3
      elif !self.allowed(session, ep, cluster, obj.write_privilege(attr))
        status = 0x7E
      else
        session.write_path = p
        status = obj.write(device, session, attr, item.find(2))
      end
      if status == 0    device.changed(ep, cluster, attr)   end
      w.st(nil)
      matter.Report.path(w, 0, ep, cluster, attr)
      w.st(1)
      w.uint(0, status)
      w.close()
      w.close()
    end
    w.close()
    w.uint(0xFF, matter.Device.IM_REVISION)
    w.close()
    if !v.find(0, false)
      matter.send(session.ctx, exchange, initiator, 1, 0x07, w.b, true)
    end
  end

  # Takes a pending timed request of this exchange
  def consume_timed(session, exchange)
    var k = self.key(session.ctx, exchange)
    var deadline = self.timed.find(k)
    if deadline == nil    return false    end
    self.timed.remove(k)
    return (matter.now() - deadline) <= 0
  end

  # Handlers return a status, or nil when they answer later through finish_invoke
  def invoke(session, exchange, initiator, v)
    var timed = self.consume_timed(session, exchange)
    if v.find(1, false) && !timed
      return self.send_status(session, exchange, initiator, 0xC6)
    end
    var device = self.device
    var w = matter.Tlv().st(nil)
    w.bool(0, false)
    w.arr(1)
    var deferred = false
    for item : v.find(2, [])
      var p = item.find(0, {})
      var ep = p.find(0)
      var cluster = p.find(1)
      var command = p.find(2)
      var obj = device.find_cluster(ep, cluster)
      var result
      if obj == nil
        result = device.find_endpoint(ep) == nil ? 0x7F : 0xC3
      elif !self.allowed(session, ep, cluster, obj.invoke_privilege(command))
        result = 0x7E
      elif obj.needs_timed(command) && !timed
        result = 0xC6
      else
        result = obj.invoke(device, session, command, item.find(1, {}), exchange)
      end
      log(format("MTR: Command endpoint %i cluster 0x%04X command 0x%02X", ep, cluster, command), 2)
      if result == nil
        deferred = true
      else
        self.encode_result(w, ep, cluster, command, result)
      end
    end
    if !deferred
      self.finish_invoke(session, exchange, initiator, w)
    end
  end

  # Writes one invoke result: a status int, or [response command, fields writer]
  def encode_result(w, ep, cluster, command, result)
    w.st(nil)
    if isinstance(result, list)
      w.st(0)
      w.lst(0)
      w.uint(0, ep)
      w.uint(1, cluster)
      w.uint(2, result[0])
      w.close()
      w.raw(result[1].b)
      w.close()
    else
      w.st(1)
      w.lst(0)
      w.uint(0, ep)
      w.uint(1, cluster)
      w.uint(2, command)
      w.close()
      w.st(1)
      w.uint(0, result)
      w.close()
      w.close()
    end
    w.close()
  end

  # Answers a command whose handler returned nil once its result exists
  def deferred_response(session, exchange, ep, cluster, result)
    var w = matter.Tlv().st(nil)
    w.bool(0, false)
    w.arr(1)
    self.encode_result(w, ep, cluster, 0, result)
    self.finish_invoke(session, exchange, false, w)
  end

  # Completes an invoke response, immediately or from a deferred handler
  def finish_invoke(session, exchange, initiator, w)
    w.close()
    w.uint(0xFF, matter.Device.IM_REVISION)
    w.close()
    matter.send(session.ctx, exchange, initiator, 1, 0x09, w.b, true)
  end

  # Packs request paths into 11 bytes each, so stored subscriptions stay one TLV item
  static def pack_paths(paths, first)
    var out = bytes()
    for p : paths
      var ep = p.find(first)
      var cluster = p.find(first + 1)
      var item = p.find(first + 2)
      out.add((ep != nil ? 1 : 0) | (cluster != nil ? 2 : 0) | (item != nil ? 4 : 0), 1)
      out.add(ep != nil ? ep : 0, 2)
      out.add(cluster != nil ? cluster : 0, 4)
      out.add(item != nil ? item : 0, 4)
    end
    return out
  end

  static def unpack_paths(packed, first)
    var paths = []
    var offset = 0
    while offset + 11 <= size(packed)
      var flags = packed[offset]
      var p = {}
      if (flags & 1) != 0   p[first] = packed.get(offset + 1, 2)      end
      if (flags & 2) != 0   p[first + 1] = packed.get(offset + 3, 4)  end
      if (flags & 4) != 0   p[first + 2] = packed.get(offset + 7, 4)  end
      paths.push(p)
      offset += 11
    end
    return paths
  end

  def changed(ep, cluster, attr)
    for sub : self.subs
      if sub.covers(ep, cluster, attr)
        var found = false
        for d : sub.dirty
          if d[2] == ep && d[3] == cluster && d[4] == attr
            found = true
            break
          end
        end
        if !found   sub.dirty.push({2: ep, 3: cluster, 4: attr})   end
      end
    end
  end

  def drop_session_subscriptions(ctx)
    var i = size(self.subs) - 1
    while i >= 0
      if self.subs[i].session.ctx == ctx
        log("MTR: Subscription ended", 2)
        var fabric = self.subs[i].session.fabric
        self.subs.remove(i)
        if fabric != nil    self.device.save_sessions(fabric)   end
      end
      i -= 1
    end
    var gone = []
    for k : self.reports.keys()
      if self.reports[k].session.ctx == ctx   gone.push(k)    end
    end
    for k : gone    self.reports.remove(k)    end
  end

  # A session stopped answering: whatever it subscribed to has ended
  def delivery_failed(ctx)
    self.drop_session_subscriptions(ctx)
  end

  # Sends changes after the floor interval and keep-alives before the ceiling
  def tick(now)
    var ended = []
    for sub : self.subs
      if matter.max_message(sub.session.ctx) == 0    # Its session was evicted
        ended.push(sub.session.ctx)
      end
    end
    for ctx : ended   self.drop_session_subscriptions(ctx)    end
    var network_ready = matter.network_ready()
    for sub : self.subs
      if !sub.active || (sub.resuming == true && !network_ready)    continue    end
      var since = now - sub.last_report
      var event_due = sub.event_paths != nil && self.device.next_event > sub.event_min
      var due = ((size(sub.dirty) > 0 || event_due) && since >= sub.floor_ms) || since >= sub.ceiling_ms
      # Everything may have changed while the device restarted, so a resumed client gets all of it
      var resume = sub.resuming == true
      if !due && !resume    continue    end
      self.next_exchange = (self.next_exchange + 1) & 0xFFFF
      var paths = resume ? sub.paths : sub.dirty
      if resume   log("MTR: Subscription resumed on fabric " + str(sub.session.fabric.index), 2)    end
      sub.resuming = false
      sub.dirty = []
      sub.last_report = now
      var report = matter.Report(self, sub.session, self.next_exchange, true, sub, paths)
      report.set_request(sub.filtered, nil, sub.event_paths, [{1: sub.event_min}])
      # Kept until the client answers, because it may refuse the subscription
      self.reports[self.key(sub.session.ctx, self.next_exchange)] = report
      report.send_chunk()
    end
    var expired = []
    for k : self.timed.keys()
      if (now - self.timed[k]) > 0    expired.push(k)    end
    end
    for k : expired   self.timed.remove(k)    end
  end
end
