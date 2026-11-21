# TLV writer. The reader is native because it faces network input; this writer only
# encodes what this firmware builds, so it stays small Berry.

#@ solidify:Matter_Tlv,weak

class Matter_Tlv
  var b

  def init()
    self.b = bytes()
  end

  # Writes the control octet, with a context tag unless anonymous
  def tag(t, kind)
    if t == nil
      self.b.add(kind, 1)
    else
      self.b.add(0x20 | kind, 1)
      self.b.add(t, 1)
    end
    return self
  end

  # Unsigned integer in its smallest width; negative means a value above 31 bits
  def uint(t, v)
    if v >= 0 && v < 0x100
      self.tag(t, 0x04)
      self.b.add(v, 1)
    elif v >= 0 && v < 0x10000
      self.tag(t, 0x05)
      self.b.add(v, 2)
    else
      self.tag(t, 0x06)
      self.b.add(v, 4)
    end
    return self
  end

  # Signed integer in its smallest width
  def int(t, v)
    if v >= -0x80 && v < 0x80
      self.tag(t, 0x00)
      self.b.add(v, 1)
    elif v >= -0x8000 && v < 0x8000
      self.tag(t, 0x01)
      self.b.add(v, 2)
    else
      self.tag(t, 0x02)
      self.b.add(v, 4)
    end
    return self
  end

  # 64-bit unsigned value held in 8 little-endian bytes
  def u64(t, v)
    self.tag(t, 0x07)
    self.b .. v
    return self
  end

  def bool(t, v)
    return self.tag(t, v ? 0x09 : 0x08)
  end

  def null(t)
    return self.tag(t, 0x14)
  end

  def str(t, v)
    if size(v) < 0x100
      self.tag(t, 0x0C)
      self.b.add(size(v), 1)
    else
      self.tag(t, 0x0D)
      self.b.add(size(v), 2)
    end
    self.b .. bytes().fromstring(v)
    return self
  end

  def bin(t, v)
    if size(v) < 0x100
      self.tag(t, 0x10)
      self.b.add(size(v), 1)
    else
      self.tag(t, 0x11)
      self.b.add(size(v), 2)
    end
    self.b .. v
    return self
  end

  def st(t)
    return self.tag(t, 0x15)
  end

  def arr(t)
    return self.tag(t, 0x16)
  end

  def lst(t)
    return self.tag(t, 0x17)
  end

  def close()
    self.b.add(0x18, 1)
    return self
  end

  # Appends elements encoded elsewhere
  def raw(v)
    self.b .. v
    return self
  end
end
