# Camera device type (0x0142) over the CSI camera: Camera AV Stream Management, digital PTZ in
# Camera AV Settings User Level Management, and the WebRTC Transport Provider, which sends its
# answer to the controller's WebRTC Transport Requestor. Only with USE_MATTER_CAMERA. The driver
# answers offers and makes them; it runs one encoder and one WebRTC peer at a time, as an
# ICE-lite host on the LAN.

#@ solidify:Matter_CameraStreams,weak
#@ solidify:Matter_CameraPTZ,weak
#@ solidify:Matter_WebRTCProvider,weak

class Matter_CameraStreams : Matter_Cluster
  static var CLUSTER = 0x0551
  static var REVISION = 2
  static var FEATURES = 0x07          # Audio, Video, Snapshot
  static var ATTRIBUTES = [0, 1, 2, 4, 5, 6, 7, 0x0A, 0x0B, 0x0C, 0x0E, 0x0F, 0x10, 0x11, 0x12,
                           0x18, 0x1D, 0x1E, 0x1F, 0x20]
  static var ACCEPTED = [0x00, 0x02, 0x03, 0x06, 0x07, 0x0A, 0x0B, 0x0C]
  static var GENERATED = [0x01, 0x04, 0x08, 0x0D]
  static var WRITE_PRIVILEGES = {0x18: 4}
  static var USAGES = [3, 1]          # LiveView, Recording
  static var SAMPLE_RATES = [8000, 48000]   # captured at 8 kHz, Opus runs on a 48 kHz clock
  static var MIN_BIT_RATE = 256000
  static var MAX_BANDWIDTH = 20000000
  static var MAX_VOLUME = 254
  static var STORE = "camera"

  var provider
  var video, audio, snapshot          # allocated streams as maps by struct tag
  var priorities, next_id
  var muted, volume

  # Adds the Camera endpoint when the build has the CSI camera and its `cam` module
  static def add_to(device)
    try
      import cam
      if cam.info() == nil   return    end
    except ..
      return
    end
    var e = device.add_endpoint(0x0142, 1)
    var streams = matter.CameraStreams(e)
    e.clusters.push(streams)
    e.clusters.push(matter.CameraPTZ(e, streams))
    e.clusters.push(matter.WebRTCProvider(e, streams))
    e.clients = [0x0554]              # WebRTC Transport Requestor
  end

  def init(endpoint)
    super(self).init(endpoint)
    self.video = []
    self.audio = []
    self.snapshot = []
    self.priorities = [3]
    self.next_id = 1
    self.muted = false
    self.volume = self.MAX_VOLUME
    self.load()
  end

  # Allocations survive a restart, as the cluster requires
  def load()
    var stored = matter.store_load(self.STORE)
    var v = stored != nil ? matter.tlv(stored) : nil
    if !isinstance(v, map)    return    end
    self.video = v.find(1, [])
    self.audio = v.find(2, [])
    self.snapshot = v.find(3, [])
    self.priorities = v.find(4, [3])
    self.next_id = v.find(5, 1)
  end

  def save()
    var w = matter.Tlv().st(nil)
    var tag = 1
    for streams : [self.video, self.audio, self.snapshot]
      w.arr(tag)
      for s : streams
        w.st(nil)
        for k : 0 .. 15
          if s.find(k) != nil   w.uint(k, s[k])   end
        end
        w.close()
      end
      w.close()
      tag += 1
    end
    self.int_list(w, 4, self.priorities)
    w.uint(5, self.next_id)
    w.close()
    matter.store_save(self.STORE, w.b)
  end

  # Width, height and frame rate the camera runs at now
  static def mode()
    import cam
    var info = cam.info()
    var fps = info.find('fps', 0)
    return [info.find('width', 0), info.find('height', 0), fps > 0 ? fps : 30]
  end

  # The part of the sensor the stream shows, [x1, y1, x2, y2] in full-sensor pixels; the whole
  # window while the camera does not stream
  static def viewport()
    import cam
    var v = cam.viewport()
    if v != nil   return v    end
    var m = matter.CameraStreams.mode()
    return [0, 0, m[0], m[1]]
  end

  static def write_viewport(w, t, v)
    w.st(t)
    for k : 0 .. 3    w.uint(k, v[k])   end
    w.close()
  end

  static def resolution(w, t, width, height)
    w.st(t)
    w.uint(0, width)
    w.uint(1, height)
    w.close()
  end

  def references(kind, id)
    return self.provider != nil ? self.provider.uses(kind, id) : 0
  end

  def read(device, session, attr, w, t)
    var m = self.mode()
    if attr == 0      w.uint(t, 1)
    elif attr == 1    w.uint(t, m[0] * m[1] * m[2])
    elif attr == 2                    # the whole array, which the viewport moves in
      import cam
      var info = cam.info()
      w.st(t)
      w.uint(0, info.find('sensor_width', 0) > 0 ? info['sensor_width'] : m[0])
      w.uint(1, info.find('sensor_height', 0) > 0 ? info['sensor_height'] : m[1])
      w.uint(2, m[2])
      w.close()
    elif attr == 4    self.resolution(w, t, m[0], m[1])
    elif attr == 5
      w.arr(t)
      w.st(nil)
      w.uint(0, 0)                    # H264
      self.resolution(w, 1, m[0], m[1])
      w.uint(2, self.MIN_BIT_RATE)
      w.close()
      w.close()
    elif attr == 6    w.uint(t, 1048576)
    elif attr == 7
      w.st(t)
      w.uint(0, 1)
      self.int_list(w, 1, [0])        # Opus
      self.int_list(w, 2, self.SAMPLE_RATES)
      self.int_list(w, 3, [16])
      w.close()
    elif attr == 0x0A
      w.arr(t)
      w.st(nil)
      self.resolution(w, 0, m[0], m[1])
      w.uint(1, m[2])
      w.uint(2, 0)                    # JPEG
      w.bool(3, true)
      w.bool(4, true)
      w.close()
      w.close()
    elif attr == 0x0B   w.uint(t, self.MAX_BANDWIDTH)
    elif attr == 0x0C   w.uint(t, m[2])
    elif attr == 0x0E   self.int_list(w, t, self.USAGES)
    elif attr == 0x0F
      w.arr(t)
      for s : self.video
        w.st(nil)
        for k : 0 .. 4    w.uint(k, s[k])   end
        self.resolution(w, 5, s[5], s[6])
        self.resolution(w, 6, s[7], s[8])
        w.uint(7, s[9])
        w.uint(8, s[10])
        w.uint(9, s[11])
        w.uint(12, self.references('video', s[0]))
        w.close()
      end
      w.close()
    elif attr == 0x10
      w.arr(t)
      for s : self.audio
        w.st(nil)
        for k : 0 .. 6    w.uint(k, s[k])   end
        w.uint(7, self.references('audio', s[0]))
        w.close()
      end
      w.close()
    elif attr == 0x11
      w.arr(t)
      for s : self.snapshot
        w.st(nil)
        w.uint(0, s[0])
        w.uint(1, s[1])
        w.uint(2, s[2])
        self.resolution(w, 3, s[3], s[4])
        self.resolution(w, 4, s[5], s[6])
        w.uint(5, s[7])
        w.uint(6, 0)
        w.bool(7, true)
        w.bool(8, true)
        w.close()
      end
      w.close()
    elif attr == 0x12   self.int_list(w, t, self.priorities)
    elif attr == 0x18   self.write_viewport(w, t, self.viewport())
    elif attr == 0x1D   w.bool(t, self.muted)
    elif attr == 0x1E   w.uint(t, self.volume)
    elif attr == 0x1F   w.uint(t, self.MAX_VOLUME)
    elif attr == 0x20   w.uint(t, 1)
    else
      return false
    end
    return true
  end

  def write(device, session, attr, value)
    if attr == 0x18                   # moves the one window every stream shows
      return matter.CameraPTZ.set_viewport(value) != nil ? 0 : 0x87
    elif attr == 0x1D
      if type(value) != 'bool'    return 0x87   end
      self.muted = value
      return 0
    elif attr == 0x1E
      if type(value) != 'int' || value < 1 || value > self.MAX_VOLUME    return 0x87   end
      self.volume = value
      return 0
    end
    return super(self).write(device, session, attr, value)
  end

  static def find_stream(streams, id)
    for s : streams
      if s[0] == id   return s    end
    end
    return nil
  end

  # Gives a new stream its id, or answers with the one already allocated, since one encoder
  # serves every client
  def allocate(device, streams, attr, s, response)
    var existing = size(streams) > 0 ? streams[0] : nil
    if existing == nil
      s[0] = self.next_id
      self.next_id = (self.next_id % 0xFFFE) + 1
      streams.push(s)
      self.save()
      device.changed(self.endpoint.id, self.CLUSTER, attr)
      existing = s
    end
    return [response, matter.Tlv().st(1).uint(0, existing[0]).close()]
  end

  def deallocate(device, streams, attr, id)
    var s = self.find_stream(streams, id)
    if s == nil   return 0x8B   end
    streams.remove(streams.find(s))
    self.save()
    device.changed(self.endpoint.id, self.CLUSTER, attr)
    return 0
  end

  static def res_field(fields, tag)
    var r = fields.find(tag)
    return isinstance(r, map) && type(r.find(0)) == 'int' && type(r.find(1)) == 'int' ? r : nil
  end

  # The camera's mode must lie within the requested range
  def fits(m, low, high)
    return low[0] <= m[0] && m[0] <= high[0] && low[1] <= m[1] && m[1] <= high[1]
  end

  def invoke(device, session, command, fields, exchange)
    if !isinstance(fields, map)   return 0x85   end
    var m = self.mode()
    if command == 0x03                # VideoStreamAllocate
      var low = self.res_field(fields, 4)
      var high = self.res_field(fields, 5)
      if low == nil || high == nil || self.USAGES.find(fields.find(0)) == nil   return 0x87   end
      if fields.find(1) != 0 || fields.find(2, 0) > fields.find(3, 0) || fields.find(6, 0) > fields.find(7, 0)
        return 0x87
      end
      if !self.fits(m, [low[0], low[1]], [high[0], high[1]]) || fields.find(3, 0) < 1   return 0x87   end
      var s = {1: fields[0], 2: 0, 3: fields.find(2, 0), 4: fields[3], 5: low[0], 6: low[1], 7: high[0],
               8: high[1], 9: fields.find(6, 0), 10: fields.find(7, 0), 11: fields.find(8, 0)}
      return self.allocate(device, self.video, 0x0F, s, 0x04)
    elif command == 0x06              # VideoStreamDeallocate
      return self.deallocate(device, self.video, 0x0F, fields.find(0))
    elif command == 0x00              # AudioStreamAllocate
      if self.USAGES.find(fields.find(0)) == nil || fields.find(1) != 0 || fields.find(2) != 1 ||
         self.SAMPLE_RATES.find(fields.find(3)) == nil || fields.find(5) != 16
        return 0x87
      end
      var s = {1: fields[0], 2: 0, 3: 1, 4: fields[3], 5: fields.find(4, 0), 6: 16}
      return self.allocate(device, self.audio, 0x10, s, 0x01)
    elif command == 0x02              # AudioStreamDeallocate
      return self.deallocate(device, self.audio, 0x10, fields.find(0))
    elif command == 0x07              # SnapshotStreamAllocate
      var low = self.res_field(fields, 2)
      var high = self.res_field(fields, 3)
      if fields.find(0) != 0 || low == nil || high == nil || !self.fits(m, [low[0], low[1]], [high[0], high[1]])
        return 0x87
      end
      var s = {1: 0, 2: fields.find(1, 1), 3: low[0], 4: low[1], 5: high[0], 6: high[1], 7: fields.find(4, 90)}
      return self.allocate(device, self.snapshot, 0x11, s, 0x08)
    elif command == 0x0A              # SnapshotStreamDeallocate
      return self.deallocate(device, self.snapshot, 0x11, fields.find(0))
    elif command == 0x0B              # SetStreamPriorities
      var usages = fields.find(0)
      if !isinstance(usages, list)    return 0x87   end
      for usage : usages
        if self.USAGES.find(usage) == nil   return 0x87   end
      end
      self.priorities = usages
      self.save()
      device.changed(self.endpoint.id, self.CLUSTER, 0x12)
      return 0
    elif command == 0x0C              # CaptureSnapshot
      return self.capture(session, fields, m)
    end
    return 0x81
  end

  # One JPEG frame in the response, which needs a TCP session for any real image
  def capture(session, fields, m)
    import cam
    if size(self.snapshot) == 0   return 0xCB   end
    var id = fields.find(0)
    if id != nil && self.find_stream(self.snapshot, id) == nil    return 0x8B   end
    var data = cam.get_image()
    if data == nil    return 0x01   end
    if size(data) + 96 > matter.max_message(session.ctx)    return 0x89   end
    var w = matter.Tlv().st(1)
    w.bin(0, data)
    w.uint(1, 0)
    self.resolution(w, 2, m[0], m[1])
    w.close()
    return [0x0D, w]
  end
