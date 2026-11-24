"""User-facing words for build features: group, summary, and why dependencies exist.

`features.py` owns what a feature builds, needs, and supports. This module only owns the words
that `make config` and build errors show. `validate_feature_docs` and
`validate_dependency_reasons` keep the two in step: a feature or dependency added to
`features.py` without an entry here fails catalog validation.
"""

from __future__ import annotations

# Display order of groups in the configurator.
GROUPS = (
    "Core services",
    "Network and time",
    "Bluetooth",
    "Buses and I/O",
    "Real-time clocks",
    "Sensors",
    "Display and UI",
    "Audio",
    "Camera",
    "LED strips",
    "LED driver chips",
    "TuyaMCU",
    "GPS",
    "Energy meter drivers",
    "Energy services",
    "Power and temperature safety",
    "Berry energy API",
)

FEATURE_DOCS: dict[str, tuple[str, str]] = {
    # Core services
    "USE_BERRY": ("Core services", "Berry scripting runtime."),
    "USE_BERRY_PERSIST": ("Core services", "Berry persist module."),
    "USE_BERRY_GPIO": ("Core services", "Berry gpio module."),
    "USE_BERRY_PIXMAT": ("Core services", "Berry pixmat class for pixel matrices."),
    "USE_BERRY_IMAGE": (
        "Core services",
        "Berry img class: one image in a kept native buffer, JPEG decoding.",
    ),
    "USE_BERRY_DRIVERS": (
        "Core services",
        "Berry add_driver hooks, Driver class and add_cmd commands, as in Tasmota.",
    ),
    "USE_BERRY_ULP": (
        "Core services",
        "Berry ULP module: load and run ULP coprocessor programs, wake from deep sleep.",
    ),
    "USE_PWM": ("Core services", "PWM outputs and the light service."),
    "USE_HARDWARE_COMMANDS": ("Core services", "GPIO and template commands."),
    "USE_DEFER": ("Core services", "Defer <ms> <command>: run a command line later."),
    "USE_STATUS_LEDS": ("Core services", "Status LED behaviour."),
    "USE_SWITCHES": ("Core services", "Switch inputs with Tasmota's SwitchMode."),
    "USE_SLEEP": ("Core services", "Sleep power saving."),
    "USE_TASMOTA_STATE": ("Core services", "Tasmota's full STATE and Status fields."),
    "USE_SENSOR_SETTINGS": ("Core services", "Sensor policy settings."),
    "USE_MESSAGE_ROUTING": ("Core services", "Message router for telemetry and state."),
    "USE_APP_MESSAGE_ROUTING": ("Core services", "Application-level message routing."),
    "USE_TIME": ("Core services", "Time service."),
    "USE_TIMERS": ("Core services", "Timer schedules."),
    "USE_ESP_TASMOTA_SETTINGS_IMPORT": ("Core services", "Import settings from Tasmota on ESP."),
    "USE_DEEPSLEEP": (
        "Core services",
        "Tasmota's DeepSleepTime: sleep between TelePeriods and wake on a timer.",
    ),
    "USE_CRYPTO_HW_SHA1": (
        "Core services",
        "BearSSL SHA-1 on resumable ESP or Bouffalo hardware (device qualification pending).",
    ),
    "USE_CRYPTO_HW_SHA256": (
        "Core services",
        "BearSSL SHA-224/256 on resumable ESP or Bouffalo hardware (qualification pending).",
    ),
    "USE_CRYPTO_HW_SHA512": (
        "Core services",
        "BearSSL SHA-384/512 on resumable ESP or Bouffalo hardware (qualification pending).",
    ),
    "USE_CRYPTO_HW_AES": (
        "Core services",
        "BearSSL AES-128/192/256 on ESP, Bouffalo or AmebaD hardware (qualification pending).",
    ),
    "USE_CRYPTO_HW_P256": (
        "Core services",
        "BearSSL P-256 on the ESP ECC engine or MPI field multiplication (qualification pending).",
    ),
    "USE_WIFI_WPA3_BEARSSL": (
        "Network and time",
        "WPA3-SAE, SAE-PK (P-256) and OWE STA on BearSSL instead of mbedTLS (ESP-IDF fork, "
        "qualification pending).",
    ),
    # Network and time
    "USE_WIFI": ("Network and time", "Wi-Fi and the network stack."),
    "USE_WIFI_POWER": ("Network and time", "WifiPower transmit power control."),
    "USE_ETHERNET": (
        "Network and time",
        "Wired Ethernet: RMII PHYs on ESP32, P4 and S31, W5500, DM9051 and KSZ8851 over SPI.",
    ),
    "USE_ESP_HOSTED": (
        "Network and time",
        "Wi-Fi through an ESP-Hosted companion; Hosted, HostedOta and HostedLoad.",
    ),
    "USE_MQTT": ("Network and time", "MQTT client and commands."),
    "USE_HA_DISCOVERY": (
        "Network and time",
        "Home Assistant MQTT discovery: one retained message per entity (SetOption19 1 = off).",
    ),
    "USE_WEB": ("Network and time", "Web interface."),
    "USE_WEB_CONSOLE": ("Network and time", "Web and Berry consoles in the web interface."),
    "USE_WEB_FILESYSTEM": ("Network and time", "File browser and editor in the web interface."),
    "USE_WEB_GPIO": ("Network and time", "Module and template editors in the web interface."),
    "USE_RECOVERY_WEB": ("Network and time", "Safeboot recovery page."),
    "USE_BEKEN_UPDATE_WEB": ("Network and time", "Firmware upload page on Beken."),
    "USE_OTA_PULL": ("Network and time", "Firmware update from a URL."),
    "USE_WGET": ("Network and time", "Wget file download."),
    "USE_PING": ("Network and time", "Ping command."),
    "USE_UDP": ("Network and time", "UDP sockets."),
    "USE_NTP": ("Network and time", "Network time synchronisation. Turn off with an external RTC."),
    "USE_NTP_SERVER": ("Network and time", "Serve the device clock over NTP (UDP 123)."),
    "USE_WEB_HTTPS": ("Network and time", "HTTPS for the web interface (default on for Large)."),
    "USE_MATTER": (
        "Network and time",
        "Matter device stack (at least 4 MiB flash; 2 MiB on Beken).",
    ),
    "USE_MATTER_BRIDGE": (
        "Network and time",
        "Bridges remote Tasmota devices to Matter over MQTT.",
    ),
    "USE_MATTER_ENERGY": (
        "Network and time",
        "Electrical sensor clusters for the energy meter (on with Matter and energy).",
    ),
    "USE_MATTER_TCP": (
        "Network and time",
        "Matter over TCP for large messages, with CASE and commands started by the device.",
    ),
    "USE_MATTER_CAMERA": (
        "Network and time",
        "Matter Camera on the CSI camera: stream allocation, snapshots and WebRTC offers.",
    ),
    # Bluetooth
    "USE_BLE": ("Bluetooth", "Bluetooth LE host and the Berry BLE module."),
    "USE_BLE_SENSORS": ("Bluetooth", "MiBeacon, BTHome, CGD1 and Mi scale BLE sensors."),
    "USE_MATTER_BLE": ("Bluetooth", "Matter commissioning over BLE."),
    # Buses and I/O
    "USE_I2C": ("Buses and I/O", "I2C bus service and commands."),
    "USE_SERIAL": ("Buses and I/O", "Shared UART service for serial peripherals."),
    "USE_DEVICE_SPI": ("Buses and I/O", "SPI bus service for peripheral drivers."),
    "USE_ADC": ("Buses and I/O", "Analog input service."),
    "USE_EDGE_CAPTURE": ("Buses and I/O", "GPIO edge capture for pulse and timing drivers."),
    "USE_SDCARD": ("Buses and I/O", "SD card as a FAT volume at /sd."),
    "USE_ZERO_CROSS": ("Buses and I/O", "Mains zero-crossing capture."),
    "USE_MODBUS_RTU": ("Buses and I/O", "Modbus RTU master over a UART."),
    "USE_MCP23008": ("Buses and I/O", "MCP23008 I2C GPIO expander."),
    # Real-time clocks
    "USE_RTC_CHIPS": ("Real-time clocks", "RTC chip framework; chips below select it."),
    "USE_DS3231": ("Real-time clocks", "DS3231 RTC (I2C 0x68)."),
    "USE_DS1307": ("Real-time clocks", "DS1307 RTC (I2C 0x68)."),
    "USE_BM8563": ("Real-time clocks", "BM8563 RTC (I2C 0x51)."),
    "USE_PCF85063": ("Real-time clocks", "PCF85063 RTC (I2C 0x51)."),
    "USE_PCF85363": ("Real-time clocks", "PCF85363 RTC (I2C 0x51)."),
    "USE_RV3028": ("Real-time clocks", "RV3028 RTC."),
    "USE_RX8010": ("Real-time clocks", "RX8010 RTC (I2C 0x32)."),
    "USE_RX8025": ("Real-time clocks", "RX8025 RTC (I2C 0x32)."),
    "USE_RX8130": ("Real-time clocks", "RX8130CE RTC (I2C 0x32)."),
    "USE_RX8030": ("Real-time clocks", "Upstream spelling of RX8130CE; selects USE_RX8130."),
    # Sensors
    "USE_BH1750": ("Sensors", "BH1750 illuminance sensor (I2C)."),
    "USE_TC74": ("Sensors", "TC74 temperature sensor (I2C)."),
    "USE_BME280": ("Sensors", "BME280 temperature, pressure and humidity sensor (I2C)."),
    "USE_DHT": ("Sensors", "DHT temperature and humidity sensor."),
    # Display and UI
    "USE_DISPLAY": ("Display and UI", "Descriptor-driven display service and Berry module."),
    "USE_UNIVERSAL_TOUCH": ("Display and UI", "Descriptor-driven I2C touch input."),
    "USE_DISPLAY_QSPI": ("Display and UI", "Four-line QSPI panel routes."),
    "USE_DISPLAY_FONTS": (
        "Display and UI",
        "Berry font class with embedded 6x12 and 8x16 Latin-1 fonts.",
    ),
    "USE_DISPLAY_DRAW": ("Display and UI", "Berry draw class: lines, rectangles, circles, text."),
    "USE_DISPLAY_FONT_5X8": ("Display and UI", "Adds the 5x8 font to the Berry font class."),
    "USE_DISPLAY_FONT_12X24": ("Display and UI", "Adds the 12x24 font to the Berry font class."),
    "USE_DISPLAY_FONT_16X32": ("Display and UI", "Adds the 16x32 font to the Berry font class."),
    "USE_LVGL": ("Display and UI", "LVGL graphics library for Berry."),
    # Audio
    "USE_I2S": ("Audio", "I2S audio output and input (at least 4 MiB flash)."),
    "USE_AUDIO_MP3": ("Audio", "MP3 decoder."),
    "USE_AUDIO_AAC": ("Audio", "AAC decoder."),
    "USE_AUDIO_OPUS": ("Audio", "Opus decoder."),
    "USE_AUDIO_SAM": ("Audio", "SAM speech synthesis."),
    "USE_BERRY_AUDIO": (
        "Audio",
        "Berry audio module: sessions, level meter, spectrum analyser, tone generator.",
    ),
    "USE_WEB_AUDIO": ("Audio", "Audio player card on the web Home page."),
    # Camera
    "USE_WEBCAM": (
        "Camera",
        "DVP camera with Berry sensor drivers, JPEG snapshot and MJPEG stream.",
    ),
    "USE_CSI_WEBCAM": (
        "Camera",
        "MIPI-CSI camera with ISP: MJPEG, H.264 over RTSP and WebSocket, WebRTC.",
    ),
    "USE_CSI_STILL": (
        "Camera",
        "CSI still session: full-sensor JPEG stills with a software ISP.",
    ),
    # LED strips
    "USE_WS2812": ("LED strips", "WS2812 addressable LED strips."),
    # LED driver chips
    "USE_LED_DRIVER": (
        "LED driver chips",
        "Two-wire LED driver chips as a five-channel light; chips below select it.",
    ),
    "USE_BP5758D": ("LED driver chips", "BP5758D RGBCW bulbs."),
    # TuyaMCU
    "USE_TUYA_MCU": ("TuyaMCU", "Tuya devices whose MCU drives relays over a serial link."),
    "USE_TUYA_MCU_SENSORS": ("TuyaMCU", "TuyaSNS: the MCU's sensor and set point data points."),
    "USE_TUYA_MCU_ENERGY": ("TuyaMCU", "The MCU's power, voltage, current and total as the meter."),
    "USE_BP1658CJ": ("LED driver chips", "BP1658CJ RGBCW bulbs."),
    "USE_SM2335": ("LED driver chips", "SM2335 RGBCW bulbs; also SM2235 and SM2185N."),
    "USE_SM2135": ("LED driver chips", "SM2135 RGBCW bulbs with Tasmota's ten presets."),
    # GPS
    "USE_GPS": ("GPS", "UBX GPS receiver: time and navigation."),
    # Energy meter drivers
    "USE_ENERGY": ("Energy meter drivers", "Energy service core; drivers below select it."),
    "USE_ENERGY_DUMMY": ("Energy meter drivers", "Simulated meter for testing."),
    "USE_ENERGY_ANALOG": ("Energy meter drivers", "Analog voltage and current inputs."),
    "USE_BL09XX": ("Energy meter drivers", "BL0939, BL0940 and BL0942 together."),
    "USE_BL0939": ("Energy meter drivers", "BL0939 two-channel meter."),
    "USE_BL0940": ("Energy meter drivers", "BL0940 meter."),
    "USE_BL0942": ("Energy meter drivers", "BL0942 meter."),
    "USE_BL0906": ("Energy meter drivers", "BL0906 six-channel meter."),
    "USE_BL0937": ("Energy meter drivers", "BL0937 pulse meter."),
    "USE_HLW8012": ("Energy meter drivers", "HLW8012 pulse meter."),
    "USE_CSE7766": ("Energy meter drivers", "CSE7759, CSE7766 and HLW8032 meters."),
    "USE_CSE7761": ("Energy meter drivers", "CSE7761 dual-channel meter."),
    "USE_CSE7761_POWCT": ("Energy meter drivers", "CSE7761 meter, PowCT variant."),
    "USE_V9240": ("Energy meter drivers", "V9240 meter."),
    "USE_MCP39F501": ("Energy meter drivers", "MCP39F501 meter."),
    "USE_ADE7880": ("Energy meter drivers", "ADE7880 three-phase meter (I2C)."),
    "USE_ADE7953_EM": ("Energy meter drivers", "ADE7953 energy-meter variant (I2C)."),
    "USE_ADE7953_PLUS2PM": ("Energy meter drivers", "ADE7953 Plus 2PM (I2C)."),
    "USE_ADE7953_PRO1PM": ("Energy meter drivers", "ADE7953 Pro 1PM (SPI)."),
    "USE_ADE7953_PRO2PM": ("Energy meter drivers", "ADE7953 Pro 2PM (SPI)."),
    "USE_ADE7953_PRO4PM": ("Energy meter drivers", "ADE7953 Pro 4PM (SPI)."),
    "USE_PZEM004T": ("Energy meter drivers", "PZEM004T meter."),
    "USE_PZEM_AC": ("Energy meter drivers", "PZEM AC meter (Modbus)."),
    "USE_PZEM_DC": ("Energy meter drivers", "PZEM DC meter (Modbus)."),
    "USE_SDM120": ("Energy meter drivers", "Eastron SDM120 (Modbus)."),
    "USE_SDM230": ("Energy meter drivers", "Eastron SDM230 (Modbus)."),
    "USE_SDM72": ("Energy meter drivers", "Eastron SDM72 (Modbus)."),
    "USE_SDM630": ("Energy meter drivers", "Eastron SDM630 (Modbus)."),
    "USE_DDS2382": ("Energy meter drivers", "DDS2382 meter (Modbus)."),
    "USE_DDSU666": ("Energy meter drivers", "DDSU666 meter (Modbus)."),
    "USE_FIF_LE01MR": ("Energy meter drivers", "FIF LE-01MR meter (Modbus)."),
    "USE_IEM3000": ("Energy meter drivers", "Schneider iEM3000 meter (Modbus)."),
    "USE_ORNO_WE517": ("Energy meter drivers", "Orno WE-517 meter (Modbus)."),
    "USE_MODBUS_ENERGY": ("Energy meter drivers", "Modbus meter defined at run time."),
    "USE_MODBUS_USER_PRECISION": ("Energy meter drivers", "User-set precision for Modbus meters."),
    "USE_BERRY_ENERGY": ("Energy meter drivers", "Meter implemented by a Berry script."),
    # Energy services
    "USE_ENERGY_CALENDAR": ("Energy services", "Dated energy buckets."),
    "USE_ENERGY_LIVE_DAILY": ("Energy services", "Live signed daily energy projection."),
    "USE_ENERGY_TARIFF": ("Energy services", "Energy tariffs."),
    "USE_ENERGY_STATUS9": ("Energy services", "Energy protection fields in StatusPTH."),
    "USE_ENERGY_PRECISION": (
        "Energy services",
        "Retained EnergyRes, WattRes and related settings.",
    ),
    "USE_ENERGY_MARGINS": ("Energy services", "Per-phase low and high margins."),
    "USE_ENERGY_POWER_DELTA": ("Energy services", "Per-phase PowerDelta reporting over MQTT."),
    "USE_ENERGY_POWER_POLICY": ("Energy services", "Calibration-driven power source policy."),
    "USE_ENERGY_COUNTER_FILTER": (
        "Energy services",
        "Hardware-total counter filter (SetOption39).",
    ),
    "USE_ENERGY_HARDWARE_TOTAL": ("Energy services", "Hardware-total reference (SetOption72)."),
    "USE_ENERGY_CALIBRATION_LEARNING": ("Energy services", "Learned calibration coefficients."),
    "USE_ENERGY_EXPORT_COMPAT": ("Energy services", "Tasmota-compatible export reporting."),
    "USE_ENERGY_TOTAL_COMPAT": ("Energy services", "Tasmota-compatible Total commands."),
    "USE_ENERGY_DAILY_COMPAT": ("Energy services", "Tasmota-compatible Today and Yesterday."),
    "USE_ENERGY_PERIOD_COMPAT": ("Energy services", "Tasmota-compatible interval energy."),
    "USE_ENERGY_TARIFF_COMPAT": ("Energy services", "Tasmota-compatible tariff projections."),
    "USE_ENERGY_BALANCED_DAILY": ("Energy services", "Balanced daily import and export per meter."),
    # Power and temperature safety
    "USE_POWER_FORCE_OFF": ("Power and temperature safety", "Forced-off output primitive."),
    "USE_POWER_TARGETS": ("Power and temperature safety", "Force logical output masks explicitly."),
    "USE_POWER_PROVENANCE": (
        "Power and temperature safety",
        "Record the origin of power operations.",
    ),
    "USE_STATE_TEXT": ("Power and temperature safety", "Durable state-word aliases (ON/OFF text)."),
    "USE_GLOBAL_TEMPERATURE": ("Power and temperature safety", "Shared temperature observations."),
    "USE_GLOBAL_TEMPERATURE_COMMANDS": (
        "Power and temperature safety",
        "Manual temperature input and selection.",
    ),
    "USE_ENERGY_PROTECTION_BINDINGS": (
        "Power and temperature safety",
        "Route energy protection through source Power endpoints.",
    ),
    "USE_ENERGY_PROTECTION_POLICY": (
        "Power and temperature safety",
        "Shared energy protection policy.",
    ),
    "USE_ENERGY_MAX_ENERGY": ("Power and temperature safety", "Daily energy threshold protection."),
    "USE_ENERGY_MAX_POWER": ("Power and temperature safety", "Maximum power protection."),
    "USE_ENERGY_OVERTEMP": (
        "Power and temperature safety",
        "Over-temperature protection for meters.",
    ),
    # Berry energy API
    "USE_BERRY_ENERGY_API": ("Berry energy API", "Berry energy module."),
    "USE_BERRY_ENERGY_ACCOUNTING_API": ("Berry energy API", "Berry daily accounting functions."),
    "USE_BERRY_ENERGY_BALANCED_API": ("Berry energy API", "Berry balanced-daily functions."),
    "USE_BERRY_ENERGY_PERIOD_API": ("Berry energy API", "Berry interval-energy functions."),
}


