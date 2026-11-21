# The device as a client: commands it sends to a controller, such as a camera's WebRTC answer,
# and the CASE handshake it starts when no session can carry them. Only with USE_MATTER_TCP:
# large messages need a TCP session, which this device opens itself.

#@ solidify:Matter_CaseInitiator,weak
#@ solidify:Matter_Client,weak

# CASE as the initiator, the mirror of Matter_Handshake's responder steps
class Matter_CaseInitiator
  var device, ctx, exchange, fabric, node, done
  var msg1, msg2, msg3
  var priv, pub, peer_pub, shared
  var local_session, peer_session, peer_noc

  # done(session) runs once, with nil when the handshake failed
  def init(device, ctx, fabric, node, done)
    self.device = device
    self.ctx = ctx
    self.fabric = fabric
    self.node = node
    self.done = done
  end

  def send(opcode, payload)
    return matter.send(self.ctx, self.exchange, true, 0, opcode, payload, true)
  end

  def start()
    import crypto
    var f = self.fabric
    var ec = crypto.EC_P256()
    var random = crypto.random(32)
    self.priv = crypto.random(32)
    self.pub = ec.public_key(self.priv)
    self.local_session = matter.new_session_id()
    self.exchange = crypto.random(2).get(0, 2)
    var h = crypto.HMAC_SHA256(f.group_key())
    h.update(random + f.root_pub() + f.fabric_id + self.node)
    var w = matter.Tlv().st(nil)
    w.bin(1, random)
    w.uint(2, self.local_session)
    w.bin(3, h.out())
    w.bin(4, self.pub)
    matter.Handshake.session_params(w, 5)
    w.close()
    self.msg1 = w.b
    if !self.send(0x30, self.msg1)    self.finish(nil)    end
  end

  def message(exchange, opcode, payload)
    if exchange != self.exchange    return    end
    if opcode == 0x31
      self.sigma2(payload)
    elif opcode == 0x40
      self.status(payload)
    end
  end

  def finish(session)
    var done = self.done
    self.done = nil
    self.device.drop_handshake(self.ctx)
    if done != nil    done(session)   end
  end

  # Checks the responder and answers with Sigma3
  def sigma2(payload)
    import crypto
    var f = self.fabric
    var v = matter.tlv(payload)
    if !isinstance(v, map) || self.msg2 != nil   return self.finish(nil)   end
    var ec = crypto.EC_P256()
    self.peer_session = v.find(2)
    self.peer_pub = v.find(3)
    var sealed = v.find(4)
    if self.peer_session == nil || self.peer_pub == nil || sealed == nil || size(sealed) < 16
      return self.finish(nil)
    end
    self.shared = ec.shared_key(self.priv, self.peer_pub)
    var transcript = crypto.SHA256().update(self.msg1).out()
    var salt = f.group_key() + v.find(1, bytes()) + self.peer_pub + transcript
    var s2k = crypto.HKDF_SHA256().derive(self.shared, salt, bytes().fromstring(matter.Handshake.S2K_INFO), 16)
    var encrypted = sealed[0..-17]
    var aes = crypto.AES_CCM(s2k, bytes().fromstring(matter.Handshake.TBE2_NONCE), bytes(), size(encrypted), 16)
    var plain = aes.decrypt(encrypted)
    if aes.tag() != sealed[-16..]   return self.finish(nil)   end
    var tbe = matter.tlv(plain)
    if !isinstance(tbe, map)    return self.finish(nil)   end
    var noc = matter.tlv(tbe.find(1, bytes()))
    if !isinstance(noc, map) || !isinstance(noc.find(6), map)   return self.finish(nil)   end
    var dn = noc[6]
    if !self.device.chain_accepted(tbe[1], tbe.find(2), f.rcac) ||
       matter.Fabric.id64(dn.find(21)) != f.fabric_id || matter.Fabric.id64(dn.find(17)) != self.node
      log("MTR: CASE responder is not the expected node", 2)
      return self.finish(nil)
    end
    var tbs = matter.Tlv().st(nil)
    tbs.bin(1, tbe[1])
    if tbe.find(2)   tbs.bin(2, tbe[2])   end
    tbs.bin(3, self.peer_pub)
    tbs.bin(4, self.pub)
    tbs.close()
    if !ec.ecdsa_verify_sha256(noc.find(9), tbs.b, tbe.find(3))
      log("MTR: CASE responder signature failed", 2)
      return self.finish(nil)
    end
    self.peer_noc = tbe[1]
    self.msg2 = payload

    var tbs3 = matter.Tlv().st(nil)
    tbs3.bin(1, f.noc)
    if f.icac   tbs3.bin(2, f.icac)   end
    tbs3.bin(3, self.pub)
    tbs3.bin(4, self.peer_pub)
    tbs3.close()
    var tbe3 = matter.Tlv().st(nil)
    tbe3.bin(1, f.noc)
    if f.icac   tbe3.bin(2, f.icac)   end
    tbe3.bin(3, ec.ecdsa_sign_sha256(f.key, tbs3.b))
    tbe3.close()
    transcript = crypto.SHA256().update(self.msg1).update(self.msg2).out()
    var s3k = crypto.HKDF_SHA256().derive(self.shared, f.group_key() + transcript, bytes().fromstring(matter.Handshake.S3K_INFO), 16)
    aes = crypto.AES_CCM(s3k, bytes().fromstring(matter.Handshake.TBE3_NONCE), bytes(), size(tbe3.b), 16)
    var encrypted3 = aes.encrypt(tbe3.b)
    var w = matter.Tlv().st(nil)
    w.bin(1, encrypted3 + aes.tag())
    w.close()
    self.msg3 = w.b
    if !self.send(0x32, self.msg3)    self.finish(nil)    end
  end

  # The responder's verdict on Sigma3: success installs the session
  def status(payload)
    import crypto
    if self.msg3 == nil || size(payload) < 2 || payload.get(0, 2) != 0
      log("MTR: CASE to a controller refused", 2)
      return self.finish(nil)
    end
    var f = self.fabric
    var transcript = crypto.SHA256().update(self.msg1).update(self.msg2).update(self.msg3).out()
    var keys = crypto.HKDF_SHA256().derive(self.shared, f.group_key() + transcript, bytes().fromstring(matter.Handshake.SE_INFO), 48)
    # The initiator encrypts with I2R and decrypts with R2I
    var session = self.device.add_session(self.ctx, self.local_session, self.peer_session, keys[16..31], keys[0..15],
      keys[32..47], self.node, f.node_id, f)
    if session != nil   session.cats = matter.cert_cats(self.peer_noc)   end
    log("MTR: CASE session to a controller on fabric " + str(f.index), 2)
    self.finish(session)
  end
