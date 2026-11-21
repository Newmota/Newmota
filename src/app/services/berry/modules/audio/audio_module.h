/*
  audio_module.h - Berry native audio session lifecycle

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_module.h
 * @brief Registers the `audio` module: `Session`, `level` and `spectrum`.
 */
#pragma once
struct bvm;

namespace newmota {
/** @brief Installs the `audio` module classes without exposing samples or command wrappers. */
bool install_audio_module(bvm*);
/** @brief Stops all VM-owned sessions before VM teardown. */
void remove_audio_module();
} // namespace newmota