# Why a feature needs another. Keyed by the needed feature; a (feature, needed) key overrides
# that sentence for one pair. `make validate-catalog` reports a dependency without a reason.
DEPENDENCY_REASONS: dict[str | tuple[str, str], str] = {
    "USE_ADC": "reads its values through the analog input service",
    "USE_BERRY": "ships a Berry module",
    ("USE_MATTER", "USE_BERRY"): "the stack above authentication is written in Berry",
    ("USE_BERRY_PERSIST", "USE_BERRY"): "a module of the Berry runtime",
    ("USE_DISPLAY_FONTS", "USE_DISPLAY"): "fonts are drawn for a display",
    ("USE_DISPLAY_FONTS", "USE_BERRY_PIXMAT"): "text is drawn into a pixmat",
    ("USE_DISPLAY_DRAW", "USE_DISPLAY"): "shapes are drawn for a display",
    ("USE_DISPLAY_DRAW", "USE_BERRY_PIXMAT"): "shapes are drawn into a pixmat",
    ("USE_DISPLAY_FONT_5X8", "USE_DISPLAY_FONTS"): "adds a size to the font class",
    ("USE_DISPLAY_FONT_12X24", "USE_DISPLAY_FONTS"): "adds a size to the font class",
    ("USE_DISPLAY_FONT_16X32", "USE_DISPLAY_FONTS"): "adds a size to the font class",
    "USE_BERRY_ENERGY_API": "extends the Berry energy module",
    "USE_BL0939": "selects every BL09xx chip",
    "USE_BL0940": "selects every BL09xx chip",
    "USE_BL0942": "selects every BL09xx chip",
    "USE_BLE": "communicates over Bluetooth LE",
    "USE_DEVICE_SPI": "the chip is on SPI",
    "USE_DISPLAY": "extends the display service",
    "USE_EDGE_CAPTURE": "reads its signal through GPIO edge capture",
    "USE_ENERGY": "feeds or extends the shared energy service",
    "USE_ENERGY_BALANCED_DAILY": "exposes balanced-daily values",
    "USE_ENERGY_CALENDAR": "keeps its values in the energy calendar",
    "USE_ENERGY_CALIBRATION_LEARNING": "the driver learns its calibration",
    "USE_ENERGY_COUNTER_FILTER": "filters the hardware counter it reads",
    "USE_ENERGY_DAILY_COMPAT": "builds on the Tasmota daily commands",
    "USE_ENERGY_EXPORT_COMPAT": "builds on the Tasmota export commands",
    "USE_ENERGY_LIVE_DAILY": "needs the live daily totals",
    "USE_ENERGY_PERIOD_COMPAT": "exposes interval energy",
    "USE_ENERGY_POWER_DELTA": "extends the power change report",
    "USE_ENERGY_POWER_POLICY": "the driver follows the relay's Power state",
    "USE_ENERGY_PRECISION": "uses the configurable output precision",
    "USE_ENERGY_PROTECTION_BINDINGS": "acts through the source Power output routing",
    "USE_ENERGY_PROTECTION_POLICY": "extends the protection policy",
    "USE_ENERGY_TARIFF": "extends the tariff service",
    "USE_ENERGY_TOTAL_COMPAT": "builds on the Tasmota total commands",
    "USE_GLOBAL_TEMPERATURE": "reads the shared temperature value",
    "USE_I2C": "the chip is on I2C",
    ("USE_DISPLAY", "USE_I2C"): "displays and their touch controllers can be on I2C",
    "USE_I2S": "extends the I2S audio service",
    "USE_MATTER": "extends the Matter stack",
    "USE_MATTER_TCP": "sends large messages over Matter's TCP transport",
    "USE_CSI_WEBCAM": "streams from the CSI camera",
    ("USE_MATTER_CAMERA", "USE_AUDIO_OPUS"): "Matter cameras send Opus, not G.711",
    "USE_MESSAGE_ROUTING": "publishes through the message router",
    ("USE_APP_MESSAGE_ROUTING", "USE_MESSAGE_ROUTING"): "the application side of the router",
    "USE_APP_MESSAGE_ROUTING": "publishes through the application's message router",
    "USE_MODBUS_ENERGY": "extends the generic Modbus meter",
    "USE_MODBUS_RTU": "talks to its meter over Modbus RTU",
    "USE_MQTT": "reports over MQTT",
    "USE_POWER_FORCE_OFF": "switches relays off through the forced-off path",
    "USE_POWER_TARGETS": "addresses relays through the power targets",
    ("USE_DISPLAY", "USE_PWM"): "dims the backlight with PWM",
    ("USE_WS2812", "USE_PWM"): "the light service in USE_PWM drives the strip",
    ("USE_LED_DRIVER", "USE_PWM"): "the light service in USE_PWM drives the chip",
    "USE_LED_DRIVER": "is sent over the shared LED driver bus",
    "USE_RTC_CHIPS": "uses the shared RTC owner",
    "USE_RX8130": "another name for the same chip",
    "USE_SERIAL": "talks to its device over a UART",
    "USE_TIME": "needs the clock",
    "USE_TUYA_MCU": "reads the data points the TuyaMCU driver receives",
    "USE_UDP": "NTP runs over UDP",
    "USE_WEB": "extends the web server",
    ("USE_WEB_GPIO", "USE_HARDWARE_COMMANDS"): "saves pins through the GPIO and template commands",
    "USE_WIFI": "needs the network",
    ("USE_WIFI_POWER", "USE_WIFI"): "sets the Wi-Fi radio's transmit power",
    ("USE_ESP_HOSTED", "USE_WIFI"): "carries the Wi-Fi stack to the companion",
    ("USE_ETHERNET", "USE_WIFI"): "shares the network stack and its services with Wi-Fi",
}

