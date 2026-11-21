# Optional device entropy smoke; executed unchanged by the host Berry VM.
# Random API smoke checks; these do not qualify entropy quality.
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
if introspect.contains(crypto,'random')
  assert(crypto.random(0)==bytes())
  rejects(/-> crypto.random(-1))
  var available=true
  try
    var sample=crypto.random(32)
    assert(sample.size()==32)
  except 'runtime_error' as error, message
    if message!='cryptographic entropy unavailable'
      raise 'runtime_error', message
    end
    available=false
    print('UNAVAILABLE random: cryptographic entropy unavailable from platform adapter')
  end
  if available
    print('PASS random: nonempty platform adapter available; entropy quality unqualified')
  end
else
  print('SKIP random: API not selected')
end
