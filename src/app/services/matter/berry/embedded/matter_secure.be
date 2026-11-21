# PASE and CASE responder. Protocol steps follow Tasmota's Matter_Commissioning_Context.be;
# framing, reliability and session keys are native.
# Adapted from Tasmota lib/libesp32/berry_matter/src/embedded/Matter_Commissioning_Context.be
# Source pin: f27e4077212dd9ae8e3ada3fa96b6040926e059b.
# Copyright (C) 2023  Stephan Hadinger & Theo Arends
# SPDX-License-Identifier: GPL-3.0-or-later

#@ solidify:Matter_Handshake,weak

class Matter_Handshake
  static var CONTEXT_PREFIX = "CHIP PAKE V1 Commissioning"
  static var SE_INFO = "SessionKeys"
  static var S2K_INFO = "Sigma2"
  static var S3K_INFO = "Sigma3"
  static var TBE2_NONCE = "NCASE_Sigma2N"
  static var TBE3_NONCE = "NCASE_Sigma3N"

  var device
  var ctx, exchange
  var peer_session, local_session
  var msg1, msg2
  var cA, Ke
  var fabric, shared, pub, peer_pub

  def init(device, ctx)
    self.device = device
    self.ctx = ctx
  end

  # Dispatches one Secure Channel message of this handshake
  def message(exchange, opcode, payload)
    self.exchange = exchange
    var v = opcode == 0x40 ? nil : matter.tlv(payload)
    if opcode != 0x40 && !isinstance(v, map)
      return self.status(1, 2)
    end
    if   opcode == 0x20   self.pbkdf_request(v, payload)
    elif opcode == 0x22   self.pake1(v)
    elif opcode == 0x24   self.pake3(v)
    elif opcode == 0x30   self.sigma1(v, payload)
    elif opcode == 0x32   self.sigma3(v, payload)
    end
  end

  def send(opcode, payload)
    return matter.send(self.ctx, self.exchange, false, 0, opcode, payload, true)
  end

  # StatusReport on the handshake exchange: general code 0 success, 1 failure
  def status(general, code)
    var r = bytes()
    r.add(general, 2)
    r.add(0, 4)
    r.add(code, 4)
    return self.send(0x40, r)
  end

  # Session parameters, also read by peers to learn our revisions
  static def session_params(w, t)
    w.st(t)
    w.uint(1, 500)
    w.uint(2, 300)
    w.uint(3, 4000)
    w.uint(4, matter.Device.DATA_MODEL_REVISION)
    w.uint(5, matter.Device.IM_REVISION)
    w.uint(6, matter.Device.SPECIFICATION_VERSION)
    w.uint(7, 1)
    w.close()
  end

  # Retransmits at the pace the peer announced
  def peer_timing(params)
    if isinstance(params, map) && params.find(2) != nil
      matter.peer_active_interval(self.ctx, params[2])
    end
  end

  def pbkdf_request(v, payload)
    import crypto
    var window = self.device.window
    if window == nil || v.find(3, 0) != 0
      return self.status(1, 2)
    end
    self.msg1 = payload
    self.peer_session = v.find(2)
    self.local_session = matter.new_session_id()
    self.peer_timing(v.find(5))
    var w = matter.Tlv().st(nil)
    w.bin(1, v.find(1))
    w.bin(2, crypto.random(32))
    w.uint(3, self.local_session)
    w.st(4)
    w.uint(1, window['iterations'])
    w.bin(2, window['salt'])
    w.close()
    self.session_params(w, 5)
    w.close()
    self.msg2 = w.b
    self.send(0x21, self.msg2)
  end

  def pake1(v)
    import crypto
    var window = self.device.window
    if window == nil || self.msg2 == nil
      return self.status(1, 2)
    end
    var pA = v.find(1)
    var spake = crypto.SPAKE2P_Matter(window['w0'], nil, window['L'])
    spake.compute_pB(crypto.random(32))
    spake.compute_ZV_verifier(pA)
    var hash = crypto.SHA256()
    hash.update(bytes().fromstring(self.CONTEXT_PREFIX))
    hash.update(self.msg1)
    hash.update(self.msg2)
    spake.pA = pA
    spake.set_context(hash.out())
    spake.compute_TT_hash(true)
    self.cA = spake.cA
    self.Ke = spake.Ke
    var w = matter.Tlv().st(nil)
    w.bin(1, spake.pB)
    w.bin(2, spake.cB)
    w.close()
    self.send(0x23, w.b)
  end

  def pake3(v)
    import crypto
    if self.cA == nil || v.find(1) != self.cA
      return self.status(1, 2)
    end
    var keys = crypto.HKDF_SHA256().derive(self.Ke, bytes(), bytes().fromstring(self.SE_INFO), 48)
    self.status(0, 0)
    var none = bytes("0000000000000000")
    self.device.add_session(self.ctx, self.local_session, self.peer_session, keys[0..15], keys[16..31],
      keys[32..47], none, none, nil)
    self.device.drop_handshake(self.ctx)
  end

  # Finds the fabric a destination identifier was computed for
  def find_fabric(destination, random)
    import crypto
    for f : self.device.fabrics
      var h = crypto.HMAC_SHA256(f.group_key())
      h.update(random + f.root_pub() + f.fabric_id + f.node_id)
      if h.out() == destination
        return f
      end
    end
    return nil
  end

  # Resumption is not offered: no session state survives, so it always takes the full handshake
  def sigma1(v, payload)
    import crypto
    var random = v.find(1)
    var ec = crypto.EC_P256()
    self.fabric = self.find_fabric(v.find(3), random)
    if self.fabric == nil
      log("MTR: CASE request for an unknown fabric", 2)
      return self.status(1, 1)        # NoSharedTrustRoots
    end
    log("MTR: CASE request for fabric " + str(self.fabric.index), 2)
    var f = self.fabric
    self.peer_session = v.find(2)
    self.local_session = matter.new_session_id()
    self.peer_pub = v.find(4)
    self.peer_timing(v.find(5))
    self.msg1 = payload
    var priv = crypto.random(32)
    self.pub = ec.public_key(priv)
    self.shared = ec.shared_key(priv, self.peer_pub)
    var responder_random = crypto.random(32)

    var tbs = matter.Tlv().st(nil)
    tbs.bin(1, f.noc)
    if f.icac   tbs.bin(2, f.icac)   end
    tbs.bin(3, self.pub)
    tbs.bin(4, self.peer_pub)
    tbs.close()
    var signature = ec.ecdsa_sign_sha256(f.key, tbs.b)

    var tbe = matter.Tlv().st(nil)
    tbe.bin(1, f.noc)
    if f.icac   tbe.bin(2, f.icac)   end
    tbe.bin(3, signature)
    tbe.bin(4, crypto.random(16))
    tbe.close()

    var transcript = crypto.SHA256().update(self.msg1).out()
    var salt = f.group_key() + responder_random + self.pub + transcript
    var s2k = crypto.HKDF_SHA256().derive(self.shared, salt, bytes().fromstring(self.S2K_INFO), 16)
    var aes = crypto.AES_CCM(s2k, bytes().fromstring(self.TBE2_NONCE), bytes(), size(tbe.b), 16)
    var encrypted = aes.encrypt(tbe.b)
    encrypted = encrypted + aes.tag()

    var w = matter.Tlv().st(nil)
    w.bin(1, responder_random)
    w.uint(2, self.local_session)
    w.bin(3, self.pub)
    w.bin(4, encrypted)
    self.session_params(w, 5)
    w.close()
    self.msg2 = w.b
    self.send(0x31, self.msg2)
  end

  def sigma3(v, payload)
    import crypto
    var f = self.fabric
    if f == nil || self.msg2 == nil
      return self.status(1, 2)
    end
    var sealed = v.find(1)
    if size(sealed) < 16
      return self.status(1, 2)
    end
    var transcript = crypto.SHA256().update(self.msg1).update(self.msg2).out()
    var s3k = crypto.HKDF_SHA256().derive(self.shared, f.group_key() + transcript, bytes().fromstring(self.S3K_INFO), 16)
    var encrypted = sealed[0..-17]
    var aes = crypto.AES_CCM(s3k, bytes().fromstring(self.TBE3_NONCE), bytes(), size(encrypted), 16)
    var plain = aes.decrypt(encrypted)
    if aes.tag() != sealed[-16..]
      return self.status(1, 2)
    end
    var tbe = matter.tlv(plain)
    if !isinstance(tbe, map)
      return self.status(1, 2)
    end
    var noc = matter.tlv(tbe.find(1, bytes()))
    if !isinstance(noc, map) || !isinstance(noc.find(6), map)
      return self.status(1, 2)
    end
    var dn = noc[6]
    if !self.device.chain_accepted(tbe[1], tbe.find(2), f.rcac)
      return self.status(1, 2)
    end
    if matter.Fabric.id64(dn.find(21)) != f.fabric_id
      return self.status(1, 2)
    end
    var tbs = matter.Tlv().st(nil)
    tbs.bin(1, tbe[1])
    if tbe.find(2)   tbs.bin(2, tbe[2])   end
    tbs.bin(3, self.peer_pub)
    tbs.bin(4, self.pub)
    tbs.close()
    if !crypto.EC_P256().ecdsa_verify_sha256(noc.find(9), tbs.b, tbe.find(3))
      return self.status(1, 2)
    end
    transcript = crypto.SHA256().update(self.msg1).update(self.msg2).update(payload).out()
    var keys = crypto.HKDF_SHA256().derive(self.shared, f.group_key() + transcript, bytes().fromstring(self.SE_INFO), 48)
    self.status(0, 0)
    var session = self.device.add_session(self.ctx, self.local_session, self.peer_session, keys[0..15], keys[16..31],
      keys[32..47], matter.Fabric.id64(dn.find(17)), f.node_id, f)
    if session != nil   session.cats = matter.cert_cats(tbe[1])   end
    log("MTR: CASE session established on fabric " + str(f.index), 2)
    self.device.save_sessions(f)
    self.device.drop_handshake(self.ctx)
  end
end
