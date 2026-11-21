# OV2640 camera sensor driver for Newmota's `cam` module (DVP, JPEG on the sensor).
#
# Upload to the filesystem and start it from autoexec.be:
#   load("ov2640.be")  cam.register(OV2640())
# or build it in with the environment option `solidify` and only register it.
# The camera then starts with the stored WcResolution; WcStream 1 serves port 81.
#
# Register tables, window calculation and setting levels are ported from
# espressif/esp32-camera sensors/ov2640.c and ov2640_settings.h,
# Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD, Apache License 2.0.

#@ solidify:OV2640
#@ requires:USE_WEBCAM

class OV2640
  static ADDR = 0x30
  static PID = 0x26
  static BANK_DSP = 0x00
  static BANK_SENSOR = 0x01

  # Base setup, written after a soft reset (ov2640_settings_cif)
  static INIT_REGS =
    "FF002CFF2EDFFF013C3211010902042813E514482C0C33783A333BFB3E004311" +
    "1610399235DA221A37C3230034C0068807C00D870E414C004A81219924402538" +
    "26825C006300617062807C05208028306C006D806E007002719473C13D345A57" +
    "4FBB509C12201711184319001A25328937C04FCA50A86D003D38FF00E57FF9C0" +
    "4124E01476FF33A0422043184C008750883FD703D910D382C808C9807C007D00" +
    "7C037D487D487C087D207D107D0E9000910E911A9131915A91699175917E9188" +
    "918F919691A391AF91C491D791E891209200930693E393059305930093049300" +
    "9300930093009300930093009600970897199702970C97249730972897269702" +
    "9798978097009700A400A800C511C651BF80C710B666B8A5B764B97CB3AFB497" +
    "B5FFB0C5B194B20FC45CC3FD7F00E51FE167DD7FDA00E0000500"

  # Sensor window per output mode (ov2640_settings_to_cif/svga/uxga)
  static MODE_CIF =
    "FF011220030A32891711184319001A254FCA50A85A236D003D38399235DA221A" +
    "37C3230034C0068807C00D870E414C00FF00E004C032C1258C005164524A5300" +
    "540055005700863D5080"
  static MODE_SVGA =
    "FF011240030A32091711184319001A4B37C04FCA50A85A236D003D38399235DA" +
    "221A37C3230034C0068807C00D870E4142034C00FF00E004C064C14B8C0051C8" +
    "52965300540055005700863D5080"
  static MODE_UXGA =
    "FF011200030F32361711187519011A973D344FBB509C5A576D803982230007C0" +
    "4C003588220A374034A006020DB70E014283FF00E004C0C8C1968C005190522C" +
    "5300540055885700863D5000"

  # JPEG output (ov2640_settings_jpeg3)
  static JPEG_REGS =
    "FF00E014DA12D703E177E51FD910DF8033803C10EB30DD7FE000"

  # Tasmota framesize index: width, height, aspect ratio row
  static SIZES = [
    [96, 96, 7], [160, 120, 0], [128, 128, 7], [176, 144, 6],
    [240, 176, 0], [240, 240, 7], [320, 240, 0], [320, 320, 7],
    [400, 296, 0], [480, 320, 1], [640, 480, 0], [800, 600, 0],
    [1024, 768, 0], [1280, 720, 4], [1280, 1024, 6], [1600, 1200, 0],
  ]
  static CIF_LAST = 8
  static SVGA_LAST = 11
  # Aspect ratio rows: offset x, offset y, max x, max y on the 1600x1200 array
  static RATIOS = [
    [0, 0, 1600, 1200], [8, 72, 1584, 1056], [0, 100, 1600, 1000],
    [0, 120, 1600, 960], [0, 150, 1600, 900], [2, 258, 1596, 684],
    [50, 0, 1500, 1200], [200, 0, 1200, 1200], [462, 0, 676, 1200],
  ]

  # Setting levels: register row first, then one row per level
  static BRIGHTNESS = [[0x7C, 0x7D, 0x7C, 0x7D, 0x7D],
    [0x00, 0x04, 0x09, 0x00, 0x00], [0x00, 0x04, 0x09, 0x10, 0x00],
    [0x00, 0x04, 0x09, 0x20, 0x00], [0x00, 0x04, 0x09, 0x30, 0x00],
    [0x00, 0x04, 0x09, 0x40, 0x00]]
  static CONTRAST = [[0x7C, 0x7D, 0x7C, 0x7D, 0x7D, 0x7D, 0x7D],
    [0x00, 0x04, 0x07, 0x20, 0x18, 0x34, 0x06], [0x00, 0x04, 0x07, 0x20, 0x1C, 0x2A, 0x06],
    [0x00, 0x04, 0x07, 0x20, 0x20, 0x20, 0x06], [0x00, 0x04, 0x07, 0x20, 0x24, 0x16, 0x06],
    [0x00, 0x04, 0x07, 0x20, 0x28, 0x0C, 0x06]]
  static SATURATION = [[0x7C, 0x7D, 0x7C, 0x7D, 0x7D],
    [0x00, 0x02, 0x03, 0x28, 0x28], [0x00, 0x02, 0x03, 0x38, 0x38],
    [0x00, 0x02, 0x03, 0x48, 0x48], [0x00, 0x02, 0x03, 0x58, 0x58],
    [0x00, 0x02, 0x03, 0x68, 0x68]]
  static EFFECTS = [[0x7C, 0x7D, 0x7C, 0x7D, 0x7D],
    [0x00, 0x00, 0x05, 0x80, 0x80], [0x00, 0x40, 0x05, 0x80, 0x80],
    [0x00, 0x18, 0x05, 0x80, 0x80], [0x00, 0x18, 0x05, 0x40, 0xC0],
    [0x00, 0x18, 0x05, 0x40, 0x40], [0x00, 0x18, 0x05, 0xA0, 0x40],
    [0x00, 0x18, 0x05, 0x40, 0xA6]]
  static WB_MODES = [[0xCC, 0xCD, 0xCE],
    [0x5E, 0x41, 0x54], [0x65, 0x41, 0x4F], [0x52, 0x41, 0x66], [0x42, 0x3F, 0x71]]
  static AE_LEVELS = [[0x24, 0x25, 0x26],
    [0x20, 0x18, 0x60], [0x34, 0x1C, 0x00], [0x3E, 0x38, 0x81],
    [0x48, 0x40, 0x81], [0x58, 0x50, 0x92]]
  static AGC_GAIN = [0x00, 0x10, 0x18, 0x30, 0x34, 0x38, 0x3C, 0x70, 0x72, 0x74, 0x76,
    0x78, 0x7A, 0x7C, 0x7E, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9,
    0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF]

  var wire, name, width, height, xclk

  def init()
    self.name = "OV2640"
    self.xclk = 20000000
    self.width = 0
    self.height = 0
  end

  # --- register access -----------------------------------------------------

  def regs(hex)
    import cam
    return cam.write_regs(self.wire, self.ADDR, bytes(hex))
  end

  def write(bank, reg, value)
    import cam
    return cam.write_reg(self.wire, self.ADDR, 0xFF, bank) &&
           cam.write_reg(self.wire, self.ADDR, reg, value)
  end

  def read(bank, reg)
    import cam
    if !cam.write_reg(self.wire, self.ADDR, 0xFF, bank) return nil end
    return cam.read_reg(self.wire, self.ADDR, reg)
  end

  def bits(bank, reg, offset, mask, value)
    var current = self.read(bank, reg)
    if current == nil return false end
    var updated = (current & ~(mask << offset)) | ((value & mask) << offset)
    return self.write(bank, reg, updated)
  end

  def flag(bank, reg, mask, enable)
    return self.bits(bank, reg, 0, mask, enable ? mask : 0)
  end

  # Writes one level row of a setting table: the first row names the registers.
  def level(bank, table, row)
    var names = table[0]
    var values = table[row]
    var i = 0
    while i < size(names)
      if !self.write(bank, names[i], values[i]) return false end
      i += 1
    end
    return true
  end

  # --- start-up ------------------------------------------------------------

  def detect()
    self.wire = newmota.wire_scan(self.ADDR)
    if self.wire == nil
      log("CAM: OV2640 not found on I2C 0x30")
      return false
    end
    var pid = self.read(self.BANK_SENSOR, 0x0A)
    if pid != self.PID
      log(format("CAM: unexpected sensor PID %s at 0x30", pid))
      return false
    end
    log(format("CAM: OV2640 version %s", self.read(self.BANK_SENSOR, 0x0B)))
    return true
  end

  def reset()
    if !self.write(self.BANK_SENSOR, 0x12, 0x80) return false end   # COM7 soft reset
    newmota.delay(10)
    return self.regs(self.INIT_REGS)
  end

  def jpeg()
    var ok = self.regs(self.JPEG_REGS)
    newmota.delay(10)
    return ok
  end

  # Selects the sensor mode and scales the window to the output size (set_framesize).
  def window(resolution)
    import cam
    var frame = self.SIZES[resolution]
    var w = frame[0]
    var h = frame[1]
    var ratio = self.RATIOS[frame[2]]
    var offset_x = ratio[0]
    var offset_y = ratio[1]
    var max_x = ratio[2]
    var max_y = ratio[3]
    var mode = self.MODE_UXGA
    var pclk = 12
    if resolution <= self.CIF_LAST
      mode = self.MODE_CIF
      pclk = 8
      max_x /= 4; max_y /= 4; offset_x /= 4; offset_y /= 4
      if max_y > 296 max_y = 296 end
    elif resolution <= self.SVGA_LAST
      mode = self.MODE_SVGA
      pclk = 8
      max_x /= 2; max_y /= 2; offset_x /= 2; offset_y /= 2
    end
    max_x /= 4; max_y /= 4
    var zw = w / 4
    var zh = h / 4
    var win = bytes()
    for pair : [
        [0xFF, self.BANK_DSP],
        [0x51, max_x & 0xFF], [0x52, max_y & 0xFF],                  # HSIZE, VSIZE
        [0x53, offset_x & 0xFF], [0x54, offset_y & 0xFF],            # XOFFL, YOFFL
        [0x55, ((max_y >> 1) & 0x80) | ((offset_y >> 4) & 0x70) |
               ((max_x >> 5) & 0x08) | ((offset_x >> 8) & 0x07)],    # VHYX
        [0x57, (max_x >> 2) & 0x80],                                 # TEST
        [0x5A, zw & 0xFF], [0x5B, zh & 0xFF],                        # ZMOW, ZMOH
        [0x5C, ((zh >> 6) & 0x04) | ((zw >> 8) & 0x03)]]             # ZMHH
      win.add(pair[0], 1)
      win.add(pair[1], 1)
    end
    if !self.write(self.BANK_DSP, 0x05, 0x01) return false end       # R_BYPASS: DSP bypass
    if !self.regs(mode) return false end
    if !cam.write_regs(self.wire, self.ADDR, win) return false end
    if !self.write(self.BANK_SENSOR, 0x11, 0x00) return false end    # CLKRC: no divider in JPEG
    if !self.write(self.BANK_DSP, 0xD3, pclk) return false end       # R_DVP_SP: PCLK divider
    if !self.write(self.BANK_DSP, 0x05, 0x00) return false end       # R_BYPASS: DSP on
    newmota.delay(10)
    self.width = w
    self.height = h
    return self.jpeg()                                               # required after a mode change
  end

  # --- cam module interface ----------------------------------------------

  def start(resolution)
    if resolution < 0 || resolution >= size(self.SIZES)
      log(format("CAM: OV2640 has no resolution %i", resolution))
      return false
    end
    if !self.detect() return false end
    if !self.reset() return false end
    if !self.jpeg() return false end
    return self.window(resolution)
  end

  def stream(on)
    return self.bits(self.BANK_SENSOR, 0x09, 4, 1, on ? 0 : 1)       # COM2 standby
  end

  def set(key, value)
    var dsp = self.BANK_DSP
    var sensor = self.BANK_SENSOR
    if key == "brightness"   return self.level(dsp, self.BRIGHTNESS, value + 3)
    elif key == "contrast"   return self.level(dsp, self.CONTRAST, value + 3)
    elif key == "saturation" return self.level(dsp, self.SATURATION, value + 3)
    elif key == "effect"     return self.level(dsp, self.EFFECTS, value + 1)
    elif key == "ae_level"   return self.level(sensor, self.AE_LEVELS, value + 3)
    elif key == "wb_mode"
      if !self.bits(dsp, 0xC7, 6, 1, value ? 1 : 0) return false end
      return value == 0 || self.level(dsp, self.WB_MODES, value)
    elif key == "quality"    return self.write(dsp, 0x44, value)              # QS
    elif key == "agc_gain"   return self.write(sensor, 0x00, self.AGC_GAIN[value])
    elif key == "gainceiling" return self.bits(sensor, 0x14, 5, 7, value)     # COM9
    elif key == "aec_value"
      return self.bits(sensor, 0x04, 0, 3, value & 3) &&                     # REG04
             self.write(sensor, 0x10, (value >> 2) & 0xFF) &&                # AEC
             self.bits(sensor, 0x45, 0, 0x3F, value >> 10)                   # REG45
    elif key == "aec2"       return self.bits(dsp, 0xC2, 6, 1, value ? 0 : 1) # CTRL0
    elif key == "colorbar"   return self.flag(sensor, 0x12, 0x02, value)      # COM7
    elif key == "agc"        return self.flag(sensor, 0x13, 0x04, value)      # COM8
    elif key == "aec"        return self.flag(sensor, 0x13, 0x01, value)      # COM8
    elif key == "mirror"     return self.flag(sensor, 0x04, 0x80, value)      # REG04
    elif key == "flip"
      return self.flag(sensor, 0x04, 0x10, value) && self.flag(sensor, 0x04, 0x40, value)
    elif key == "raw_gma"    return self.bits(dsp, 0xC3, 5, 1, value)         # CTRL1
    elif key == "awb"        return self.bits(dsp, 0xC3, 3, 1, value)
    elif key == "awb_gain"   return self.bits(dsp, 0xC3, 2, 1, value)
    elif key == "lenc"       return self.bits(dsp, 0xC3, 1, 1, value)
    elif key == "dcw"        return self.bits(dsp, 0x86, 5, 1, value)         # CTRL2
    elif key == "bpc"        return self.bits(dsp, 0x87, 7, 1, value)         # CTRL3
    elif key == "wpc"        return self.bits(dsp, 0x87, 6, 1, value)
    end
    return false
  end
end
