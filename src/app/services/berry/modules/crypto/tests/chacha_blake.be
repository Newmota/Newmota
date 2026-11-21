# Optional device vectors; also executed unchanged by the host Berry VM.
# RFC 8439 section 2.5.2 Poly1305; RFC 7693 BLAKE2s abc.
# ChaCha/XChaCha roundtrip and authentication-failure checks.
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
if introspect.contains(crypto,'CHACHA20_POLY1305')
  var poly=crypto.CHACHA20_POLY1305.poly_run(bytes().fromstring('Cryptographic Forum Research Group'),bytes('85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b'))
  assert(poly==bytes('a8061dc1305136c6c22b8baf0c0127a9'))
  var plain=bytes().fromstring('hello')
  var counter=crypto.CHACHA20_POLY1305.chacha_run(bytes().resize(32),bytes().resize(12),0,plain)
  assert(counter==1)
  assert(plain!=bytes().fromstring('hello'))
  crypto.CHACHA20_POLY1305.chacha_run(bytes().resize(32),bytes().resize(12),0,plain)
  assert(plain==bytes().fromstring('hello'))
  rejects(/-> crypto.CHACHA20_POLY1305.chacha_run(bytes().resize(32),bytes().resize(12),-1,bytes().resize(65)))
  var alias_key=bytes().resize(32)
  rejects(/-> crypto.CHACHA20_POLY1305.chacha_run(alias_key,bytes().resize(12),0,alias_key))
  print('PASS CHACHA20_POLY1305')
else
  print('SKIP CHACHA20_POLY1305: API not selected')
end
if introspect.contains(crypto,'BLAKE2S')
  var bl=crypto.BLAKE2S(32,bytes())
  bl.update(bytes().fromstring('abc'))
  assert(bl.out()==bytes('508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982'))
  print('PASS BLAKE2S')
else
  print('SKIP BLAKE2S: API not selected')
end
if introspect.contains(crypto,'XCHACHA20_POLY1305')
  var xcdata=bytes().fromstring('hello')
  var xctag=bytes().resize(16)
  assert(crypto.XCHACHA20_POLY1305.run(bytes().resize(32),bytes().resize(24),xcdata,bytes(),xctag,true))
  var xccipher=xcdata.copy()
  assert(crypto.XCHACHA20_POLY1305.run(bytes().resize(32),bytes().resize(24),xcdata,bytes(),xctag,false))
  assert(xcdata==bytes().fromstring('hello'))
  xctag[0]=xctag[0]^1
  assert(!crypto.XCHACHA20_POLY1305.run(bytes().resize(32),bytes().resize(24),xccipher,bytes(),xctag,false))
  print('PASS XCHACHA20_POLY1305')
else
  print('SKIP XCHACHA20_POLY1305: API not selected')
end
