# Adapted from Tasmota lib/libesp32/berry_tasmota/src/embedded/crypto_spake2p_matter.be
# Source pin: 48da93dca8728c4c246b33a2d7f23c0ecaa056f0.
# SPDX-License-Identifier: GPL-3.0-or-later
# Production class solidified as crypto.SPAKE2P_Matter; vectors live in ../tests/.
# SPAKE2+ with Matter 1.0 specification
#
# Real name is SPAKE2+-P256-SHA256-HKDF-SHA256-HMAC-SHA256
# but we'll use SPAKE2P_Matter as a shorter name
#
# To start SPAKE2+, we need either w0/w1 for the prover (Matter Gateway)
# or w0/L for the verifier (end-device).
# The end-device can store in flash pre-computed values for w0/L.
#
# In this class we consider w0/w1/L to be computed before using this class

#@ solidify:SPAKE2P_Matter,weak

class SPAKE2P_Matter
  # computed values from above
  var w0                                  # w0
  var w1
  var L
  var pA, pB, Z, V
  var x, y
  var Kmain
  var KcA, KcB, K_shared
  var Ke                                  # specific to Matter
  var cA, cB
  # Matter values for key generation
  var A, B, Context
  # values from the Matter specification
  static var CRYPTO_GROUP_SIZE_BYTES = 32
  static var CRYPTO_W_SIZE_BYTES = 40         # CRYPTO_GROUP_SIZE_BYTES + 8
  static var spake_M_hex = "04886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f5ff355163e43ce224e0b0e65ff02ac8e5c7be09419c785e0ca547d55a12e2d20"
  static var spake_N_hex = "04d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b4907d60aa6bfade45008a636337f5168c64d9bd36034808cd564490b1e656edbe7"
  var M, N                                # decoded uncompressed P-256 points

  # Initialize the SPAKE2+ class with required value.
  # We need either w0/w1 for the gateway or w0/L for the end-device
  def init(w0, w1, L)
    self.M = bytes().fromhex(self.spake_M_hex)
    self.N = bytes().fromhex(self.spake_N_hex)
    # set w0/w1/L
    self.w0 = w0
    self.w1 = w1
    self.L = L
  end

  # compute the shared values pA (done by Matter gateway)
  # w0 must be known
  # x is a random 32 bytes vector. It does not need to be lower than the order (we take the modulus anyways)
  #
  # if `x` is nil, we take a random vector, otherwise it must be bytes(32)
  def compute_pA(x)
    import crypto
    if x == nil   x = crypto.random(32) end
    var ec = crypto.EC_P256()
    self.x = ec.mod(x)
    self.pA = ec.muladd(self.w0, self.M, self.x, bytes() #- empty means generator -#)    # compute x*P+w0*M
    return self.pA
  end

  # compute the shared values pB (done by Matter end-device)
  # w0 must be known
  # y is a random 32 bytes vector. It does not need to be lower than the order (we take the modulus anyways)
  def compute_pB(y)
    import crypto
    if y == nil   y = crypto.random(32) end
    var ec = crypto.EC_P256()
    self.y = ec.mod(y)
    self.pB = ec.muladd(self.w0, self.N, self.y, bytes() #- empty means generator -#)    # compute y*P+w0*M
    return self.pB
  end

  # compute Z and V for the Prover (Gateway), receiving pB from the verifier (end-device)
  # pB must be bytes(65)
  # x must be known
  def compute_ZV_prover(pB)
    import crypto
    var ec = crypto.EC_P256()
    self.pB = pB
    # EC_P256.muladd validates encoding, coordinate range and curve membership,
    # and rejects a subtraction whose result is the point at infinity.
    # we need a subtraction so we negate w0
    var w0_neg = ec.neg(self.w0)
    var t_inner = ec.muladd(bytes("01"), self.pB, w0_neg, self.N)    # compute 1*pB-w0*N
    self.Z = ec.mul(self.x, t_inner)
    self.V = ec.mul(self.w1, t_inner)
  end

  # compute Z and V for the Verifier (end-device), receiving pA from the prover (Gateway)
  # pA must be bytes(65)
  # y must be known
  def compute_ZV_verifier(pA)
    import crypto
    var ec = crypto.EC_P256()
    self.pA = pA
    # EC_P256.muladd validates encoding, coordinate range and curve membership,
    # and rejects a subtraction whose result is the point at infinity.
    # we need a subtraction so we negate w0
    var w0_neg = ec.neg(self.w0)
    var t_inner = ec.muladd(bytes("01"), self.pA, w0_neg, self.M)    # compute 1*pA-w0*M
    self.Z = ec.mul(self.y, t_inner)
    self.V = ec.mul(self.y, self.L)
  end

  # Set context, prover and verifier identities (last 2 can be empty)
  def set_context(context, prover, verifier)
    if prover == nil    prover   = bytes() end
    if verifier == nil  verifier = bytes() end
    self.Context = context
    self.A = prover
    self.B = verifier
  end

  # Need to know "Context, pA, pB, Z, V, w0"
  #
  # Arg:
  #  matter_specific: if set to `true`, uses only half of the hash as implemented
  #                   in reference code, but not compliant with SPAKE2+
  def compute_TT_hash(matter_specific)
    class SPAKE_Hasher
      var hash
      def init()
        import crypto
        self.hash = crypto.SHA256()
      end
      # add v (bytes) to the hash
      def add_item(v)
        var len = v.size()
        var len_bin = bytes().add(len, 4).add(0, 4)
        self.hash.update(len_bin)
        self.hash.update(v)
      end
      def out()
        return self.hash.out()
      end
    end
    import crypto
    var hasher = SPAKE_Hasher()

    # Context
    hasher.add_item(self.Context)
    hasher.add_item(self.A)
    hasher.add_item(self.B)
    hasher.add_item(self.M)
    hasher.add_item(self.N)
    hasher.add_item(self.pA)
    hasher.add_item(self.pB)
    hasher.add_item(self.Z)
    hasher.add_item(self.V)
    hasher.add_item(self.w0)

    self.Kmain = hasher.out()
    if matter_specific
      self.Ke = self.Kmain[16..31]
      self.Kmain = self.Kmain[0..15]
    end

    # compute KcA and KcB
    var kdf = crypto.HKDF_SHA256()
    var KPV = kdf.derive(self.Kmain, bytes(), bytes().fromstring("ConfirmationKeys"), 64)
    self.KcA = matter_specific ? KPV[0..15] : KPV[0..31]
    self.KcB = matter_specific ? KPV[16..31] : KPV[32..63]
    self.K_shared = kdf.derive(self.Kmain, bytes(), bytes().fromstring("SharedKey"), 32)

    self.cA = crypto.HMAC_SHA256(self.KcA).update(self.pB).out()
    self.cB = crypto.HMAC_SHA256(self.KcB).update(self.pA).out()
  end
end
