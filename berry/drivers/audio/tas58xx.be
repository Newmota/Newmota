# TAS5805M and TAS5825M stereo Class-D amplifiers with I2S input, on I2C, adapted from ESPHome's
# esphome/components/tas58xx, source pin 4969f643cd42ee8a9caf2c3649899bcc43a41622.
# Copyright (c) 2019 ESPHome.
# SPDX-License-Identifier: GPL-3.0-only
#
# autoexec.be:  load("tas58xx.be")  amp = TAS5805M(5)  newmota.add_driver(amp)
# app:          amp.set_volume(60)  amp.set_mixer(TAS58xx.LEFT)
#
# TAS5805M(pdn) pulses the power-down pin PDN, nil when it is wired high; then pbtl (true: one
# speaker on both bridges, mono mixer) and the I2C address (TAS5805M 0x2D, TAS5825M 0x4C) and
# bus. Setup leaves the amplifier playing at 0 dB digital volume and -15.5 dB analog gain, waiting
# in Hi-Z until the I2S clocks run, with FAULTZ on ADR (TAS5805M) or GPIO1 (TAS5825M). Registered
# as a driver it logs fault changes every second and writes a mixer change once the clocks run,
# the only time the DSP takes coefficients, and follows the output: deep sleep while it is idle,
# play when it starts. DC and over-current faults keep the output off until activate(); a start
# does not clear them.

#@ solidify:TAS58xx
#@ solidify:TAS5805M
#@ solidify:TAS5825M
#@ requires:USE_I2C,USE_I2S,USE_BERRY_GPIO

