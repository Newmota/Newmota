# Espressif H.264 hardware encoder

This directory contains the ESP32-P4 hardware encoder part of Espressif's
`espressif/esp_h264` component, version **1.4.1**.

- Upstream: <https://components.espressif.com/components/espressif/esp_h264>
- Archive: `espressif__esp_h264-v1.4.1.zip`, the version Tasmota's CSI camera uses
- License: Apache-2.0, see [LICENSE](LICENSE)

Newmota imports `hw/`, `interface/` and `port/` unmodified, without the
version-dependent `port/src/esp_h264_alloc_less_than_5_3.c`. The software
encoder and decoder (`sw/`, about 80 MiB of prebuilt libraries), the test apps
and the component manager files are not imported.

`USE_CSI_WEBCAM` compiles only the single-stream hardware encoder: the
`esp_h264_enc_single*`, `esp_h264_enc_param*` and `esp_h264_version` interface
sources, `hw/src` without the dual encoder, the ESP32-P4 HAL and the
allocation port. The P4 silicon revision selects `hw/soc/esp32p4/hw_ver1` or
`hw_ver3`, as the component's own CMake does. Do not edit the imported files.
