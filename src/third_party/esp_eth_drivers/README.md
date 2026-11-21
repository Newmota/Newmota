# esp-eth-drivers

Ethernet PHY and SPI MAC drivers from Espressif's
[esp-eth-drivers](https://github.com/espressif/esp-eth-drivers), taken from the component
registry unchanged (`include/` and `src/` only). ESP-IDF 5.5 carries the same drivers in
`esp_eth`; from ESP-IDF 6.0 they moved to the registry, so only IDF 6.1 builds compile these.

| Directory | Component | Version | License |
|---|---|---|---|
| `ip101` | espressif/ip101 | 1.1.0 | Apache-2.0 |
| `lan87xx` | espressif/lan87xx | 1.1.0 | Apache-2.0 |
| `rtl8201` | espressif/rtl8201 | 1.1.0 | Apache-2.0 |
| `dp83848` | espressif/dp83848 | 1.1.0 | Apache-2.0 |
| `ksz80xx` | espressif/ksz80xx | 1.1.0 | Apache-2.0 |
| `w5500` | espressif/w5500 | 2.0.1 | Apache-2.0 |
| `wiznet_common` | espressif/wiznet_common (for W5500) | 1.1.0 | Apache-2.0 |
| `dm9051` | espressif/dm9051 | 1.1.1 | Apache-2.0 |
| `ksz8851snl` | espressif/ksz8851snl | 1.2.0 | MIT |

`LICENSE` is the Apache-2.0 text; the KSZ8851SNL sources carry their MIT notice.
