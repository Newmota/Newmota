# Crypto benchmark: times every Berry crypto API in this build and logs the results.
# Run with: Br compile('/crypto_bench.be','file')()
# It returns at once; each primitive runs in its own timer step, so each gets its own
# four-second execution budget. Bulk cases process 1 KiB per call, and Berry call
# overhead is part of every result. APIs left out of the build print SKIP.
#
# Building: config/README.md#crypto lists the crypto selection, which hardware options
# each chip supports and which Berry classes each option speeds up. Berry cannot see
# whether hardware or software is linked, so build the same selection twice, with and
# without the hardware options, for example for an ESP32-C5:
#
#   # user_config/bench-c5.yaml       # user_config/bench-c5-sw.yaml
#   name: bench-c5                    name: bench-c5-sw
#   inherit: esp32c5                  inherit: esp32c5
#   crypto: all                       crypto: all
#   features:
#     USE_CRYPTO_HW_SHA256: true
#     USE_CRYPTO_HW_SHA512: true
#     USE_CRYPTO_HW_P256: true
#
# Build each with make bench-c5 and make bench-c5-sw, run this script on both and
# compare the BENCH lines. A hardware option the chip lacks fails the build; the C5
# has no AES-192, so USE_CRYPTO_HW_AES is left out here.
import crypto
import introspect
import newmota
var bench_budget=250
var bench_kib=bytes().resize(1024)
var bench_key16=bytes('000102030405060708090a0b0c0d0e0f')
var bench_key32=bytes('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f')
var bench_scalar=bytes('c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721')
var bench_scalar2=bytes('7e945f34d78785b8a3ef44d0df5a1a97d6b3b460409a345ca7830387a74b1dba')
var bench_rsa=bytes('3082025d02010002818100e91dc74a0723556f8034c6883f6504a49eb18146dc71f14231f843bab6c92210221cf64125ad453381dd533a61461c6de593ed0dad1e62afd264b9ea8a3b973e9fb99beb243f913e6bec669989a1413d6f2ec0defbe6dbb7ceb59df2525fae463fc7a09075baf4b0a870edf19b4f2d7c0079985b39eae61eb41c7c2552b1b16f02030100010281810094bac80c8613580e049927558444da3a817934c6d61ce855587652180d48b42fe0e3e3855b197a7e4b4bbf2f0f22e13e886be1a189234ac96c8d91447f9f5775882d301bbc79676990535578377ccaaaad58d5a3f77f5698fe17969999185c2e1ef0d1e950be6e7b4d2357ef502b95956ba3d7c49fffee5eb4e0ac35f19a7f11024100fb97a2d22b21bb50597a9a26694ca6cf4ec8fab7f9ca1bd8c64c6127b40198c82d785fa2a82554a751a94e42710ffe78f4364fa8bd0dff1430b05186e3fd3aa7024100ed3347938155cbc5df92c5005aad868456c18e750d06df8c08349aaf59d38f8dc3432b3177038f30626d4c50f841dac42cb2bdd60ae21150217a85f03314d3f9024100b57c55d47b8e929b4c60974ffc077210fb09196a9b028301e5517587f95e698284e25f426b3eba8913ec286adf3a255bc4f6649219063ea9a6d1998dcf6a7ce302406c3e517483b9f652a71320296837f5c1e0a155741eecbb08f919a05952e19389fac865269e8c1644db35cb74cbc5a9a28a4f4b0829513edb555bc75174ce156902402e9715dbf565b4e709798fd4f43f3905f4fdf502adb03ce5bbb25f5b892f59207fac912bc30459065a8efe94141b29b0a43016c1d2a175a0e00d561f694fa1cc')
var bench_cases=[]
def bench_add(api,name,bulk,setup)
  if introspect.contains(crypto,api)
    bench_cases.push([name,bulk,setup])
  else
    print('SKIP '+name+': not in this build')
  end
end
# Device integers are 32 bits, so the difference of two signed millis() values wraps correctly.
def bench_elapsed(start)
  return newmota.millis()-start
