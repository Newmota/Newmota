/*
  image_descriptor.cpp - Newmota compatibility metadata in a native ESP-IDF image

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file image_descriptor.cpp
 * @brief Newmota compatibility metadata in a native ESP-IDF image.
 */

#include "image_descriptor.h"

using newmota::platform::esp_idf::NewmotaEspImageDescriptor;

#if defined(NEWMOTA_IMAGE_SAFEBOOT)
constexpr std::uint8_t kImageRole = newmota::platform::esp_idf::kSafebootImageRole;
#else
constexpr std::uint8_t kImageRole = newmota::platform::esp_idf::kApplicationImageRole;
#endif

extern "C" {
extern const NewmotaEspImageDescriptor custom_app_desc
    __attribute__((used, section(".rodata_custom_desc"))) = {
        {'N', 'E', 'W', 'M', 'O', 'T', 'A', '\0'},
        newmota::platform::esp_idf::kCustomDescriptorSchema,
        sizeof(NewmotaEspImageDescriptor),
        kImageRole,
        {},
        NEWMOTA_IMAGE_APP0_OFFSET,
        NEWMOTA_IMAGE_SAFEBOOT_OFFSET,
        NEWMOTA_IMAGE_OTADATA_OFFSET,
        NEWMOTA_IMAGE_NVS_OFFSET,
        NEWMOTA_IMAGE_ENVIRONMENT,
        NEWMOTA_IMAGE_SDK_TARGET,
        NEWMOTA_IMAGE_LAYOUT_ID,
        NEWMOTA_IMAGE_PSRAM_BYTES,
};
}
