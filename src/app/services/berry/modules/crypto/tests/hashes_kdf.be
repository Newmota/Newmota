# Optional device vectors; also executed unchanged by the host Berry VM.
# RFC 1321 MD5, SHA-256 abc/empty, RFC 4231 HMAC, RFC 5869 A.1 HKDF.
# PBKDF2 SHA-256: pinned Tasmota vectors (iterations 1, 2, 3 and 4096).
import crypto
import gc
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
if introspect.contains(crypto,'SHA256')
  var sha=crypto.SHA256()
  assert(sha.out()==bytes('e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'))
  sha.update(bytes().fromstring('a'))
  gc.collect()
  sha.update(bytes().fromstring('bc'))
  assert(sha.out()==bytes('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'))
  assert(sha.out()==sha.out())
  rejects(/-> crypto.SHA256().update('bad'))
  var sha_other=crypto.SHA256()
  sha_other.update(bytes().fromstring('different'))
  assert(sha_other.out()!=sha.out())
  # Cross a compression boundary, interleave contexts, and continue after out().
  var sha_block=crypto.SHA256()
  sha_block.update(bytes().fromstring('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'))
  assert(sha_block.out()==bytes('ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb'))
  var sha_interleaved=crypto.SHA256()
  sha_interleaved.update(bytes().fromstring('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'))
  sha_block.update(bytes().fromstring('bc'))
  assert(sha_interleaved.out()==bytes('74b128f30cf83de43ddf4aafc40c7b50a7443d3c73a89a7cfca17e15e43d51ab'))
  assert(sha_block.out()==bytes('2eac2ab13a13bd7eb0771ce0647c9cdddb62d4a1bebdd1829780810962280a84'))
  print('PASS SHA256')
else
  print('SKIP SHA256: API not selected')
end
if introspect.contains(crypto,'MD5')
  var md=crypto.MD5()
  md.update(bytes().fromstring('abc'))
  assert(md.finish()==bytes('900150983cd24fb0d6963f7d28e17f72'))
  print('PASS MD5')
else
  print('SKIP MD5: API not selected')
end
if introspect.contains(crypto,'HMAC_SHA256')
  var mac=crypto.HMAC_SHA256(bytes('0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b'))
  mac.update(bytes().fromstring('Hi There'))
  assert(mac.out()==bytes('b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7'))
  gc.collect()
  assert(mac.out()==mac.out())
  var long_mac=crypto.HMAC_SHA256(bytes().fromstring('kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk'))
  long_mac.update(bytes().fromstring('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'))
  assert(long_mac.out()==bytes('b5775ef6c760eb00ee6c3619820333ee1ca17bfaa2e85c26019a68ea38166430'))
  print('PASS HMAC_SHA256')
else
  print('SKIP HMAC_SHA256: API not selected')
end
if introspect.contains(crypto,'HKDF_SHA256')
  var hk=crypto.HKDF_SHA256.derive(bytes('0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b'),bytes('000102030405060708090a0b0c'),bytes('f0f1f2f3f4f5f6f7f8f9'),42)
  assert(hk==bytes('3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865'))
  print('PASS HKDF_SHA256')
else
  print('SKIP HKDF_SHA256: API not selected')
end
if introspect.contains(crypto,'PBKDF2_HMAC_SHA256')
  # PBKDF2 SHA-256 vectors retained from the pinned Tasmota source.
  assert(crypto.PBKDF2_HMAC_SHA256().derive('password','salt',1,20)==bytes('120fb6cffcf8b32c43e7225256c4f837a86548c9'))
  assert(crypto.PBKDF2_HMAC_SHA256().derive('password','salt',2,20)==bytes('ae4d0c95af6b46d32d0adff928f06dd02a303f8e'))
  assert(crypto.PBKDF2_HMAC_SHA256().derive('password','salt',3,20)==bytes('ad35240ac683febfaf3cd49d845473fbbbaa2437'))
  assert(crypto.PBKDF2_HMAC_SHA256().derive('password','salt',4096,20)==bytes('c5e478d59288c841aa530db6845c4c8d962893a0'))
  print('PASS PBKDF2_HMAC_SHA256')
else
  print('SKIP PBKDF2_HMAC_SHA256: API not selected')
end