end

# Commands this device invokes on other nodes, matched to their responses by exchange
class Matter_Client
  static var TIMEOUT = 30000

  var device
  var requests            # waiting, sent, or waiting for a handshake
  var connecting          # handshakes by fabric index and node

  def init(device)
    self.device = device
    self.requests = []
    self.connecting = {}
  end

  # Invokes a command on the node of a session. fields is a matter.Tlv holding st(1); large
  # needs a TCP session. callback(status, fields) gets nil for the status when it failed.
  def invoke(session, ep, cluster, command, fields, large, callback)
    if session == nil || session.fabric == nil    return false    end
    self.requests.push({'fabric': session.fabric, 'node': session.peer_node, 'via': session.ctx,
      'ep': ep, 'cluster': cluster, 'command': command, 'fields': fields != nil ? fields.b : nil,
      'large': large, 'callback': callback, 'deadline': matter.now() + self.TIMEOUT})
    return true
  end

  static def peer_key(r)
    return str(r['fabric'].index) + ":" + r['node'].tohex()
  end

  # A session of the node that can carry the request
  def session_for(r)
    for s : self.device.sessions
      if s.fabric != r['fabric'] || s.peer_node != r['node'] || matter.max_message(s.ctx) == 0   continue    end
      if !r['large'] || matter.transport(s.ctx) == 2    return s    end
    end
    return nil
  end

  def send(r, session)
    var im = self.device.im
    im.next_exchange = (im.next_exchange + 1) & 0xFFFF
    var exchange = im.next_exchange
    var w = matter.Tlv().st(nil)
    w.bool(0, false)
    w.bool(1, false)
    w.arr(2)
    w.st(nil)
    w.lst(0)
    w.uint(0, r['ep'])
    w.uint(1, r['cluster'])
    w.uint(2, r['command'])
    w.close()
    if r['fields'] != nil   w.raw(r['fields'])    else w.st(1).close()    end
    w.close()
    w.close()
    w.uint(0xFF, matter.Device.IM_REVISION)
    w.close()
    if size(w.b) > matter.max_message(session.ctx) || !matter.send(session.ctx, exchange, true, 1, 0x08, w.b, true)
      return self.finish(r, nil, nil)
    end
    r['key'] = im.key(session.ctx, exchange)
    r['ctx'] = session.ctx
  end

  # Opens a CASE session over TCP to the node, through the address of the session it came from
  def connect(r)
    var k = self.peer_key(r)
    if self.connecting.contains(k)    return    end
    var ctx = matter.connect(r['via'])
    if ctx < 0
      log("MTR: No TCP connection to the controller", 2)
      return self.finish(r, nil, nil)
    end
    var handshake = matter.CaseInitiator(self.device, ctx, r['fabric'], r['node'], / session -> self.established(k, session))
    self.connecting[k] = handshake
    self.device.handshakes[ctx & 0xFF] = handshake
    handshake.start()
  end

  def established(k, session)
    self.connecting.remove(k)
    if session != nil   return    end
    for r : self.requests.copy()
      if r.find('key') == nil && self.peer_key(r) == k    self.finish(r, nil, nil)    end
    end
  end

  def finish(r, status, fields)
    var i = self.requests.find(r)
    if i != nil   self.requests.remove(i)   end
    var callback = r['callback']
    if callback != nil    callback(status, fields)    end
  end

  # Takes an InvokeResponse or StatusResponse on an exchange this client started
  def response(session, exchange, opcode, v)
    var k = self.device.im.key(session.ctx, exchange)
    for r : self.requests
      if r.find('key') != k   continue    end
      var status = 1
      var fields
      if opcode == 0x09
        var items = v.find(1, [])
        var item = size(items) > 0 ? items[0] : {}
        if item.find(0) != nil
          status = 0
          fields = item[0].find(1)
        elif item.find(1) != nil
          status = item[1].find(1, {}).find(0, 1)
        end
      elif opcode == 0x01
        status = v.find(0, 1)
      end
      self.finish(r, status, fields)
      return true
    end
    return false
  end

  # Reliable delivery gave up on a session: what was sent on it is lost
  def failed(ctx)
    for r : self.requests.copy()
      if r.find('ctx') == ctx   self.finish(r, nil, nil)    end
    end
  end

  def tick(now)
    for r : self.requests.copy()
      if (now - r['deadline']) >= 0
        self.finish(r, nil, nil)
      elif r.find('key') == nil
        var session = self.session_for(r)
        if session != nil   self.send(r, session)   else self.connect(r)    end
      end
    end
  end
end