# Why a role feature that is off by default cannot be turned on.
UNAVAILABLE_REASONS = {
    "USE_WIFI": "the chip has no Wi-Fi radio",
    "USE_WIFI_POWER": "only ESP-IDF has the native adapter and chip limits",
    "USE_ESP_HOSTED": "needs an ESP32-P4 board with a Wi-Fi companion",
    "USE_OTA_PULL": "needs a paired Safeboot image or Beken's native update writer",
    "USE_RECOVERY_WEB": "the recovery page belongs to Safeboot images",
    "USE_BEKEN_UPDATE_WEB": "writes Beken's native update format",
    "USE_ESP_TASMOTA_SETTINGS_IMPORT": "reads the settings format of Tasmota on ESP",
}


def dependency_reason(name: str, needed: str) -> str:
    """Return why `name` needs `needed`, or '' when no reason is recorded."""

    return DEPENDENCY_REASONS.get((name, needed), DEPENDENCY_REASONS.get(needed, ""))


def unavailable_reason(name: str, role: str) -> str:
    """Return why a role feature that is off here cannot be turned on."""

    if name in UNAVAILABLE_REASONS:
        return UNAVAILABLE_REASONS[name]
    if role == "safeboot":
        return "Safeboot images hold only recovery code"
    return "the chip has no Wi-Fi radio"


def validate_dependency_reasons(dependencies: dict[str, tuple[str, ...]]) -> list[str]:
    """Return every dependency without a reason, and every reason nothing uses."""

    pairs = {(name, needed) for name, needs in dependencies.items() for needed in needs}
    used = {needed for _, needed in pairs} | pairs
    problems = [
        f"feature_docs.py has no reason why {name} needs {needed}"
        for name, needed in sorted(pairs)
        if not dependency_reason(name, needed)
    ]
    problems += [
        f"feature_docs.py gives a reason for unknown dependency {key}"
        for key in DEPENDENCY_REASONS
        if key not in used
    ]
    return problems


def validate_feature_docs(selectable: set[str]) -> list[str]:
    """Return problems between the documented names and the selectable feature names."""

    problems = [
        f"feature {name} has no entry in feature_docs.py"
        for name in sorted(selectable - set(FEATURE_DOCS))
    ]
    problems += [
        f"feature_docs.py documents unknown feature {name}"
        for name in sorted(set(FEATURE_DOCS) - selectable)
    ]
    problems += [
        f"feature_docs.py: {name} uses unknown group '{group}'"
        for name, (group, _) in sorted(FEATURE_DOCS.items())
        if group not in GROUPS
    ]
    return problems
