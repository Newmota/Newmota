# Optional device vectors; also executed unchanged by the host Berry VM.
# RFC 8032 section 7.1 Ed25519; RFC 7748 section 6.1 X25519.
# P-256 generator known answer and deterministic ECDSA composition.
import crypto
import introspect
def rejects(operation)
  var failed=false
  try
    operation()
  except 'value_error', 'type_error'
    failed=true
  end
  assert(failed)
end
if introspect.contains(crypto,'ED25519')
  var ed=crypto.ED25519()
  var sk=ed.secret_key(bytes('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60'))
  var sig=ed.sign(bytes(),sk)
  assert(sig==bytes('e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b'))
  assert(ed.verify(bytes(),sig,sk[32..63]))
  sig[0]=sig[0]^1
  assert(!ed.verify(bytes(),sig,sk[32..63]))
  rejects(/-> ed.secret_key(bytes()))
  print('PASS ED25519')
else
  print('SKIP ED25519: API not selected')
end
if introspect.contains(crypto,'EC_C25519')
  var ec=crypto.EC_C25519()
  var alice=bytes('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a')
  assert(ec.public_key(alice)==bytes('8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a'))
  assert(ec.shared_key(alice,bytes('de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f'))==bytes('4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742'))
  rejects(/-> ec.shared_key(alice,bytes().resize(32)))
  print('PASS EC_C25519')
else
  print('SKIP EC_C25519: API not selected')
end
if introspect.contains(crypto,'EC_P256')
  var p256=crypto.EC_P256()
  var scalar=bytes('01')
  var pub=p256.public_key(scalar)
  assert(pub==bytes('046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5'))
  assert(p256.public_key(bytes('02'))==bytes('047cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc4766997807775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1'))
  var message=bytes().fromstring('sample')
  var raw=p256.ecdsa_sign_sha256(scalar,message)
  assert(p256.ecdsa_verify_sha256(pub,message,raw))
  assert(raw==p256.ecdsa_sign_sha256(scalar,message))
  var der=p256.ecdsa_sign_sha256_asn1(scalar,message)
  assert(p256.ecdsa_verify_sha256_asn1(pub,message,der))
  rejects(/-> p256.public_key(bytes('00')))
  print('PASS EC_P256')
else
  print('SKIP EC_P256: API not selected')
end
