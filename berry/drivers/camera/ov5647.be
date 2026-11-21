# OV5647 MIPI-CSI sensor driver for Newmota's `cam` module (ESP32-P4, USE_CSI_WEBCAM).
#
# Upload to the filesystem and start it from autoexec.be:
#   load("ov5647.be")  cam.register(OV5647())
# or build it in with the environment option `solidify` and only register it.
# Upload isp/ov5647.json as /isp.json for its colour tuning. The sensor sits on a
# configured I2C bus at 0x36. WcRes 0..4 selects a preset, WcWindow x,y,w,h,bin,fps,format
# a custom window; x and y shift it from the centre. In the still session (WcSession 4)
# the presets are RAW8 with a brighter exposure target.
#
# Ported from Tasmota's tasmota/berry/camera_sensor/ov5647.be: register tables, window
# and PLL calculation are Tasmota's; the interface follows Newmota's camera contract.

#@ solidify:OV5647
#@ requires:USE_CSI_WEBCAM

class OV5647
  static ADDR = 0x36
  static CHIP_ID = 0x5647
  static REG_END = 0xFFFF
  static REG_DELAY = 0xFFFE
  # Presets: width, height, binning, format (0 RAW8, 1 RAW10), fps
  static PRESETS = [
    [640, 480, 2, 1, 30],     # 0: VGA, bin 2
    [1280, 720, 2, 1, 30],    # 1: 720p, bin 2
    [1296, 960, 2, 1, 30],    # 2: bin-2 frame; the full 972 lines do not work
    [1920, 1080, 1, 0, 20],   # 3: 1080p, bin 1, RAW8
    [2592, 1944, 1, 1, 15],   # 4: full sensor, bin 1
  ]

  var wire, name, width, height, format, lanes, mipi_clock, fps, bin_mode
  var max_width, max_height, is_streaming
  var offset_x, offset_y, bayer, mirror, flip, still

  def init()
    self.name = "OV5647"
    self.width = 640
    self.height = 480
    self.format = 1
    self.lanes = 2
    self.mipi_clock = 291
    self.fps = 30
    self.bin_mode = 2
    self.max_width = 2592
    self.max_height = 1944
    self.is_streaming = false
    self.offset_x = 0
    self.offset_y = 0
    self.bayer = 0
    self.mirror = false
    self.flip = false
    self.still = false
  end

  def write_reg16(reg, val)
    import cam
    return cam.write_reg(self.wire, self.ADDR, reg, val, 2)
  end

  def read_reg16(reg)
    import cam
    return cam.read_reg(self.wire, self.ADDR, reg, 2)
  end

  def write_array(regs)
    for entry : regs
      var reg = entry[0]
      if reg == self.REG_END break end
      if reg == self.REG_DELAY
        newmota.delay(entry[1])
      elif !self.write_reg16(reg, entry[1])
        log(format("CAM: OV5647 write failed at 0x%04X", reg))
        return false
      end
    end
    return true
  end

  def detect()
    self.wire = newmota.wire_scan(self.ADDR)
    if self.wire == nil
      log("CAM: OV5647 not found on I2C 0x36")
      return false
    end
    newmota.delay(10)
    var high = self.read_reg16(0x300A)
    var low = self.read_reg16(0x300B)
    if high == nil || low == nil return false end
    return ((high << 8) | low) == self.CHIP_ID
  end

  # Basic initialization and reset sequence
  def common_regs()
    return [
      [0x0100, 0x00],           # enable sleep
      [0x0103, 0x01],           # soft reset
      [self.REG_DELAY, 10],     # 10ms delay
      [0x4800, 0x01],           # clock lane gate
      [self.REG_END, 0x00]
    ]
  end

  # Dynamic register calculator; x and y shift the window from the centre in output pixels
  def regs_custom(x, y, w, h, bin, fps, fmt)
    # 1. Align geometry (CSI requires alignment)
    w = (w / 8) * 8
    if w < 16 w = 16 end
    if h < 16 h = 16 end
    if fps < 1 fps = 1 end
    self.width = w
    self.height = h
    self.format = fmt
    self.bin_mode = bin

    # 2. PLL: pclk = (EXCLK 25 MHz / prediv 3) * mult / sys_div 2, 833333 Hz per mult unit
    #    at RAW10; RAW8 gives a 10/8 higher pixel rate. Lane rate = 25 MHz * mult / 6.
    var bpp = (fmt == 0) ? 8 : 10
    var vts_min = h + 50
    var pll_mult
    if bin == 2
      # mult = ceil(1896 * vts_min * fps * bpp / 8333330), rescaled against overflow
      pll_mult = (vts_min * fps * 5688 * (bpp / 2) + 12499999) / 12500000
      if pll_mult < 70 pll_mult = 70 end    # floor: minimum MIPI bandwidth
    else
      # mult = ceil(2500 * vts_min * fps * bpp / 8333330) ~ 3/10000
      pll_mult = (vts_min * fps * 3 * bpp + 9999) / 10000
      if pll_mult < 80 pll_mult = 80 end
    end
    if pll_mult > 252 pll_mult = 252 end
    if pll_mult >= 128 pll_mult = (pll_mult + 1) & 0xFE end  # even only above 127
    self.mipi_clock = (25 * pll_mult + 5) / 6
    self.fps = fps
    var ppm = 833333 * 10 / bpp
    var mipi_period = (2400 * (bpp / 2) / 5) / pll_mult
    if mipi_period < 0x08 mipi_period = 0x08 end
    log(format("CAM: OV5647 %dx%d bin %d fmt %d %d fps, PLL %d, MIPI %d Mbit/s",
      w, h, bin, fmt, fps, pll_mult, self.mipi_clock))

    # 3. Registers
    if bin == 2
      # Bin 2, high speed: crop in the 1296x972 binned frame. Native starts are
      # multiples of four, so binned starts are even.
      var center_x = (1296 - w) / 2
      var center_y = (972 - h) / 2
      var crop_x = center_x + x
      var crop_y = center_y + y
      if crop_x < 0 crop_x = 0 elif crop_x > 1296 - w crop_x = 1296 - w end
      if crop_y < 0 crop_y = 0 elif crop_y > 972 - h crop_y = 972 - h end
      crop_x = (crop_x / 2) * 2
      crop_y = (crop_y / 2) * 2
      self.offset_x = crop_x - center_x
      self.offset_y = crop_y - center_y
      var win_x = crop_x * 2
      var win_y = crop_y * 2
      var end_x = win_x + w * 2 + 31
      var end_y = win_y + h * 2 + 15
      if end_x > 2623 end_x = 2623 end
      if end_y > 1953 end_y = 1953 end
      var hts = 1896
      var vts = pll_mult * ppm / (hts * fps)
      if vts < h + 50 vts = h + 50 end
      return [
        [0x3034, fmt == 0 ? 0x18 : 0x1a],
        [0x3035,0x21],
        [0x3036, pll_mult],
        [0x303c,0x11],
        [0x3106,0xf5],
        [0x3814,0x31], [0x3815,0x31], [0x3820,0x41], [0x3821,0x03],
        [0x3827,0xec], [0x370c,0x0f], [0x3612,0x59], [0x3618,0x00],
        [0x5000,0xff], [0x583e,0xf0], [0x583f,0x20], [0x5002,0x41], [0x5003,0x08], [0x5a00,0x08],
        [0x3000,0x00], [0x3001,0x00], [0x3002,0x00], [0x3016,0x08], [0x3017,0xe0],
        [0x3018,0x44], [0x301c,0xf8], [0x301d,0xf0],
        [0x3a18,0x00], [0x3a19,0xf8], [0x3c01,0x80], [0x3c00,0x40], [0x3b07,0x0c],
        [0x380c,0x07], [0x380d,0x68], # HTS = 1896
        [0x380e, (vts >> 8) & 0xFF],  [0x380f, vts & 0xFF],
        [0x3800, (win_x >> 8) & 0xFF], [0x3801, win_x & 0xFF],
        [0x3802, (win_y >> 8) & 0xFF], [0x3803, win_y & 0xFF],
        [0x3804, (end_x >> 8) & 0xFF], [0x3805, end_x & 0xFF],
        [0x3806, (end_y >> 8) & 0xFF], [0x3807, end_y & 0xFF],
        [0x3808, (w >> 8) & 0xFF], [0x3809, w & 0xFF],
        [0x380a, (h >> 8) & 0xFF], [0x380b, h & 0xFF],
        [0x3810,0x00], [0x3811,0x08],
        [0x3812,0x00], [0x3813,0x00],
        [0x3630,0x2e], [0x3632,0xe2], [0x3633,0x23], [0x3634,0x44], [0x3636,0x06],
        [0x3620,0x64], [0x3621,0xe0], [0x3600,0x37],
        [0x3704,0xa0], [0x3703,0x5a], [0x3715,0x78], [0x3717,0x01], [0x3731,0x02],
        [0x370b,0x60], [0x3705,0x1a],
        [0x3f05,0x02], [0x3f06,0x10], [0x3f01,0x0a],
        [0x3a08,0x01], [0x3a09,0x27], [0x3a0a,0x00], [0x3a0b,0xf6], [0x3a0d,0x04],
        [0x3a0e,0x03], [0x3a0f,0x58], [0x3a10,0x50], [0x3a1b,0x58], [0x3a1e,0x50],
        [0x3a11,0x60], [0x3a1f,0x28],
        [0x4001,0x02], [0x4004,0x02], [0x4000,0x09],
        [0x4837, mipi_period], [0x4050,0x6e], [0x4051,0x8f],
        [self.REG_END,0x00]
      ]
    end
    # Bin 1, full resolution: crop in 2592x1944
    var center_x = (2592 - w) / 2
    var center_y = (1944 - h) / 2
    var start_x = center_x + x
    var start_y = center_y + y
    if start_x < 0 start_x = 0 elif start_x > 2592 - w start_x = 2592 - w end
    if start_y < 0 start_y = 0 elif start_y > 1944 - h start_y = 1944 - h end
    start_x = (start_x / 2) * 2
    start_y = (start_y / 2) * 2
    self.offset_x = start_x - center_x
    self.offset_y = start_y - center_y
    var win_x = start_x + 12
    var win_y = start_y + 2
    var end_x = win_x + w + 8 - 1
    var end_y = win_y + h + 8 - 1
    if end_x > 2611 end_x = 2611 end
    if end_y > 1953 end_y = 1953 end
    # The full 2592-pixel line needs Linux's full-array HTS 2844; narrower windows such as
    # 1080p keep the tested 2500.
    var hts = (w > 2500) ? 2844 : 2500
    var vts = pll_mult * ppm / (hts * fps)
    if vts < h + 50 vts = h + 50 end
    return [
      # System and PLL
      [0x3034, fmt == 0 ? 0x18 : 0x1a], # MIPI 8-bit / 10-bit
      [0x3035, 0x21],
      [0x3036, pll_mult],
      [0x303c, 0x11],
      [0x3106, 0xf5],
      # Timing and ISP offset
      [0x3814, 0x11], [0x3815, 0x11],   # no skipping
      [0x3820, 0x00], [0x3821, 0x02],
      # Geometry
      [0x3800, (win_x >> 8) & 0xFF], [0x3801, win_x & 0xFF],
      [0x3802, (win_y >> 8) & 0xFF], [0x3803, win_y & 0xFF],
      [0x3804, (end_x >> 8) & 0xFF], [0x3805, end_x & 0xFF],
      [0x3806, (end_y >> 8) & 0xFF], [0x3807, end_y & 0xFF],
      [0x3808, (w >> 8) & 0xFF],     [0x3809, w & 0xFF],
      [0x380a, (h >> 8) & 0xFF],     [0x380b, h & 0xFF],
      # Timing generator
      [0x3810, 0x00], [0x3811, 0x04],
      [0x3812, 0x00], [0x3813, 0x02],
      [0x380c, (hts >> 8) & 0xFF], [0x380d, hts & 0xFF],
      [0x380e, (vts >> 8) & 0xFF], [0x380f, vts & 0xFF],
      # Analog core
      [0x3708, 0x64], [0x3709, 0x12],
      [0x3827, 0xec], [0x370c, 0x03],
      [0x3612, 0x5b], [0x3618, 0x04],
      [0x3630, 0x2e], [0x3632, 0xe2], [0x3633, 0x23], [0x3634, 0x44], [0x3636, 0x06],
      [0x3620, 0x64], [0x3621, 0xe0], [0x3600, 0x37],
      [0x3704, 0xa0], [0x3703, 0x5a], [0x3715, 0x78], [0x3717, 0x01], [0x3731, 0x02],
      [0x370b, 0x60], [0x3705, 0x1a],
      # Lens shading and gamma
      [0x5000, 0xff], [0x583e, 0xf0], [0x583f, 0x4f],
      [0x5003, 0x08], [0x5a00, 0x08],
      # Automatic exposure and gain at most 8x. Streams keep low targets against noise;
      # stills take Tasmota's earlier brighter ones.
      [0x3503, 0x00],
      [0x3a0f, self.still ? 0x58 : 0x30], [0x3a10, self.still ? 0x50 : 0x28],
      [0x3a1b, self.still ? 0x58 : 0x30], [0x3a1e, self.still ? 0x50 : 0x26],
      [0x3a18, 0x00], [0x3a19, 0x80],
      [0x3a08, 0x01], [0x3a09, 0x4b],
      [0x3a0a, 0x01], [0x3a0b, 0x13],
      # AWB and drive
      [0x3000, 0x00], [0x3001, 0x00], [0x3002, 0x00],
      [0x3016, 0x08], [0x3017, 0xe0],
      [0x3018, 0x44], [0x301c, 0xf8], [0x301d, 0xf0],
      [0x3c00, 0x40], [0x3b07, 0x0c],
      # Statistics, black level and MIPI
      [0x3a11, 0x60], [0x3a1f, 0x28],
      [0x4001, 0x02], [0x4004, 0x04], [0x4000, 0x09],
      [0x4837, mipi_period], [0x4800, 0x34],
      [self.REG_END, 0x00]
    ]
  end

  # Sets flip (0x3820 bit 1) and mirror (0x3821 bit 1) over the tables' other bits and
  # records the Bayer order they give.
  def orient()
    var r3820 = self.read_reg16(0x3820)
    var r3821 = self.read_reg16(0x3821)
    if r3820 == nil || r3821 == nil return false end
    r3820 = self.flip ? (r3820 | 0x02) : (r3820 & 0xFD)
    r3821 = self.mirror ? (r3821 | 0x02) : (r3821 & 0xFD)
    if !self.write_reg16(0x3820, r3820) || !self.write_reg16(0x3821, r3821) return false end
    self.bayer = (self.flip ? 2 : 0) | (self.mirror ? 1 : 0)
    return true
  end

  def configure(x, y, w, h, bin, fps, fmt)
    if !self.detect() return false end
    if !self.write_array(self.common_regs()) return false end
    if !self.write_array(self.regs_custom(x, y, w, h, bin, fps, fmt)) return false end
    return self.orient()
  end

  # --- cam module interface ----------------------------------------------

  def start(resolution)
    if resolution < 0 || resolution >= size(self.PRESETS)
      log(format("CAM: OV5647 has no preset %i", resolution))
      return false
    end
    var p = self.PRESETS[resolution]
    return self.configure(0, 0, p[0], p[1], p[2], p[4], self.still ? 0 : p[3])
  end

  def window(x, y, w, h, bin, fps, fmt)
    if w > self.max_width || h > self.max_height return false end
    return self.configure(x, y, w, h, bin, fps, fmt)
  end

  def stream(on)
    if on
      if !self.write_reg16(0x4800, 0x14) return false end   # MIPI enable
      if !self.write_reg16(0x0100, 0x01) return false end   # stream
    elif !self.write_reg16(0x0100, 0x00)
      return false
    end
    self.is_streaming = on
    return true
  end

  # Flip and mirror take effect at the next start, which the cam module runs for them.
  # Other image tuning is the ISP's: /isp.json
  def set(key, value)
    if key == "mirror" self.mirror = value != 0 return true end
    if key == "flip" self.flip = value != 0 return true end
    if key == "session" self.still = value == 4 return true end
    return false
  end
end
