# Network

`NetworkService` owns the Wi-Fi station, its two SSID slots and the setup and recovery
SoftAP. `online()` is true while the station or Ethernet has an address; MQTT, NTP, wget,
Ping and the web server follow it, and the STATE `Wifi` block follows the station only.

## ESP-Hosted on the ESP32-P4

The P4 has no radio. With `USE_ESP_HOSTED`, which the `wifi-hosted` chip capability turns
on for the P4 and P4X, ESP-IDF's Wi-Fi calls become RPCs to an ESP32-C6 running Espressif's
`network_adapter` firmware over SDIO, as in Tasmota. The pins default to the ESP32-P4
Function EV Board (CLK 18, CMD 19, D0–D3 14–17, reset 54); the template's `Hosted SDIO`
and `Hosted reset` functions replace them. The device ID comes from the P4's base MAC.

| Command | Action |
| --- | --- |
| `Hosted` | `{"Hosted":{"Host":..,"Hosted":..,"MCU":..,"Update":..}}`: library and companion versions, companion chip, update state |
| `HostedOta [url\|version]` | Downloads with `wget` and loads the image. Without an argument the URL is `OtaUrl` up to its last `/`, then `/coprocessor/network_adapter_<chip>.bin`; a version such as `v2.12.13` adds `/v2.12.13`. Plain HTTP only |
| `HostedLoad [file\|version]` | Loads `/coprocessor[/version]/network_adapter_<chip>.bin` or the given file from the filesystem |

A successful update restarts the device, since the companion reboots into its new firmware.

## Ethernet

`USE_ETHERNET` starts one wired interface after Wi-Fi. Pins follow Tasmota: for RMII PHYs
`ETH MDC` and `ETH MDIO` are the management bus and `ETH power` the PHY reset; for SPI
controllers `ETH MDC` is chip select (instance 1 on SPI bus 1, instance 2 on SPI bus 2),
`ETH MDIO` the interrupt (polled when absent) and `ETH power` the reset, with the bus's
`SPI clock`, `SPI MISO` and `SPI MOSI`. The interface is named `<hostname>-eth`.

While Ethernet has an address the setup and recovery SoftAP stays off, as in Tasmota. It
comes back when the wired link drops; a credential test started from the AP keeps it open
until the test ends.

| `EthType` | Chip | Interface |
| --- | --- | --- |
| 0 | LAN8720 | RMII |
| 1 | TLK110/IP101 | RMII |
| 2 | RTL8201 | RMII |
| 3 | DP83848 | RMII |
| 4 | DM9051 | SPI |
| 5 | KSZ8081 | RMII |
| 6 | KSZ8041 | RMII |
| 7 | JL1101 (generic 802.3 PHY) | RMII |
| 8 | W5500 | SPI |
| 9 | KSZ8851 | SPI |

RMII needs the chip's own MAC: ESP32, P4 and S31. `EthClockMode` selects the classic ESP32's
reference clock (0 GPIO0 in, 1 GPIO0 out, 2 GPIO16 out, 3 GPIO17 inverted out); the P4 and
S31 take their board default.

| Command | Action |
| --- | --- |
| `Ethernet [0\|1]` | Turns the interface on or off and restarts |
| `EthType [0..9]`, `EthAddress [-1..31]`, `EthClockMode [0..3]` | PHY settings; `EthAddress -1` probes. Changes restart |
| `EthIPAddress`, `EthGateway`, `EthSubnetmask`, `EthDNSServer<1\|2> [a.b.c.d]` | Static IPv4, applied at once; `EthIPAddress 0.0.0.0` returns to DHCP. Configured DNS servers also replace those of a DHCP lease |

The interface counts as online only while its link is up and it has an address.
Settings live in the two slots `/.newmota/ethernet.a` and `ethernet.b`. StatusNET carries an `Ethernet` block while the
interface runs.
