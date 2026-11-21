# Optional device vectors; also executed unchanged by the host Berry VM.
# Published SPAKE2+ P-256 draft transcript retained from pinned Tasmota.
# https://chris-wood.github.io/draft-bar-cfrg-spake2plus/draft-bar-cfrg-spake2plus.html
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
if introspect.contains(crypto,'SPAKE2P_Matter')
  # Deterministic two-party composition exercises the ported closure and its dependencies.
  var prover=crypto.SPAKE2P_Matter(bytes('01'),bytes('02'),nil)
  var verifier=crypto.SPAKE2P_Matter(bytes('01'),nil,crypto.EC_P256().public_key(bytes('02')))
  var pA=prover.compute_pA(bytes('03'))
  var pB=verifier.compute_pB(bytes('04'))
  prover.compute_ZV_prover(pB)
  verifier.compute_ZV_verifier(pA)
  assert(prover.Z==verifier.Z)
  assert(prover.V==verifier.V)
  prover.set_context(bytes(),nil,nil)
  verifier.set_context(bytes(),nil,nil)
  prover.compute_TT_hash(false)
  verifier.compute_TT_hash(false)
  assert(prover.Kmain==verifier.Kmain)
  var w0 = bytes("bb8e1bbcf3c48f62c08db243652ae55d3e5586053fca77102994f23ad95491b3")
  var w1 = bytes("7e945f34d78785b8a3ef44d0df5a1a97d6b3b460409a345ca7830387a74b1dba")
  var L  = bytes("04eb7c9db3d9a9eb1f8adab81b5794c1f13ae3e225efbe91ea487425854c7fc00f00bfedcbd09b2400142d40a14f2064ef31dfaa903b91d1faea7093d835966efd")
  var spake_matter = crypto.SPAKE2P_Matter(w0, w1, L)

  # context values
  var Context = bytes().fromstring('SPAKE2+-P256-SHA256-HKDF-SHA256-HMAC-SHA256 Test Vectors')
  var Prover = bytes().fromstring('client')
  var Verifier = bytes().fromstring('server')
  spake_matter.set_context(Context, Prover, Verifier)

  # x is supposed to be random 32 bytes, test vector sets it
  var x = bytes("d1232c8e8693d02368976c174e2088851b8365d0d79a9eee709c6a05a2fad539")
  spake_matter.compute_pA(x)
  assert(spake_matter.pA == bytes('04EF3BD051BF78A2234EC0DF197F7828060FE9856503579BB1733009042C15C0C1DE127727F418B5966AFADFDD95A6E4591D171056B333DAB97A79C7193E341727'))

  # y is supposed to be random 32 bytes, test vector sets it
  var y = bytes("717a72348a182085109c8d3917d6c43d59b224dc6a7fc4f0483232fa6516d8b3")
  spake_matter.compute_pB(y)
  assert(spake_matter.pB == bytes('04C0F65DA0D11927BDF5D560C69E1D7D939A05B0E88291887D679FCADEA75810FB5CC1CA7494DB39E82FF2F50665255D76173E09986AB46742C798A9A68437B048'))

  spake_matter.compute_ZV_prover(spake_matter.pB)
  assert(spake_matter.Z == bytes('04BBFCE7DD7F277819C8DA21544AFB7964705569BDF12FB92AA388059408D50091A0C5F1D3127F56813B5337F9E4E67E2CA633117A4FBD559946AB474356C41839'))
  assert(spake_matter.V == bytes('0458BF27C6BCA011C9CE1930E8984A797A3419797B936629A5A937CF2F11C8B9514B82B993DA8A46E664F23DB7C01EDC87FAA530DB01C2EE405230B18997F16B68'))

  spake_matter.compute_ZV_verifier(spake_matter.pA)
  assert(spake_matter.Z == bytes('04BBFCE7DD7F277819C8DA21544AFB7964705569BDF12FB92AA388059408D50091A0C5F1D3127F56813B5337F9E4E67E2CA633117A4FBD559946AB474356C41839'))
  assert(spake_matter.V == bytes('0458BF27C6BCA011C9CE1930E8984A797A3419797B936629A5A937CF2F11C8B9514B82B993DA8A46E664F23DB7C01EDC87FAA530DB01C2EE405230B18997F16B68'))

  spake_matter.compute_TT_hash(false)

  assert(spake_matter.Kmain == bytes('4c59e1ccf2cfb961aa31bd9434478a1089b56cd11542f53d3576fb6c2a438a29'))

  assert(spake_matter.KcA == bytes('871ae3f7b78445e34438fb284504240239031c39d80ac23eb5ab9be5ad6db58a'))
  assert(spake_matter.KcB == bytes('ccd53c7c1fa37b64a462b40db8be101cedcf838950162902054e644b400f1680'))

  assert(spake_matter.cA == bytes('926cc713504b9b4d76c9162ded04b5493e89109f6d89462cd33adc46fda27527'))
  assert(spake_matter.cB == bytes('9747bcc4f8fe9f63defee53ac9b07876d907d55047e6ff2def2e7529089d3e68'))
  assert(spake_matter.K_shared   == bytes('0c5f8ccd1413423a54f6c1fb26ff01534a87f893779c6e68666d772bfd91f3e7'))




  # The same parties also agree with Matter's truncated confirmation-key mode.
  prover.compute_TT_hash(true)
  verifier.compute_TT_hash(true)
  assert(prover.Kmain==verifier.Kmain && prover.Kmain.size()==16)
  assert(prover.KcA==verifier.KcA && prover.KcA.size()==16)
  assert(prover.KcB==verifier.KcB && prover.KcB.size()==16)
  assert(prover.Ke==verifier.Ke && prover.Ke.size()==16)
  # Reject invalid length, wrong encoding, off-curve point, and infinity after unmasking.
  var bad=bytes().resize(65)
  bad[0]=4
  rejects(/-> prover.compute_ZV_prover(bytes('00')))
  rejects(/-> verifier.compute_ZV_verifier(bytes('00')))
  rejects(/-> prover.compute_ZV_prover(bytes().resize(65)))
  rejects(/-> verifier.compute_ZV_verifier(bytes().resize(65)))
  rejects(/-> prover.compute_ZV_prover(bad))
  rejects(/-> verifier.compute_ZV_verifier(bad))
  rejects(/-> prover.compute_ZV_prover(crypto.EC_P256().mul(prover.w0,prover.N)))
  rejects(/-> verifier.compute_ZV_verifier(crypto.EC_P256().mul(verifier.w0,verifier.M)))
  print('PASS SPAKE2P_Matter')
else
  print('SKIP SPAKE2P_Matter: API not selected')
end