end

# Digital PTZ only: the viewport moves the sensor window. The camera crops and has no scaler,
# so a viewport keeps the window's size; a zoom is a constraint error. One window serves every
# stream, so all allocated video streams share its viewport.
class Matter_CameraPTZ : Matter_Cluster
  static var CLUSTER = 0x0552
  static var FEATURES = 0x01          # DigitalPTZ
  static var ATTRIBUTES = [0x03]
  static var ACCEPTED = [0x05, 0x06]

  var streams, shown

  def init(endpoint, streams)
    super(self).init(endpoint)
    self.streams = streams
  end

  # Moves the window to a ViewportStruct map; the applied viewport, or nil
  static def set_viewport(value)
    import cam
    if !isinstance(value, map)    return nil    end
    var v = []
    for k : 0 .. 3
      if type(value.find(k)) != 'int'   return nil    end
      v.push(value[k])
    end
    return cam.set_viewport(v[0], v[1], v[2], v[3])
  end

  def read(device, session, attr, w, t)
    if attr != 0x03   return false    end
    var v = self.streams.viewport()
    w.arr(t)
    for s : self.streams.video
      w.st(nil)
      w.uint(0, s[0])
      self.streams.write_viewport(w, 1, v)
      w.close()
    end
    w.close()
    return true
  end

  # Reports a move from WcPan, WcPosition or the web UI
  def poll(device)
    var v = self.streams.viewport()
    if v == self.shown    return    end
    if self.shown != nil
      device.changed(self.endpoint.id, self.CLUSTER, 0x03)
      device.changed(self.endpoint.id, self.streams.CLUSTER, 0x18)
    end
    self.shown = v
  end

  def invoke(device, session, command, fields, exchange)
    if !isinstance(fields, map)   return 0x85   end
    if self.streams.find_stream(self.streams.video, fields.find(0)) == nil    return 0x8B   end
    var applied
    if command == 0x05                # DPTZSetViewport
      applied = self.set_viewport(fields.find(1))
    elif command == 0x06              # DPTZRelativeMove
      import cam
      var zoom = fields.find(3, 0)
      if type(zoom) != 'int' || zoom < -100 || zoom > 100 || zoom != 0    return 0x87   end
      var dx = fields.find(1, 0), dy = fields.find(2, 0)
      if type(dx) != 'int' || type(dy) != 'int'   return 0x87   end
      var v = cam.viewport()
      var info = cam.info()
      if v == nil   return 0x87   end
      # Stops at the sensor's edges, as the SDK's example camera does
      var width = v[2] - v[0], height = v[3] - v[1]
      var x = v[0] + dx, y = v[1] + dy
      x = x < 0 ? 0 : (x > info['sensor_width'] - width ? info['sensor_width'] - width : x)
      y = y < 0 ? 0 : (y > info['sensor_height'] - height ? info['sensor_height'] - height : y)
      applied = cam.set_viewport(x, y, x + width, y + height)
    else
      return 0x81
    end
    if applied == nil   return 0x87   end
    self.poll(device)
    return 0
  end
