# IMX219 MIPI-CSI sensor driver for Newmota's `cam` module (ESP32-P4, USE_CSI_WEBCAM).
#
# Upload to the filesystem and start it from autoexec.be:
#   load("imx219.be")  cam.register(IMX219())
# or build it in with the environment option `solidify` and only register it.
# Upload isp/imx219.json as /isp.json for its colour tuning. The sensor sits on a
# configured I2C bus at 0x10. WcRes 0..4 selects a preset, WcWindow a custom window;
# its x and y shift the window from the centre. In the still session (WcSession 4) the
# presets are RAW8 with a longer exposure.
#
# Ported from Tasmota's tasmota/berry/camera_sensor/imx219.be: register tables, window
# and PLL calculation are Tasmota's; the interface follows Newmota's camera contract.

#@ solidify:IMX219
#@ requires:USE_CSI_WEBCAM

class IMX219
  static ADDR = 0x10
  static CHIP_ID = 0x0219
  static REG_END = 0xFFFF
  static REG_DELAY = 0xFFFE
  # Presets: width, height, binning, format (0 RAW8, 1 RAW10), fps
  static PRESETS = [
    [640, 480, 2, 1, 30],      # 0: VGA, bin 2, RAW10 (0.3 MP)
    [1280, 720, 2, 0, 30],     # 1: 720p, bin 2, RAW8 (0.9 MP)
    [1640, 1232, 2, 0, 30],    # 2: full bin-2 frame, RAW8 (2.0 MP)
    [1920, 1080, 1, 0, 30],    # 3: 1080p, bin 1, RAW8 (2.1 MP)
    [3280, 2464, 1, 0, 10],    # 4: full sensor, RAW8 (8.0 MP), 10 fps for bandwidth
  ]

  var wire, name, width, height, format, lanes, mipi_clock, fps, bin_mode
  var max_width, max_height, is_streaming
  var offset_x, offset_y, bayer, mirror, flip, still

  def init()
    self.name = "IMX219"
    self.width = 640
    self.height = 480
    self.format = 1
    self.lanes = 2
    self.mipi_clock = 456
    self.fps = 30
    self.bin_mode = 2
    self.max_width = 3280
    self.max_height = 2464
    self.is_streaming = false
    self.offset_x = 0
    self.offset_y = 0
    self.bayer = 3
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
        log(format("CAM: IMX219 write failed at 0x%04X", reg))
        return false
      end
    end
    return true
  end

  def detect()
    self.wire = newmota.wire_scan(self.ADDR)
    if self.wire == nil
      log("CAM: IMX219 not found on I2C 0x10")
      return false
    end
    newmota.delay(10)
    var high = self.read_reg16(0x0000)
    var low = self.read_reg16(0x0001)
    if high == nil || low == nil return false end
    return ((high << 8) | low) == self.CHIP_ID
  end

  def common_regs()
    return [
      [0x0103, 0x01], [self.REG_DELAY, 10], [0x0100, 0x00],
      [0x30eb, 0x05], [0x30eb, 0x0c], [0x300a, 0xff], [0x300b, 0xff],
      [0x30eb, 0x05], [0x30eb, 0x09],
      [0x455e, 0x00], [0x471e, 0x4b], [0x4767, 0x0f], [0x4750, 0x14],
      [0x4540, 0x00], [0x47b4, 0x14], [0x4713, 0x30], [0x478b, 0x10],
      [0x478f, 0x10], [0x4793, 0x10], [0x4797, 0x0e], [0x479b, 0x0e],
      [0x0114, 0x01], [0x0128, 0x00], [0x012a, 0x18], [0x012b, 0x00],
      [0x0301, 0x05], [0x0303, 0x01], [0x0304, 0x03], [0x0305, 0x03],
      [0x030b, 0x01],
      [self.REG_END, 0x00]
    ]
  end

  # x and y shift the window from the centre in output pixels
  def regs_custom(x, y, w, h, bin, fps, fmt)
    self.width = w
    self.height = h
    self.bin_mode = bin
    self.format = fmt

    var reg_fmt = (fmt == 0) ? 0x08 : 0x0A
    var reg_bin = (bin == 2) ? 0x01 : 0x00
    var scale = (bin == 2) ? 2 : 1
    var eff_w = w * scale
    var eff_h = h * scale
    # Native coordinates in the 3280x2464 array, clamped, then even for the Bayer phase
    var center_x = (3280 - eff_w) / 2
    var center_y = (2464 - eff_h) / 2
    var sx = center_x + x * scale
    var sy = center_y + y * scale
    if sx < 0 sx = 0 elif sx > 3280 - eff_w sx = 3280 - eff_w end
    if sy < 0 sy = 0 elif sy > 2464 - eff_h sy = 2464 - eff_h end
    sx = (sx / 2) * 2
    sy = (sy / 2) * 2
    self.offset_x = (sx - center_x) / scale
    self.offset_y = (sy - center_y) / scale
    var ex = sx + eff_w - 1
    var ey = sy + eff_h - 1

    # PLL (EXCLK 24 MHz, pre_div 3, PLL input 8 MHz)
    var bpp = (fmt == 0) ? 8 : 10
    var hts = 3448
    var act_fps = (fps == 0) ? 30 : fps
    # VT PLL: effective pclk = vt_mpy * 3.2 MHz
    var vt_mpy = (hts * (h + 50) * act_fps + 3199999) / 3200000
    if vt_mpy < 54 vt_mpy = 54 end
    if vt_mpy > 114 vt_mpy = 114 end
    # OP PLL: lane rate = 8 * op_mpy Mbit/s; w*bpp bits per line on 2 lanes
    var op_mpy = (w * bpp * vt_mpy + hts * 5 - 1) / (hts * 5)
    if op_mpy < 54 op_mpy = 54 end
    if op_mpy > 114 op_mpy = 114 end
    var pclk = vt_mpy * 3200000
    self.mipi_clock = 8 * op_mpy
    var vts = pclk / (hts * act_fps)
    if vts < h + 50 vts = h + 50 end
    # Streams keep 50 lines of guard; a still may expose up to 8 lines short of the frame.
    var exposure = self.still ? vts - 8 : vts - 50
    if exposure < 4 exposure = 4 end
    var gain = 0xE0
    var reg_oppxck = (fmt == 0) ? 0x08 : 0x0A
    # 0x0172 bit 0 mirrors, bit 1 flips; the colour filter order follows (Linux imx219)
    var orient = (self.flip ? 2 : 0) | (self.mirror ? 1 : 0)
    self.bayer = [3, 2, 1, 0][orient]
    log(format("CAM: IMX219 %dx%d bin %d fmt %d %d fps, VT %d, OP %d, MIPI %d Mbit/s",
      w, h, bin, fmt, act_fps, vt_mpy, op_mpy, self.mipi_clock))

    return [
      # PLL
      [0x0306, (vt_mpy >> 8) & 0x07], [0x0307, vt_mpy & 0xFF],
      [0x030c, (op_mpy >> 8) & 0x07], [0x030d, op_mpy & 0xFF],
      # Geometry and format
      [0x0164, (sx>>8)], [0x0165, (sx&0xFF)], [0x0166, (ex>>8)], [0x0167, (ex&0xFF)],
      [0x0168, (sy>>8)], [0x0169, (sy&0xFF)], [0x016a, (ey>>8)], [0x016b, (ey&0xFF)],
      [0x016c, (w>>8)], [0x016d, (w&0xFF)],
      [0x016e, (h>>8)], [0x016f, (h&0xFF)],
      [0x0172, orient],
      [0x0174, reg_bin], [0x0175, reg_bin],
      [0x018c, reg_fmt], [0x018d, reg_fmt],
      [0x0309, reg_oppxck],
      # Timing
      [0x0160, (vts>>8)], [0x0161, (vts&0xFF)],
      [0x0162, (hts>>8)], [0x0163, (hts&0xFF)],
      # Analog controls under group hold
      [0x0104, 0x01],
      [0x0157, gain],                 # analog gain
      [0x0158, 0x01], [0x0159, 0x80], # digital gain x1.5 to lift shadows
      [0x015a, (exposure >> 8) & 0xFF],
      [0x015b, exposure & 0xFF],
      [0x0104, 0x00],
      [self.REG_END, 0x00]
    ]
  end

  def configure(x, y, w, h, bin, fps, fmt)
    if !self.detect() return false end
    # Tasmota toggles streaming once before the reset sequence.
    self.write_reg16(0x0100, 0x00); newmota.delay(10)
    self.write_reg16(0x0100, 0x01); newmota.delay(10)
    self.write_reg16(0x0100, 0x00); newmota.delay(10)
    if !self.write_array(self.common_regs()) return false end
    self.fps = (fps == 0) ? 30 : fps
    return self.write_array(self.regs_custom(x, y, w, h, bin, fps, fmt))
  end

  # --- cam module interface ----------------------------------------------

  def start(resolution)
    if resolution < 0 || resolution >= size(self.PRESETS)
      log(format("CAM: IMX219 has no preset %i", resolution))
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
