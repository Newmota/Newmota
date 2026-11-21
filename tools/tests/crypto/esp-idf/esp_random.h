// Host fixture for the ESP-IDF entropy adapter; never included in native builds.
#pragma once
#include <cstddef>
void esp_fill_random(void* destination, std::size_t size);