class TAS58xx
  static var STEREO = 0
  static var INVERSE = 1
  static var MONO = 2
  static var LEFT = 3
  static var RIGHT = 4
  # Left to left, right to left, left to right and right to right per mixer mode, as the second
  # byte of each 9.23 coefficient: 0x80 is 0 dB, 0x40 is -6 dB, 0x00 mutes.
  static var MIXES = [bytes("80000080"), bytes("00808000"), bytes("40404040"), bytes("80008000"),
    bytes("00800080")]
  # Device states of DEVICE_CTRL_2 and POWER_STATE.
  static var DEEP_SLEEP = 0
  static var HIZ = 2
  static var PLAY = 3
  static var OUTPUT_OFF = 0x0000000F

  var bus, address, pbtl, state, muted, mixer, mixer_written, logged

  def init(pdn, pbtl, address, bus)
    self.address = address != nil ? address : self.ADDRESS
    self.pbtl = pbtl == true
    self.muted = false
    self.mixer = self.pbtl ? self.MONO : self.STEREO
    self.mixer_written = false
    self.logged = 0
    if pdn != nil
      import gpio
      gpio.pin_mode(pdn, gpio.OUTPUT)
      gpio.digital_write(pdn, 0)
      newmota.delay(1)
      gpio.digital_write(pdn, 1)
      newmota.delay(5)
    end
    self.bus = bus != nil ? bus : newmota.wire_scan(self.address)
    if self.bus == nil raise "io_error", "TAS58xx not found" end
    # TI PurePath Console header: book 0, Hi-Z, reset DSP and control registers, Hi-Z again.
    var ok = self.select(0, 0) && self.write(0x03, self.HIZ) && self.write(0x01, 0x11) &&
      self.write(0x03, self.HIZ)
    newmota.delay(5)
    # The address is latched at power-up, so FAULTZ may now take its pin: function before enable.
    ok = ok && self.write_pairs(self.STARTUP) && self.write(0x61 + self.FAULT_PIN, 0x0B) &&
      self.write(0x60, 1 << self.FAULT_PIN) && self.write(0x02, self.pbtl ? 0x04 : 0x00) &&
      self.write(0x54, 0x1F) && self.write(0x4C, 0x30) && self.set_state(self.PLAY) &&
      self.write(0x78, 0x80)
    if !ok raise "io_error", "TAS58xx setup failed" end
  end

  def write(reg, value)
    return self.bus.write(self.address, reg, value, 1)
  end

  def write_pairs(pairs)
    for i: 0 .. pairs.size() / 2 - 1
      if !self.write(pairs[2 * i], pairs[2 * i + 1]) return false end
    end
    return true
  end

  # The book can only change from page 0.
  def select(book, pg)
    return self.write(0x00, 0x00) && self.write(0x7F, book) && self.write(0x00, pg)
  end

  def set_state(state)
    if !self.write(0x03, state | (self.muted ? 0x08 : 0x00)) return false end
    self.state = state
    return true
  end

  def playing()
    var state = self.bus.read(self.address, 0x68, 1)
    return state != nil && (state & 0x03) == self.PLAY
  end

  # Clears faults and plays; this also restarts the output after a DC or over-current fault.
  def activate()
    return self.select(0, 0) && self.write(0x78, 0x80) && self.play()
  end

  # Leaving deep sleep resets the state machine with Hi-Z, deep sleep, Hi-Z.
  def play()
    var ok = true
    if self.state == self.DEEP_SLEEP
      ok = self.set_state(self.HIZ) && self.set_state(self.DEEP_SLEEP) && self.set_state(self.HIZ)
    end
    return ok && self.set_state(self.PLAY)
  end

  # Deep sleep, the lowest power state that keeps I2C and the DSP.
  def deactivate()
    return self.select(0, 0) && self.set_state(self.DEEP_SLEEP)
  end

  def mute(on)
    var was = self.muted
    self.muted = on
    if self.set_state(self.state) return true end
    self.muted = was
    return false
  end

  # Volume 0..100 from -103 dB to +24 dB in 0.5 dB steps.
  def set_volume(volume)
    volume = volume < 0 ? 0 : volume > 100 ? 100 : volume
    var db = -103.0 + 127.0 * volume / 100
    var step = int(0x30 - db * 2 + 0.5)
    return self.write(0x4C, step < 0 ? 0 : step > 0xFE ? 0xFE : step)
  end

  # Analog gain -15.5..0 dB in 0.5 dB steps.
  def set_analog_gain(db)
    var step = int(-db * 2 + 0.5)
    return self.write(0x54, step < 0 ? 0 : step > 0x1F ? 0x1F : step)
  end

  # STEREO, INVERSE, MONO, LEFT or RIGHT; PBTL takes MONO, LEFT or RIGHT.
  def set_mixer(mode)
    if mode < self.STEREO || mode > self.RIGHT || (self.pbtl && mode < self.MONO)
      raise "value_error", "TAS58xx mixer mode not valid here"
    end
    self.mixer = mode
    self.mixer_written = false
    return !self.playing() || self.write_mixer()
  end

  def write_mixer()
    var gains = self.MIXES[self.mixer]
    var coefficients = bytes()
    for i: 0 .. 3
      coefficients.add(0)
      coefficients.add(gains[i])
      coefficients.add(0)
      coefficients.add(0)
    end
    var at = self.MIXER
    var ok = self.select(at[0], at[1]) && self.bus.write_bytes(self.address, at[2], coefficients)
    # Always back to the control port, also after a failed write.
    ok = self.select(0, 0) && ok
    self.mixer_written = ok
    return ok
  end

  # CHAN_FAULT, GLOBAL_FAULT1, GLOBAL_FAULT2 and OT_WARNING packed from the low byte up; nil on a
  # failed read.
  def faults()
    var raw = self.bus.read_bytes(self.address, 0x70, 4)
    return raw != nil ? raw.get(0, 4) : nil
  end

  # Driver hook: audio("power", on, "tx").
  def audio(cmd, idx, payload)
    if cmd != "power" || payload != "tx" return end
    if idx != 0
      if self.select(0, 0) && self.play() && !self.mixer_written && self.playing()
        self.write_mixer()
      end
    else
      self.deactivate()
    end
  end

  def every_second()
    var faults = self.faults()
    if faults == nil return end
    var active = faults & (self.ERRORS | self.WARNINGS)
    if active != self.logged
      log(format("TAS58xx: faults %08X", active), active & self.ERRORS ? 1 : 2)
      if active & ~self.logged & self.OUTPUT_OFF
        log("TAS58xx: output off until activate()", 2)
      end
      self.logged = active
    end
    # Latched bits only report; clear them unless the output waits for activate().
    if (faults & self.LATCHED) && !(active & self.OUTPUT_OFF)
      self.write(0x78, 0x80)
    end
    if !self.mixer_written && self.playing()
      self.write_mixer()
    end
  end
end

class TAS5805M : TAS58xx
  static var ADDRESS = 0x2D
  static var FAULT_PIN = 0
  # Book, page and register of the input mixer.
  static var MIXER = [0x8C, 0x29, 0x18]
  # TI PurePath Console startup after the reset; registers 0x46, 0x7D, 0x7E and page 1 register
  # 0x51 are undocumented. Ends with ANA_CTRL 175 kHz bandwidth at Fsw 768 kHz.
  static var STARTUP = bytes("0300460103027D117EFF0001510500005360")
  # The clock fault is left out: it is set whenever the I2S clocks stop.
  static var ERRORS = 0x0001C30F
  static var WARNINGS = 0x04000000
  static var LATCHED = 0x0401C70F
end

class TAS5825M : TAS58xx
  static var ADDRESS = 0x4C
  static var FAULT_PIN = 1
  static var MIXER = [0x8C, 0x0B, 0x14]
  # TI PurePath Console "2-Band DRC&AGL 2.0 96k" startup after the reset; 0x7D, 0x7E, page 1
  # register 0x51 and page 2 registers 0x1D and 0x19 are undocumented. DSP_CTRL 96 kHz with default
  # coefficients, ANA_CTRL 100 kHz bandwidth at Fsw 384 kHz, Hi-Z, cycle-by-cycle current limit.
  static var STARTUP = bytes("03007D117EFF0001510500021D00198000004611530103027707")
  static var ERRORS = 0x0007E30F
  static var WARNINGS = 0x3C000000
  static var LATCHED = 0x3F07E70F
end
