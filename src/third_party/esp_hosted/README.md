# esp_hosted 2.12.13

Host side of Espressif's ESP-Hosted-MCU (`espressif/esp_hosted` 2.12.13, Apache-2.0,
commit dd0176e5), the version Tasmota pins for the ESP32-P4. Only `host/`, `common/`,
`CMakeLists.txt`, `Kconfig`, `sdkconfig.rename` and `LICENSE` are kept; slave firmware,
docs and examples are dropped.

Newmota change: `port_esp_hosted_host_init.c` is removed. Its constructor started the
transport with the sdkconfig SDIO pins; Newmota applies the template `HSDIO` pins first
and calls `esp_hosted_init()` and `esp_hosted_connect_to_slave()` itself.
