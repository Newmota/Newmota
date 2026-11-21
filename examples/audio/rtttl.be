# RTTTL ringtones on a PWM buzzer or I2S, with the command Rtttl <song>; Rtttl without a song
# stops. Needs USE_BERRY_DRIVERS. The buzzer (USE_BERRY_GPIO) is the template's Buzzer or
# Buzzer_i pin, or the GPIO given to Rtttl(); Rtttl("i2s") plays on the I2SConfig Tx output
# instead (USE_BERRY_AUDIO). The second value is the volume 0..100 (50% duty at 100 on PWM).
# Upload it, then in autoexec.be: load("rtttl.be")  rtttl = Rtttl()
#
# Rtttl Dkong:d=4,o=5,b=160:2c,8d.,d#.,c.,16b,16c6,16b,16c6,16b,16c6,16b,2c6

class Rtttl
  # C to B in octave 8, Hz; each lower octave halves them.
  static var FREQUENCIES = [4186, 4435, 4699, 4978, 5274, 5588, 5920, 6272, 6645, 7040, 7459,
    7902]
  static var STEPS = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11, "h": 11}

  var pin, i2s, session, volume, notes, position

  def init(output, volume)
    if volume == nil volume = 50 end
    self.volume = volume < 0 ? 0 : volume > 100 ? 100 : volume
    self.i2s = output == "i2s"
    if !self.i2s
      import gpio
      var pin = output
      if pin == nil pin = gpio.pin(gpio.BUZZER) end
      if pin < 0 pin = gpio.pin(gpio.BUZZER_INV) end
      if pin < 0 raise "value_error", "no Buzzer pin; assign one or pass a GPIO" end
      self.pin = pin
    end
    self.notes = []
    newmota.add_cmd("Rtttl", / cmd, idx, payload -> self.command(payload))
  end

  def command(payload)
    if payload == ""
      self.stop()
      return newmota.resp_cmnd_done()
    end
    try
      self.play(payload)
      newmota.resp_cmnd_done()
    except .. as e, message
      log("RTL: " + str(message), 2)
      newmota.resp_cmnd_error()
    end
  end

  def digit(c)
    return c >= "0" && c <= "9"
  end

  # Returns [Hz, ms] per note, 0 Hz for a pause, from name:d=4,o=6,b=63:notes.
  def parse(song)
    import string
    var parts = string.split(string.tr(string.tolower(song), " ", ""), ":")
    if size(parts) != 3 raise "value_error", "RTTTL is name:settings:notes" end
    var length = 4
    var octave = 6
    var bpm = 63
    for setting: string.split(parts[1], ",")
      if size(setting) < 3 || setting[1] != "=" continue end
      var value = int(setting[2 ..])
      if setting[0] == "d" length = value
      elif setting[0] == "o" octave = value
      elif setting[0] == "b" bpm = value
      end
    end
    if bpm <= 0 || length <= 0 raise "value_error", "RTTTL needs b and d above 0" end
    var whole = 240000 / bpm
    var notes = []
    for token: string.split(parts[2], ",")
      if size(token) == 0 continue end
      var i = 0
      var divider = 0
      while i < size(token) && self.digit(token[i])
        divider = divider * 10 + int(token[i])
        i += 1
      end
      if divider == 0 divider = length end
      if i == size(token) raise "value_error", "RTTTL note without pitch: " + token end
      var step = self.STEPS.find(token[i])
      if step == nil && token[i] != "p" raise "value_error", "RTTTL note not valid: " + token end
      i += 1
      if i < size(token) && token[i] == "#"
        if step != nil step += 1 end
        i += 1
      end
      var ms = whole / divider
      if i < size(token) && token[i] == "."
        ms = ms * 3 / 2
        i += 1
      end
      var note_octave = octave
      if i < size(token) && self.digit(token[i])
        note_octave = int(token[i])
        i += 1
      end
      if i < size(token) && token[i] == "."
        ms = ms * 3 / 2
        i += 1
      end
      if i != size(token) raise "value_error", "RTTTL note not valid: " + token end
      var hz = 0
      if step != nil
        if step > 11
          step -= 12
          note_octave += 1
        end
        if note_octave < 1 || note_octave > 8
          raise "value_error", "RTTTL octave not valid: " + token
        end
        hz = self.FREQUENCIES[step] >> (8 - note_octave)
      end
      notes.push([hz, ms])
    end
    return notes
  end

  # A song that does not parse leaves the current one playing; a new one keeps the I2S session.
  def play(song)
    var notes = self.parse(song)
    self.halt()
    self.notes = notes
    self.position = 0
    self.next()
  end

  # Each note sounds for 90% of its time, so repeated notes stay apart.
  def next()
    if self.position >= size(self.notes) return self.stop() end
    var note = self.notes[self.position]
    self.position += 1
    self.sound(note[0])
    if note[0] == 0 return newmota.set_timer(note[1], / -> self.next(), "rtttl") end
    var sound = note[1] * 9 / 10
    newmota.set_timer(sound, / -> self.rest(note[1] - sound), "rtttl")
  end

  def rest(ms)
    self.sound(0)
    newmota.set_timer(ms, / -> self.next(), "rtttl")
  end

  # 0 Hz is silence. The tone session starts with the first note and is replaced once it ended.
  def sound(hz)
    if !self.i2s
      import gpio
      if hz > 0 gpio.set_pwm_freq(self.pin, hz) end
      gpio.set_pwm(self.pin, hz > 0 ? self.volume * 512 / 100 : 0)
      return
    end
    if self.session != nil && self.session.status()["state"] != 1 self.release() end
    if self.session == nil
      if hz == 0 return end
      import audio
      self.session = audio.Session({"source": "text", "codec": "tone"})
      self.session.start()
    end
    self.session.tone(hz, self.volume)
  end

  def halt()
    newmota.remove_timer("rtttl")
    self.notes = []
    self.sound(0)
  end

  # Stopping plays out the queued tail; the session is closed once it has ended.
  def stop()
    self.halt()
    if self.session != nil
      self.session.stop()
      self.release()
    end
  end

  def release()
    if self.session == nil return end
    if self.session.status()["state"] == 1
      return newmota.set_timer(100, / -> self.release(), "rtttl")
    end
    self.session.close()
    self.session = nil
  end
end
