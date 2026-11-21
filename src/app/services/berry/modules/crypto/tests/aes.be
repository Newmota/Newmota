# Optional device vectors; also executed unchanged by the host Berry VM.
# NIST SP 800-38A F.2.1 CBC; SP 800-38D GCM examples; SP 800-38C C.1 CCM.
# Additional roundtrip, streaming-state and authentication-failure checks.
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
if introspect.contains(crypto,'AES_CTR')
  var aes=crypto.AES_CTR(bytes().resize(32))
  var data=bytes().fromstring('hello')
  var ciphertext=aes.encrypt(data,bytes().resize(12),0)
  assert(data==bytes().fromstring('hello'))
  assert(aes.decrypt(ciphertext,bytes().resize(12),0)==data)
  assert(aes.decrypt(aes.encrypt(data,bytes().resize(12),-1),bytes().resize(12),-1)==data)
  rejects(/-> aes.encrypt(bytes().resize(17),bytes().resize(12),-1))
  print('PASS AES_CTR')
else
  print('SKIP AES_CTR: API not selected')
end
if introspect.contains(crypto,'AES_GCM')
  var gcm=crypto.AES_GCM(bytes().resize(32),bytes().resize(12))
  assert(gcm.encrypt(bytes().resize(16))==bytes('cea7403d4d606b6e074ec5d3baf39d18'))
  assert(gcm.tag()==bytes('d0d1c8a799996bf0265b98b5d48ab919'))
  assert(gcm.tag()==gcm.tag())
  rejects(/-> gcm.encrypt(bytes('00')))
  var gcm_chunked=crypto.AES_GCM(bytes().resize(32),bytes().resize(12))
  var gcm_combined=gcm_chunked.encrypt(bytes().resize(1))+gcm_chunked.encrypt(bytes().resize(15))
  assert(gcm_combined==bytes('cea7403d4d606b6e074ec5d3baf39d18'))
  assert(gcm_chunked.tag()==gcm.tag())
  print('PASS AES_GCM')
else
  print('SKIP AES_GCM: API not selected')
end
if introspect.contains(crypto,'AES_CCM')
  var ccm=crypto.AES_CCM(bytes('404142434445464748494a4b4c4d4e4f'),bytes('10111213141516'),bytes('0001020304050607'),4,4)
  assert(ccm.encrypt(bytes('20212223'))==bytes('7162015b'))
  assert(ccm.tag()[0..3]==bytes('4dac255d'))
  rejects(/-> ccm.encrypt(bytes('00')))
  var ccm_key=bytes('404142434445464748494a4b4c4d4e4f')
  var ccm_nonce=bytes('10111213141516')
  var ccm_aad=bytes('0001020304050607')
  var ccm_data=bytes('20212223')
  var ccm_tag=bytes().resize(4)
  assert(crypto.AES_CCM.encrypt1(ccm_key,ccm_nonce,0,7,ccm_aad,0,8,ccm_data,0,4,ccm_tag,0,4))
  assert(ccm_data==bytes('7162015b') && ccm_tag==bytes('4dac255d'))
  ccm_tag[0]=ccm_tag[0]^1
  assert(!crypto.AES_CCM.decrypt1(ccm_key,ccm_nonce,0,7,ccm_aad,0,8,ccm_data,0,4,ccm_tag,0,4))
  assert(ccm_data==bytes('7162015b'))
  ccm_tag[0]=ccm_tag[0]^1
  assert(crypto.AES_CCM.decrypt1(ccm_key,ccm_nonce,0,7,ccm_aad,0,8,ccm_data,0,4,ccm_tag,0,4))
  assert(ccm_data==bytes('20212223'))
  var ccm_chunked=crypto.AES_CCM(ccm_key,ccm_nonce,ccm_aad,4,4)
  assert(ccm_chunked.encrypt(bytes('20'))+ccm_chunked.encrypt(bytes('212223'))==bytes('7162015b'))
  assert(ccm_chunked.tag()[0..3]==ccm_tag)
  var ccm_incomplete=crypto.AES_CCM(ccm_key,ccm_nonce,ccm_aad,4,4)
  rejects(/-> ccm_incomplete.tag())
  print('PASS AES_CCM')
else
  print('SKIP AES_CCM: API not selected')
end
if introspect.contains(crypto,'AES_CBC')
  var block=bytes('6bc1bee22e409f96e93d7e117393172a')
  var iv=bytes('000102030405060708090a0b0c0d0e0f')
  assert(crypto.AES_CBC.encrypt1(bytes('2b7e151628aed2a6abf7158809cf4f3c'),iv,block))
  assert(block==bytes('7649abac8119b246cee98e9b12e9197d'))
  assert(iv==block)
  iv=bytes('000102030405060708090a0b0c0d0e0f')
  assert(crypto.AES_CBC.decrypt1(bytes('2b7e151628aed2a6abf7158809cf4f3c'),iv,block))
  assert(block==bytes('6bc1bee22e409f96e93d7e117393172a'))
  rejects(/-> crypto.AES_CBC.encrypt1(bytes().resize(16),block,block))
  rejects(/-> crypto.AES_CBC.encrypt1(bytes().resize(16),iv,bytes('00')))
  print('PASS AES_CBC')
else
  print('SKIP AES_CBC: API not selected')
end
