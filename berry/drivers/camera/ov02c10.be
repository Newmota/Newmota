# OV02C10 MIPI-CSI sensor driver for Newmota's `cam` module (ESP32-P4, USE_CSI_WEBCAM).
#
# Upload to the filesystem and start it from autoexec.be:
#   load("ov02c10.be")  cam.register(OV02C10())
# or build it in with the environment option `solidify` and only register it.
# The sensor sits on a configured I2C bus at 0x36. WcRes 0..2 selects a preset, WcWindow
# x,y,w,h,bin,fps,format a custom window; x and y shift it from the centre. The sensor
# reads RAW10 without binning only, so other formats and binning are ignored. In the
# still session (WcSession 4) the exposure runs up to the frame length.
#
# Ported from the OV02C10 driver of a local Tasmota branch, whose register tables follow
# Espressif's esp-video-components PR #46; the interface follows Newmota's camera contract.
# Neither automatic exposure nor gain is qualified on this sensor: the exposure is fixed
# near the frame length, so the ISP's tuning has to carry the rest.

#@ solidify:OV02C10
#@ requires:USE_CSI_WEBCAM

class OV02C10
  static ADDR = 0x36
  static CHIP_ID = 0x5602
  static REG_END = 0xFFFF
  static REG_DELAY = 0xFFFE
  # The readable array: x 0..0x078F, y 4..0x0443
  static ARRAY_W = 1928
  static ARRAY_H = 1088
  static X_LAST = 0x078F
  static Y_LAST = 0x0443
  static HTS = 2280
  static PCLK = 80000000
  static VTS_MIN = 1164 # about 30 fps
  # Presets: width, height, fps; all RAW10 crops of the unbinned array
  static PRESETS = [
    [640, 480, 30],    # 0: VGA crop
    [1280, 720, 30],   # 1: 720p crop
    [1920, 1080, 30],  # 2: 1080p, the whole array
  ]

  var wire, name, width, height, format, lanes, mipi_clock, fps, bin_mode
  var max_width, max_height, is_streaming
  var offset_x, offset_y, bayer, mirror, flip, still

  def init()
    self.name = "OV02C10"
    self.width = 1920
    self.height = 1080
    self.format = 1
    self.lanes = 2
    self.mipi_clock = 800
    self.fps = 30
    self.bin_mode = 1
    self.max_width = 1920
    self.max_height = 1080
    self.is_streaming = false
    self.offset_x = 0
    self.offset_y = 0
    self.bayer = 2
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
        log(format("CAM: OV02C10 write failed at 0x%04X", reg))
        return false
      end
    end
    return true
  end

  def detect()
    self.wire = newmota.wire_scan(self.ADDR)
    if self.wire == nil
      log("CAM: OV02C10 not found on I2C 0x36")
      return false
    end
    newmota.delay(10)
    var high = self.read_reg16(0x300A)
    var low = self.read_reg16(0x300B)
    if high == nil || low == nil return false end
    return ((high << 8) | low) == self.CHIP_ID
  end

  def common_regs()
    return [
      [0x0103, 0x01], [self.REG_DELAY, 10], [0x0100, 0x00],
      [self.REG_END, 0x00]
    ]
  end

  # x and y shift the window from the centre in output pixels
  def regs_custom(x, y, w, h, fps)
    w = (w / 8) * 8
    h = (h / 2) * 2
    if w < 64 w = 64 elif w > self.max_width w = self.max_width end
    if h < 64 h = 64 elif h > self.max_height h = self.max_height end
    self.width = w
    self.height = h
    self.format = 1
    self.bin_mode = 1
    self.mipi_clock = 800

    # FPS = PCLK / (HTS * VTS); the shortest frame limits the rate to about 30 fps
    var act_fps = (fps <= 0) ? 30 : fps
    var vts = self.PCLK / (self.HTS * act_fps)
    if vts < self.VTS_MIN vts = self.VTS_MIN elif vts > 65535 vts = 65535 end
    self.fps = self.PCLK / (self.HTS * vts)

    # Array window: w + 8 columns and h + 4 lines, which the ISP offsets of 7 and 4 below
    # cut to the output. Starts stay even, so the colour filter phase does not move.
    var center_x = (self.ARRAY_W - w) / 2
    var center_y = (self.ARRAY_H - h) / 2 + 4
    var sx = center_x + x
    var sy = center_y + y
    var max_x = self.X_LAST - w - 7
    var max_y = self.Y_LAST - h - 3
    if sx < 0 sx = 0 elif sx > max_x sx = max_x end
    if sy < 4 sy = 4 elif sy > max_y sy = max_y end
    sx = (sx / 2) * 2
    sy = (sy / 2) * 2
    self.offset_x = sx - center_x
    self.offset_y = sy - center_y
    var ex = sx + w + 7
    var ey = sy + h + 3

    # Streams keep 14 lines of guard; a still exposes up to 4 lines short of the frame.
    var exposure = vts - (self.still ? 4 : 14)
    if exposure < 4 exposure = 4 end
    # 0x3820 bit 3 mirrors, bit 4 flips; the colour filter order follows
    var orient = (self.flip ? 2 : 0) | (self.mirror ? 1 : 0)
    var r3820 = 0xA0 | (self.mirror ? 0x08 : 0) | (self.flip ? 0x10 : 0)
    self.bayer = [2, 3, 0, 1][orient]
    log(format("CAM: OV02C10 %dx%d %d fps, VTS %d, MIPI %d Mbit/s",
      w, h, self.fps, vts, self.mipi_clock))

    return [
      # PLL and system
      [0x0301, 0x08], [0x0303, 0x06], [0x0304, 0x01], [0x0305, 0x90],
      [0x0313, 0x40], [0x031C, 0x4F],
      [0x301B, 0xF0], [0x3020, 0x97], [0x3022, 0x01], [0x3026, 0xB4],
      [0x3027, 0xF1], [0x303B, 0x00], [0x303C, 0x4F], [0x303D, 0xE6],
      [0x303E, 0x00], [0x303F, 0x03], [0x3021, 0x23],
      # Exposure and gain
      [0x3501, (exposure >> 8) & 0xFF], [0x3502, exposure & 0xFF], [0x3504, 0x0C],
      [0x3507, 0x00], [0x3508, 0x40], [0x3509, 0x00],
      [0x350A, 0x01], [0x350B, 0x00], [0x350C, 0x41],
      # Analog
      [0x3600, 0x84], [0x3603, 0x08], [0x3610, 0x57], [0x3611, 0x1B],
      [0x3613, 0x78], [0x3623, 0x00], [0x3632, 0xA0], [0x3642, 0xE8],
      [0x364C, 0x70], [0x365F, 0x0F],
      # Sensor core
      [0x3708, 0x30], [0x3714, 0x24], [0x3725, 0x02], [0x3737, 0x08],
      [0x3739, 0x28], [0x3749, 0x32], [0x374A, 0x32], [0x374B, 0x32],
      [0x374C, 0x32], [0x374D, 0x81], [0x374E, 0x81], [0x374F, 0x81],
      [0x3752, 0x36], [0x3753, 0x36], [0x3754, 0x36], [0x3761, 0x00],
      [0x376C, 0x81], [0x3774, 0x18], [0x3776, 0x08], [0x377C, 0x81],
      [0x377D, 0x81], [0x377E, 0x81], [0x37A0, 0x44], [0x37A6, 0x44],
      [0x37AA, 0x0D], [0x37AE, 0x00], [0x37CB, 0x03], [0x37CC, 0x01],
      [0x37D8, 0x02], [0x37D9, 0x10], [0x37E1, 0x10], [0x37E2, 0x18],
      [0x37E3, 0x08], [0x37E4, 0x08], [0x37E5, 0x02], [0x37E6, 0x08],
      # Geometry: array window, output size, line and frame length, ISP offsets
      [0x3800, (sx >> 8) & 0xFF], [0x3801, sx & 0xFF],
      [0x3802, (sy >> 8) & 0xFF], [0x3803, sy & 0xFF],
      [0x3804, (ex >> 8) & 0xFF], [0x3805, ex & 0xFF],
      [0x3806, (ey >> 8) & 0xFF], [0x3807, ey & 0xFF],
      [0x3808, (w >> 8) & 0xFF], [0x3809, w & 0xFF],
      [0x380A, (h >> 8) & 0xFF], [0x380B, h & 0xFF],
      [0x380C, (self.HTS >> 8) & 0xFF], [0x380D, self.HTS & 0xFF],
      [0x380E, (vts >> 8) & 0xFF], [0x380F, vts & 0xFF],
      [0x3810, 0x00], [0x3811, 0x07], [0x3812, 0x00], [0x3813, 0x04],
      [0x3814, 0x01], [0x3815, 0x01], [0x3816, 0x01], [0x3817, 0x01],
      # ISP and readout
      [0x3820, r3820], [0x3821, 0x00], [0x3822, 0x80], [0x3823, 0x08],
      [0x3824, 0x00], [0x3825, 0x20], [0x3826, 0x00], [0x3827, 0x08],
      [0x382A, 0x00], [0x382B, 0x08], [0x382D, 0x00], [0x382E, 0x00],
      [0x382F, 0x23], [0x3834, 0x00], [0x3839, 0x00], [0x383A, 0xD1],
      [0x383E, 0x03],
      # Phase and timing
      [0x393D, 0x29], [0x393F, 0x6E],
      [0x394B, 0x06], [0x394C, 0x06], [0x394D, 0x08], [0x394E, 0x0A],
      [0x394F, 0x01], [0x3950, 0x01], [0x3951, 0x01], [0x3952, 0x01],
      [0x3953, 0x01], [0x3954, 0x01], [0x3955, 0x01], [0x3956, 0x01],
      [0x3957, 0x0E], [0x3958, 0x08], [0x3959, 0x08], [0x395A, 0x08],
      [0x395B, 0x13], [0x395C, 0x09], [0x395D, 0x05], [0x395E, 0x02],
      [0x395F, 0x00], [0x3960, 0x00], [0x3961, 0x00], [0x3962, 0x00],
      [0x3963, 0x00], [0x3964, 0x00], [0x3965, 0x00], [0x3966, 0x00],
      [0x3967, 0x00], [0x3968, 0x01], [0x3969, 0x01], [0x396A, 0x01],
      [0x396B, 0x01], [0x396C, 0x10], [0x396D, 0xF0], [0x396E, 0x11],
      [0x396F, 0x00], [0x3970, 0x37], [0x3971, 0x37], [0x3972, 0x37],
      [0x3973, 0x37], [0x3974, 0x00], [0x3975, 0x3C], [0x3976, 0x3C],
      [0x3977, 0x3C], [0x3978, 0x3C],
      # Clock and debug
      [0x3C00, 0x0F], [0x3C20, 0x01], [0x3C21, 0x08],
      [0x3F00, 0x8B], [0x3F02, 0x0F],
      # Black level and ISP control
      [0x4000, 0xC3], [0x4001, 0xE0], [0x4002, 0x00], [0x4003, 0x40],
      [0x4008, 0x04], [0x4009, 0x23], [0x400A, 0x04], [0x400B, 0x01],
      [0x4041, 0x20],
      [0x4077, 0x06], [0x4078, 0x00], [0x4079, 0x1A], [0x407A, 0x7F],
      [0x407B, 0x01], [0x4080, 0x03], [0x4081, 0x84],
      # Format
      [0x4308, 0x03], [0x4309, 0xFF], [0x430D, 0x00],
      # MIPI
      [0x4806, 0x00], [0x4813, 0x00], [0x4837, 0x10], [0x4857, 0x05],
      [0x4884, 0x04],
      [0x4500, 0x07], [0x4501, 0x00], [0x4503, 0x00],
      [0x450A, 0x04], [0x450E, 0x00], [0x450F, 0x00],
      [0x4800, 0x64], [0x4900, 0x00], [0x4901, 0x00], [0x4902, 0x01],
      # OTP and misc
      [0x4D00, 0x03], [0x4D01, 0xD8], [0x4D02, 0xBA], [0x4D03, 0xA0],
      [0x4D04, 0xB7], [0x4D05, 0x34], [0x4D0D, 0x00],
      # ISP pipeline
      [0x5000, 0xFD], [0x5001, 0x50], [0x5006, 0x00], [0x5080, 0x40],
      [0x5181, 0x2B], [0x5202, 0xA3], [0x5206, 0x01], [0x5207, 0x00],
      [0x520A, 0x01], [0x520B, 0x00],
      # 2-lane tail
      [0x365D, 0x00], [0x4815, 0x40], [0x4816, 0x12], [0x481F, 0x30],
      [0x4F00, 0x01], [0x0316, 0x90], [0x3016, 0x32],
      [0x3503, 0x00], # from the source table; on-sensor AEC/AGC is not qualified
      [self.REG_END, 0x00]
    ]
  end

  def configure(x, y, w, h, fps)
    if !self.detect() return false end
    if !self.write_array(self.common_regs()) return false end
    return self.write_array(self.regs_custom(x, y, w, h, fps))
  end

  # --- cam module interface ----------------------------------------------

  def start(resolution)
    if resolution < 0 || resolution >= size(self.PRESETS)
      log(format("CAM: OV02C10 has no preset %i", resolution))
      return false
    end
    var p = self.PRESETS[resolution]
    return self.configure(0, 0, p[0], p[1], p[2])
  end

  # RAW10 without binning only: bin and fmt are ignored, and the reported mode says so
  def window(x, y, w, h, bin, fps, fmt)
    if w > self.max_width || h > self.max_height return false end
    return self.configure(x, y, w, h, fps)
  end

  def stream(on)
    if !self.write_reg16(0x0100, on ? 0x01 : 0x00) return false end
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
