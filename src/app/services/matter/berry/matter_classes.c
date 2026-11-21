/*
  matter_classes.c - solidified classes of the Berry matter module

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "berry.h"
#include "newmota_features.h"

#include "solidified_matter_base38.h"
#include "solidified_matter_certs.h"
#include "solidified_matter_device.h"
#include "solidified_matter_im.h"
#include "solidified_matter_model.h"
#include "solidified_matter_secure.h"
#include "solidified_matter_tlv.h"
#if USE_MATTER_BRIDGE
#include "solidified_matter_bridge.h"
#endif
#if USE_MATTER_ENERGY
#include "solidified_matter_energy.h"
#endif
#if USE_MATTER_TCP
#include "solidified_matter_client.h"
#endif
#if USE_MATTER_CAMERA
#include "solidified_matter_camera.h"
#endif