end
# A case is a setup closure returning the operation, so contexts and inputs are built untimed.
bench_add('MD5','MD5',true,def ()
  var md=crypto.MD5()
  return /-> md.update(bench_kib)
end)
bench_add('SHA256','SHA256',true,def ()
  var sha=crypto.SHA256()
  return /-> sha.update(bench_kib)
end)
bench_add('HMAC_SHA256','HMAC_SHA256',true,def ()
  var mac=crypto.HMAC_SHA256(bench_key32)
  return /-> mac.update(bench_kib)
end)
bench_add('BLAKE2S','BLAKE2S',true,def ()
  var bl=crypto.BLAKE2S(32,bytes())
  return /-> bl.update(bench_kib)
end)
bench_add('HKDF_SHA256','HKDF_SHA256 32 bytes',false,def ()
  return /-> crypto.HKDF_SHA256.derive(bench_key32,bench_key16,bytes(),32)
end)
bench_add('PBKDF2_HMAC_SHA256','PBKDF2_HMAC_SHA256 1000 iterations',false,def ()
  var kdf=crypto.PBKDF2_HMAC_SHA256()
  return /-> kdf.derive('password','salt',1000,32)
end)
bench_add('AES_CBC','AES_CBC-128 encrypt',true,def ()
  var iv=bytes().resize(16)
  var data=bytes().resize(1024)
  return /-> crypto.AES_CBC.encrypt1(bench_key16,iv,data)
end)
bench_add('AES_CBC','AES_CBC-128 decrypt',true,def ()
  var iv=bytes().resize(16)
  var data=bytes().resize(1024)
  return /-> crypto.AES_CBC.decrypt1(bench_key16,iv,data)
end)
bench_add('AES_CTR','AES_CTR-256',true,def ()
  var aes=crypto.AES_CTR(bench_key32)
  var nonce=bytes().resize(12)
  return /-> aes.encrypt(bench_kib,nonce,0)
end)
bench_add('AES_GCM','AES_GCM-256 encrypt',true,def ()
  var gcm=crypto.AES_GCM(bench_key32,bytes().resize(12))
  return /-> gcm.encrypt(bench_kib)
end)
bench_add('AES_CCM','AES_CCM-128 encrypt1',true,def ()
  var nonce=bytes().resize(12)
  var data=bytes().resize(1024)
  var tag=bytes().resize(16)
  return /-> crypto.AES_CCM.encrypt1(bench_key16,nonce,0,12,bytes(),0,0,data,0,1024,tag,0,16)
end)
bench_add('CHACHA20_POLY1305','CHACHA20',true,def ()
  var nonce=bytes().resize(12)
  var data=bytes().resize(1024)
  return /-> crypto.CHACHA20_POLY1305.chacha_run(bench_key32,nonce,0,data)
end)
bench_add('CHACHA20_POLY1305','POLY1305',true,def ()
  return /-> crypto.CHACHA20_POLY1305.poly_run(bench_kib,bench_key32)
end)
bench_add('XCHACHA20_POLY1305','XCHACHA20_POLY1305 encrypt',true,def ()
  var nonce=bytes().resize(24)
  var data=bytes().resize(1024)
  var tag=bytes().resize(16)
  return /-> crypto.XCHACHA20_POLY1305.run(bench_key32,nonce,data,bytes(),tag,true)
end)
bench_add('ED25519','ED25519 sign',false,def ()
  var ed=crypto.ED25519()
  var sk=ed.secret_key(bench_key32)
  return /-> ed.sign(bench_key16,sk)
end)
bench_add('ED25519','ED25519 verify',false,def ()
  var ed=crypto.ED25519()
  var sk=ed.secret_key(bench_key32)
  var sig=ed.sign(bench_key16,sk)
  var pub=sk[32..63]
  return def ()
    assert(ed.verify(bench_key16,sig,pub))
  end
end)
bench_add('EC_C25519','EC_C25519 shared_key',false,def ()
  var ec=crypto.EC_C25519()
  var peer=ec.public_key(bench_scalar2)
  return /-> ec.shared_key(bench_scalar,peer)
end)
bench_add('EC_P256','EC_P256 public_key',false,def ()
  return /-> crypto.EC_P256.public_key(bench_scalar)
end)
bench_add('EC_P256','EC_P256 shared_key',false,def ()
  var peer=crypto.EC_P256.public_key(bench_scalar2)
  return /-> crypto.EC_P256.shared_key(bench_scalar,peer)
end)
bench_add('EC_P256','EC_P256 mul',false,def ()
  var point=crypto.EC_P256.public_key(bench_scalar2)
  return /-> crypto.EC_P256.mul(bench_scalar,point)
end)
bench_add('EC_P256','EC_P256 muladd',false,def ()
  var a=crypto.EC_P256.public_key(bench_scalar)
  var b=crypto.EC_P256.public_key(bench_scalar2)
  return /-> crypto.EC_P256.muladd(bench_scalar,a,bench_scalar2,b)
end)
bench_add('EC_P256','EC_P256 ecdsa_sign_sha256',false,def ()
  return /-> crypto.EC_P256.ecdsa_sign_sha256(bench_scalar,bench_key16)
end)
bench_add('EC_P256','EC_P256 ecdsa_verify_sha256',false,def ()
  var pub=crypto.EC_P256.public_key(bench_scalar)
  var sig=crypto.EC_P256.ecdsa_sign_sha256(bench_scalar,bench_key16)
  return def ()
    assert(crypto.EC_P256.ecdsa_verify_sha256(pub,bench_key16,sig))
  end
end)
bench_add('SPAKE2P_Matter','SPAKE2P_Matter compute_pA',false,def ()
  var prover=crypto.SPAKE2P_Matter(bench_scalar,bench_scalar2,nil)
  return /-> prover.compute_pA(bench_scalar)
end)
bench_add('RSA','RSA-1024 rs256',false,def ()
  return /-> crypto.RSA.rs256(bench_rsa,bench_key16)
end)
# The first call is untimed warm-up unless it alone already exceeds the budget.
def bench_run(entry)
  var operation=entry[2]()
  var start=newmota.millis()
  operation()
  var elapsed=bench_elapsed(start)
  var count=1
  if elapsed<bench_budget
    count=0
    start=newmota.millis()
    elapsed=0
    while elapsed<bench_budget
      operation()
      count+=1
      elapsed=bench_elapsed(start)
    end
  end
  var line='BENCH '+entry[0]+': '+str(elapsed*1000/count)+' us/op'
  if entry[1]
    line+=', '+str(count*1000/elapsed)+' KiB/s'
  end
  print(line+' ('+str(count)+' in '+str(elapsed)+' ms)')
end
var bench_index=0
def bench_step()
  if bench_index>=size(bench_cases)
    print('BENCH done: '+str(size(bench_cases))+' cases')
    return
  end
  var entry=bench_cases[bench_index]
  bench_index+=1
  # Schedule first: a failing case is logged and the next case still runs.
  newmota.set_timer(20,bench_step,'crypto_bench')
  bench_run(entry)
end
print('BENCH start: '+str(size(bench_cases))+' cases, '+str(bench_budget)+' ms each')
newmota.set_timer(20,bench_step,'crypto_bench')
