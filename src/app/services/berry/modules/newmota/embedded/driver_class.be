# Adapted from Tasmota lib/libesp32/berry_tasmota/src/embedded/driver_class.be
# Source pin: cced3d1d9396c1d0b3eeb77d86c1fdcb9cca9748.
# SPDX-License-Identifier: GPL-3.0-or-later
# Solidified as the global Driver class: an instance whose hook variables a script fills in,
# before or after newmota.add_driver(). Tasmota's variables are kept, including the web ones
# Newmota does not call, so assigning them still works; Newmota's other hooks are added.

#@ solidify:Driver

class Driver
  var every_second
  var every_100ms
  var web_add_handler
  var web_add_button
  var web_add_main_button
  var web_add_management_button
  var web_add_config_button
  var web_add_console_button
  var save_before_restart
  var web_sensor
  var json_append
  var after_teleperiod
  var button_pressed
  var display
  # Newmota hooks that Tasmota's Driver class does not declare.
  var every_50ms
  var every_250ms
  var any_key
  var switch_action
  var button_multi_pressed
  var set_power_handler
  var network_up
  var network_down
  var audio

  def add_cmd(c, f)
    newmota.add_cmd(c, / cmd, idx, payload, payload_json -> f(self, cmd, idx, payload, payload_json))
  end
end
