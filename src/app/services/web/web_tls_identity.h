/*
  web_tls_identity.h - persistent self-signed identity for the web server

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/platform/web.h"

namespace newmota {

/** @brief Loads or creates the device's persistent HTTPS identity. */
[[nodiscard]] bool prepare_web_tls_identity();

/** @brief Returns the identity after successful preparation. */
[[nodiscard]] const platform::WebTlsIdentity& web_tls_identity();

} // namespace newmota