end

class Matter_WebRTCProvider : Matter_Cluster
  static var CLUSTER = 0x0553
  static var REVISION = 2
  static var ATTRIBUTES = [0]
  static var READ_PRIVILEGES = {0: 4}
  static var ACCEPTED = [0x00, 0x02, 0x04, 0x05, 0x06]
  static var GENERATED = [0x01, 0x03]
  static var REQUESTOR = 0x0554
  static var WEBRTC_SESSION = 3       # WcSession
  static var REPLACED = 4             # WebRTCEndReasonEnum

  var streams, sessions, next_id

  def init(endpoint, streams)
    super(self).init(endpoint)
    self.streams = streams
    streams.provider = self
    self.sessions = []
    self.next_id = 1
  end

  # How many sessions carry a stream, for its ReferenceCount
  def uses(kind, id)
    var count = 0
    for s : self.sessions
      if s.find(kind) == id   count += 1    end
    end
    return count
  end

  def read(device, session, attr, w, t)
    if attr != 0    return false    end
    w.arr(t)
    for s : self.sessions
      if session.fabric != nil && s['fabric'] != session.fabric    continue    end
      w.st(nil)
      w.uint(0, s['id'])
      w.u64(1, s['peer'])
      w.uint(2, s['ep'])
      w.uint(3, s['usage'])
      if s['video'] != nil    w.uint(4, s['video'])   else w.null(4)    end
      if s['audio'] != nil    w.uint(5, s['audio'])   else w.null(5)    end
      w.bool(6, false)
      w.uint(0xFE, s['fabric'].index)
      w.close()
    end
    w.close()
    return true
  end

  # A session of the invoking controller, or nil
  def find_session(session, id)
    for s : self.sessions
      if s['id'] == id && s['fabric'] == session.fabric && s['peer'] == session.peer_node   return s   end
    end
    return nil
  end

  # A stream named by the offer, the first allocated one for null, or none when absent
  def pick_stream(fields, tag, streams)
    if !fields.contains(tag)    return [true, nil]    end
    var id = fields[tag]
    if id == nil    return [true, size(streams) > 0 ? streams[0][0] : nil]   end
    return [self.streams.find_stream(streams, id) != nil, id]
  end

  # Tells a requestor that its session ended, through the client
  def send_end(device, s, reason)
    var via
    for session : device.sessions
      if session.fabric == s['fabric'] && session.peer_node == s['peer']   via = session   end
    end
    if via == nil || device.client == nil   return    end
    var w = matter.Tlv().st(1).uint(0, s['id']).uint(1, reason).close()
    device.client.invoke(via, s['ep'], self.REQUESTOR, 0x03, w, false, nil)
  end

  def invoke(device, session, command, fields, exchange)
    if !isinstance(fields, map)   return 0x85   end
    if session.fabric == nil    return 0xCB   end
    if command == 0x00    return self.solicit_offer(device, session, fields)   end
    if command == 0x02    return self.provide_offer(device, session, fields)   end
    if command == 0x04    return self.provide_answer(device, session, fields)   end
    var s = self.find_session(session, fields.find(0))
    if s == nil   return 0x8B   end
    if command == 0x05                # ProvideICECandidates: ICE-lite learns the peer from STUN
      return 0
    elif command == 0x06              # EndSession
      self.sessions.remove(self.sessions.find(s))
      device.changed(self.endpoint.id, self.CLUSTER, 0)
      return 0
    end
    return 0x81
  end

  # A new session from the fields of SolicitOffer or ProvideOffer, which start at `first`
  def new_session(session, fields, first)
    var usage = fields.find(first)
    var origin = fields.find(first + 1)
    if type(usage) != 'int' || type(origin) != 'int'   return 0x85   end
    if self.streams.USAGES.find(usage) == nil   return 0x87   end
    var video = self.pick_stream(fields, first + 2, self.streams.video)
    var audio = self.pick_stream(fields, first + 3, self.streams.audio)
    if !video[0] || !audio[0]   return 0x87   end
    return {'id': self.next_id, 'fabric': session.fabric, 'peer': session.peer_node, 'ep': origin,
            'usage': usage, 'video': video[1], 'audio': audio[1]}
  end

  # Makes a session current. The driver serves one peer, so an earlier one has lost its media.
  def take_over(device, s)
    for other : self.sessions.copy()
      if other != s
        self.send_end(device, other, self.REPLACED)
        self.sessions.remove(self.sessions.find(other))
      end
    end
    if self.sessions.find(s) == nil
      self.sessions.push(s)
      self.next_id = (self.next_id % 0xFFFE) + 1
    end
    device.changed(self.endpoint.id, self.CLUSTER, 0)
  end

  # Sends this side's SDP to the requestor: Offer (0) or Answer (1)
  def send_sdp(device, session, s, command, sdp)
    var w = matter.Tlv().st(1).uint(0, s['id']).str(1, sdp).close()
    device.client.invoke(session, s['ep'], self.REQUESTOR, command, w, true, def (status, response)
      if status != 0    log("MTR: WebRTC SDP not delivered" + (status != nil ? ", status " + str(status) : ""), 2)   end
    end)
  end

  static def stream_fields(w, s, first)
    if s['video'] != nil    w.uint(first, s['video'])       else w.null(first)        end
    if s['audio'] != nil    w.uint(first + 1, s['audio'])   else w.null(first + 1)    end
  end

  def solicit_offer(device, session, fields)
    import cam
    var s = self.new_session(session, fields, 0)
    if type(s) == 'int'   return s    end
    if cam.info().find('session') != self.WEBRTC_SESSION    return 0xCB   end
    var offer = cam.webrtc_offer()
    if offer == nil   return 0x01   end
    s['offered'] = true
    self.take_over(device, s)
    self.send_sdp(device, session, s, 0x00, offer)
    var w = matter.Tlv().st(1)
    w.uint(0, s['id'])
    w.bool(1, false)                  # the offer is ready, not deferred
    self.stream_fields(w, s, 2)
    w.close()
    return [0x01, w]
  end

  def provide_answer(device, session, fields)
    import cam
    var s = self.find_session(session, fields.find(0))
    if s == nil   return 0x8B   end
    var sdp = fields.find(1)
    if type(sdp) != 'string' || !s.find('offered', false)    return 0x87   end
    if !cam.webrtc_accept(sdp)   return 0x01   end
    s['offered'] = false
    return 0
  end

  def provide_offer(device, session, fields)
    import cam
    var sdp = fields.find(1)
    if type(sdp) != 'string'    return 0x87   end
    var id = fields.find(0)
    var s
    if id != nil
      s = self.find_session(session, id)
      if s == nil   return 0x8B   end
    else
      s = self.new_session(session, fields, 2)
      if type(s) == 'int'   return s    end
    end
    if cam.info().find('session') != self.WEBRTC_SESSION    return 0xCB   end
    var answer = cam.webrtc_answer(sdp)
    if answer == nil    return 0x01   end
    s['offered'] = false
    self.take_over(device, s)
    self.send_sdp(device, session, s, 0x01, answer)
    var w = matter.Tlv().st(1)
    w.uint(0, s['id'])
    self.stream_fields(w, s, 1)
    w.close()
    return [0x03, w]
  end
end
