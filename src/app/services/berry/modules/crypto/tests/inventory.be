# Optional device inventory; also executed unchanged by the host Berry VM.
# Lists every Berry crypto API with IN or OUT and checks that a selected class is complete.
import crypto
import introspect
var crypto_apis=[
  ['MD5',['init','update','finish']],
  ['SHA256',['init','update','out']],
  ['HMAC_SHA256',['init','update','out']],
  ['HKDF_SHA256',['derive']],
  ['PBKDF2_HMAC_SHA256',['derive']],
  ['ED25519',['secret_key','sign','verify']],
  ['EC_C25519',['public_key','shared_key']],
  ['EC_P256',['public_key','shared_key','ecdsa_sign_sha256','ecdsa_sign_sha256_asn1',
    'ecdsa_verify_sha256','ecdsa_verify_sha256_asn1','mod','neg','mul','muladd']],
  ['CHACHA20_POLY1305',['chacha_run','poly_run']],
  ['AES_CBC',['encrypt1','decrypt1']],
  ['AES_CTR',['init','encrypt','decrypt']],
  ['AES_GCM',['init','encrypt','decrypt','tag']],
  ['AES_CCM',['init','encrypt','decrypt','tag','encrypt1','decrypt1']],
  ['RSA',['rs256']],
  ['BLAKE2S',['init','update','out','hmac']],
  ['XCHACHA20_POLY1305',['run']],
  ['SPAKE2P_Matter',['compute_pA','compute_pB','compute_ZV_prover','compute_ZV_verifier',
    'set_context','compute_TT_hash']],
]
var crypto_selected=0
for api: crypto_apis
  if introspect.contains(crypto,api[0])
    var cls=crypto.(api[0])
    for method: api[1]
      assert(introspect.contains(cls,method),api[0]+'.'+method+' missing')
    end
    crypto_selected+=1
    print('IN  '+api[0])
  else
    print('OUT '+api[0])
  end
end
if introspect.contains(crypto,'random')
  print('IN  random')
else
  print('OUT random')
end
print('PASS inventory: '+str(crypto_selected)+' of '+str(size(crypto_apis))+' classes')
