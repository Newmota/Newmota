"""Resolve firmware features once for native code and build generators."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path

from newmota.feature_docs import dependency_reason, unavailable_reason
from newmota.host import write_if_changed
from newmota.model import ROOT, BuildError, Environment, esp_keeps_wpa3
from newmota.package import LayoutFacts

AUDIO_MINIMUM_FLASH_KB = 4096


@dataclass(frozen=True)
class ResolvedFeatures:
    """One immutable feature selection and its generated build inputs."""

    assignments: tuple[tuple[str, bool], ...]
    header: Path
    manifest: Path
    identity: str
    sources: tuple[str, ...]
    sdk_requirements: tuple[str, ...]
    native_settings: dict[str, str]

    def enabled(self, name: str) -> bool:
        """Return whether a named feature is enabled."""

        return dict(self.assignments).get(name, False)


# Source ownership is declared once; native adapters only translate the result.
CORE_SOURCES = (
    "app/core/app.cpp",
    "app/core/command/command.cpp",
    "app/core/command/serial_console.cpp",
    "app/core/hardware/hardware.cpp",
    "app/core/hardware/hardware_runtime.cpp",
    "app/core/log.cpp",
    "app/core/scheduler.cpp",
    "app/core/storage/filesystem.cpp",
    "app/core/storage/filesystem_commands.cpp",
    "app/core/update/update_session.cpp",
    "app/core/system/boot_counter.cpp",
    "app/core/system/boot_guard.cpp",
    "app/core/system/reset_commands.cpp",
    "app/core/system/restart.cpp",
    "app/core/system/restart_commands.cpp",
    "app/core/system/uptime.cpp",
    "app/drivers/io/basic_io.cpp",
    "app/services/power/power_commands.cpp",
    "app/services/power/power_service.cpp",
    "app/services/power/power_telemetry.cpp",
    "app/services/mqtt/mqtt_settings.cpp",
    "app/services/settings/settings.cpp",
    "app/services/settings/settings_commands.cpp",
    "app/services/status/status_commands.cpp",
    "app/support/float_math.cpp",
    "app/support/json.cpp",
    "app/support/number.cpp",
    "app/support/text.cpp",
)
FEATURE_SOURCES = {
    "USE_PWM": (
        "app/drivers/pwm/pwm_outputs.cpp",
        "app/services/light/light_color.cpp",
        "app/services/light/light_api.cpp",
        "app/services/light/light_service.cpp",
        "app/services/light/light_commands.cpp",
        "platform/pwm.cpp",
        "platform/pwm_controller.cpp",
        "platform/pwm_policy.cpp",
    ),
    "USE_MESSAGE_ROUTING": ("app/core/message/message.cpp",),
    # Loop sleep, Wi-Fi power saving and their reporting; Safeboot never sleeps.
    "USE_SLEEP": ("app/core/system/loop_sleep.cpp",),
    "USE_WIFI_POWER": ("app/services/network/wifi_tx_power.cpp",),
    "USE_ESP_HOSTED": ("app/services/network/hosted_service.cpp",),
    "USE_HARDWARE_COMMANDS": ("app/core/hardware/hardware_commands.cpp",),
    "USE_DEFER": ("app/core/command/defer.cpp",),
    # Safeboot keeps its LED pins off; the status LED policy is application-only.
    "USE_STATUS_LEDS": (
        "app/services/power/led_commands.cpp",
        "app/services/power/led_service.cpp",
    ),
    "USE_SWITCHES": ("app/services/power/power_switches.cpp",),
    "USE_WIFI": (
        "app/services/network/network_service.cpp",
        "app/services/network/network_commands.cpp",
        "app/services/network/wifi_commands.cpp",
    ),
    "USE_TIMERS": (
        "app/services/timers/timer_service.cpp",
        "app/services/timers/timer_commands.cpp",
    ),
    "USE_TIME": (
        "app/services/time/time_service.cpp",
        "app/services/time/time_commands.cpp",
        "app/services/time/time_format.cpp",
    ),
    "USE_UDP": ("platform/lwip/udp.cpp",),
    "USE_WEB": ("app/services/web/web_service.cpp",),
    "USE_WEB_CONSOLE": ("app/core/console_history.cpp",),
    "USE_WEB_FILESYSTEM": (),
    "USE_WEB_GPIO": (),
    "USE_RECOVERY_WEB": (
        "app/services/web/recovery_web_service.cpp",
        "app/services/web/update_web_service.cpp",
    ),
    "USE_BEKEN_UPDATE_WEB": ("app/services/web/update_web_service.cpp",),
    "USE_WGET": ("app/services/network/wget_service.cpp",),
    "USE_PING": (
        "app/services/network/ping_service.cpp",
        "platform/lwip/ping.cpp",
    ),
    "USE_OTA_PULL": (
        "app/services/update/url_update_service.cpp",
        "app/services/update/update_commands.cpp",
    ),
    "USE_MQTT": (
        "app/services/mqtt/mqtt_service.cpp",
        "app/services/mqtt/mqtt_commands.cpp",
        "app/services/mqtt/mqtt_client.cpp",
        "app/services/mqtt/mqtt_codec.cpp",
        "app/services/mqtt/mqtt_wire.cpp",
        "third_party/coremqtt/v2.3.1/core_mqtt_serializer.c",
    ),
    "USE_BERRY": (
        "app/services/berry/autoexec.cpp",
        "app/services/berry/berry_service.cpp",
        "app/services/berry/berry_port.cpp",
        "app/services/berry/berry_module_table.c",
        "app/services/berry/modules/newmota/newmota_module.cpp",
        "app/services/berry/modules/path/path_module.cpp",
        "app/services/berry/modules/persist/persist_module.cpp",
        "app/services/light/berry/light_module.cpp",
    ),
    "USE_BERRY_GPIO": ("app/services/berry/modules/gpio/gpio_module.cpp",),
    "USE_BERRY_PIXMAT": ("app/services/berry/modules/pixmat/pixmat_module.cpp",),
    "USE_ESP_TASMOTA_SETTINGS_IMPORT": (
        "app/services/migration/tasmota_settings.cpp",
        "app/services/migration/tasmota_settings_import.cpp",
    ),
}
# Role features default on or off per role and platform. A build may turn any of them off;
# turning one on is refused only where UNAVAILABLE_REASONS in feature_docs.py gives a reason.
CORE_FEATURES = (
    "USE_PWM",
    "USE_BERRY",
    "USE_BERRY_PERSIST",
    "USE_BERRY_GPIO",
    "USE_BERRY_PIXMAT",
    "USE_HARDWARE_COMMANDS",
    "USE_DEFER",
    "USE_STATUS_LEDS",
    "USE_SWITCHES",
    "USE_SLEEP",
    "USE_WIFI_POWER",
    "USE_TASMOTA_STATE",
    "USE_SENSOR_SETTINGS",
    "USE_MESSAGE_ROUTING",
    "USE_APP_MESSAGE_ROUTING",
    "USE_WIFI",
    "USE_ESP_HOSTED",
    "USE_WGET",
    "USE_PING",
    "USE_OTA_PULL",
    "USE_MQTT",
    "USE_WEB",
    "USE_WEB_CONSOLE",
    "USE_WEB_FILESYSTEM",
    "USE_WEB_GPIO",
    "USE_RECOVERY_WEB",
    "USE_BEKEN_UPDATE_WEB",
    "USE_TIME",
    "USE_TIMERS",
    "USE_NTP",
    "USE_UDP",
    "USE_ESP_TASMOTA_SETTINGS_IMPORT",
)
# Every dependency needs a reason in DEPENDENCY_REASONS in feature_docs.py.
FEATURE_DEPENDENCIES = {
    "USE_BERRY_PERSIST": ("USE_BERRY",),
    "USE_BERRY_GPIO": ("USE_BERRY",),
    "USE_BERRY_PIXMAT": ("USE_BERRY",),
    "USE_APP_MESSAGE_ROUTING": ("USE_MESSAGE_ROUTING",),
    "USE_WIFI_POWER": ("USE_WIFI",),
    "USE_ESP_HOSTED": ("USE_WIFI",),
    "USE_TIMERS": ("USE_TIME",),
    "USE_NTP": ("USE_TIME", "USE_WIFI", "USE_UDP"),
    "USE_UDP": ("USE_WIFI",),
    "USE_OTA_PULL": ("USE_WIFI",),
    "USE_WGET": ("USE_WIFI",),
    "USE_PING": ("USE_WIFI", "USE_APP_MESSAGE_ROUTING"),
    "USE_WEB": ("USE_WIFI", "USE_APP_MESSAGE_ROUTING"),
    "USE_WEB_CONSOLE": ("USE_WEB",),
    "USE_WEB_FILESYSTEM": ("USE_WEB",),
    "USE_WEB_GPIO": ("USE_WEB", "USE_HARDWARE_COMMANDS"),
    "USE_RECOVERY_WEB": ("USE_WIFI",),
    "USE_BEKEN_UPDATE_WEB": ("USE_WEB",),
    "USE_MQTT": ("USE_MESSAGE_ROUTING", "USE_WIFI"),
    "USE_ESP_TASMOTA_SETTINGS_IMPORT": (),
}


@dataclass(frozen=True)
class SensorSpec:
    """Stable runtime identity and typed telemetry composition for one driver."""

    driver_id: int  # Assigned once, 1..128; never reuse an ID for another driver.
    symbol: str
    name: str
    output_field: str
    output_fields: tuple[str, ...] = ()
    output_routes: str = "StatusSNS, MQTT SENSOR, GET /api/sensors, GET /api/sensors/readings"
    remote: bool = False  # Measures elsewhere (radio); excluded from the global temperature.


@dataclass(frozen=True)
class EnergySpec:
    """Permanent Newmota energy identity, independent of upstream XNRG numbers."""

    driver_id: int
    symbol: str
    name: str
    maximum_channels: int
    default_flags: int = 0
    default_invalid_power_frames: int = 128


@dataclass(frozen=True)
class FeatureSetting:
    """One whole-number build setting written under a feature's USE_* key."""

    name: str
    macro: str
    minimum: int
    maximum: int
    excluded: tuple[int, ...] = ()
    personal: bool = False  # Only in user_config/, which Git ignores.


@dataclass(frozen=True)
class FeatureSpec:
    """An optional module with one selection, source, and support contract."""

    sources: tuple[str, ...]
    dependencies: tuple[str, ...] = ()
    profiles: tuple[str, ...] = ()
    roles: tuple[str, ...] = ("application",)
    platforms: tuple[str, ...] = ("esp", "beken", "bouffalo", "amebad", "pico")
    sensor: SensorSpec | None = None
    energy: EnergySpec | None = None
    crypto: tuple[str, ...] = ()  # C crypto primitives this module calls natively.
    settings: tuple[FeatureSetting, ...] = ()


MATTER_MINIMUM_FLASH_KB = 4096
# ESP targets whose vector units esp-dsp's assembly kernels drive for Newmota.
ESP_DSP_TARGETS = ("esp32s3", "esp32p4")
# Cortex-M33 SDK targets whose spectrum FFT runs on CMSIS-DSP.
CMSIS_DSP_TARGETS = ("rp2350-arm-s", "rtl8721d")
# CMSIS-DSP's float complex FFT with its bit reversal and constant tables.
CMSIS_DSP_SOURCES = (
    "CommonTables/arm_common_tables.c",
    "CommonTables/arm_const_structs.c",
    "TransformFunctions/arm_bitreversal2.c",
    "TransformFunctions/arm_cfft_f32.c",
    "TransformFunctions/arm_cfft_radix8_f32.c",
)
# Families whose display SPI and SD card share platform/<family>/spi_controller.cpp.
SPI_CONTROLLER_PLATFORMS = ("pico", "amebad", "bouffalo")
# Bouffalo targets whose SDH controller carries SDIO cards (sd_card_sdh.cpp).
SDH_TARGETS = ("bl616", "bl618dg")
# Beken has no Safeboot and keeps LittleFS outside its update area, so Matter fits 2 MiB there;
# the image checks against the application slot and update area decide each build.
MATTER_SMALL_FLASH_PLATFORMS = ("beken",)
# Drivers overriding Device::configure_registers, write_registers or the profile hooks.
ENERGY_REGISTER_DRIVERS = ("USE_ENERGY_DUMMY", "USE_ADE7880", "USE_ENERGY_ANALOG")
# Features reaching Device::inject: the virtual meters and the Berry energy API.
ENERGY_INJECT_FEATURES = ("USE_ENERGY_DUMMY", "USE_BERRY_ENERGY_API")

# Keep niche modules off in every existing profile. Extend this registry when a
# module is added; native adapters translate the resolved requirements only.
OPTIONAL_FEATURES = {
    # Plan 44: Home Assistant MQTT discovery, one retained message per entity.
    "USE_HA_DISCOVERY": FeatureSpec(
        ("app/services/mqtt/ha_discovery.cpp",), dependencies=("USE_MQTT",)
    ),
    "USE_CRYPTO_HW_P256": FeatureSpec(
        (), roles=("application", "safeboot"), platforms=("esp",), crypto=("EC",)
    ),
    # WPA3-SAE on BearSSL through the ESP-IDF fork's CONFIG_ESP_WIFI_EXTERNAL_CRYPTO, with the
    # supplicant's MD5, SHA-1, SHA-256 and AES on BearSSL too; SAE-PK (P-256 keys) and OWE STA
    # use the backend's EC key and ECDH functions. On Beken the same backend replaces the BDK's
    # crypto_mbedtls.c under the prebuilt supplicant (plus HMAC, PBKDF2 and SHA-384/512), and on
    # Bouffalo BL616/BL618DG the fork's CONFIG_WIFI6_EXTERNAL_CRYPTO adapter. BL602 moves only the
    # supplicant's MD5/SHA-1/SHA-256 (CONFIG_WIFI4_EXTERNAL_HASH); SAE stays on blcrypto_suite.
    "USE_WIFI_WPA3_BEARSSL": FeatureSpec(
        ("platform/crypto/hostap_sae_crypto.c", "platform/crypto/hostap_symmetric_bearssl.c"),
        dependencies=("USE_WIFI",),
        roles=("application", "safeboot"),
        platforms=("esp", "beken", "bouffalo"),
        crypto=("EC", "ECDSA_VERIFY", "MD5", "SHA1", "SHA224_SHA256", "AES"),
    ),
    "USE_CRYPTO_HW_AES": FeatureSpec((), platforms=("esp", "bouffalo", "amebad"), crypto=("AES",)),
    "USE_CRYPTO_HW_SHA1": FeatureSpec(
        (), roles=("application", "safeboot"), platforms=("esp", "bouffalo"), crypto=("SHA1",)
    ),
    "USE_CRYPTO_HW_SHA256": FeatureSpec(
        (),
        roles=("application", "safeboot"),
        platforms=("esp", "bouffalo"),
        crypto=("SHA224_SHA256",),
    ),
    "USE_CRYPTO_HW_SHA512": FeatureSpec(
        (), platforms=("esp", "bouffalo"), crypto=("SHA384_SHA512",)
    ),
    "USE_NTP_SERVER": FeatureSpec(
        ("platform/ntp_packet.cpp", "platform/lwip/ntp_server.cpp"),
        dependencies=("USE_TIME", "USE_WIFI"),
    ),
    "USE_ENERGY_LIVE_DAILY": FeatureSpec(
        ("app/services/energy/energy_live_daily.cpp",), dependencies=("USE_ENERGY_CALENDAR",)
    ),
    "USE_ENERGY_PROTECTION_POLICY": FeatureSpec(
        ("app/services/energy/energy_protection_policy.cpp",),
        dependencies=("USE_ENERGY_PROTECTION_BINDINGS", "USE_TIME"),
    ),
    "USE_ENERGY_MAX_ENERGY": FeatureSpec(
        ("app/services/energy/energy_max_energy.cpp",),
        dependencies=("USE_ENERGY_PROTECTION_POLICY", "USE_ENERGY_LIVE_DAILY"),
    ),
    "USE_ENERGY_MAX_POWER": FeatureSpec(
        ("app/services/energy/energy_max_power.cpp",),
        dependencies=("USE_ENERGY_PROTECTION_POLICY",),
    ),
    "USE_ENERGY_PROTECTION_BINDINGS": FeatureSpec(
        ("app/services/energy/energy_protection.cpp",),
        dependencies=("USE_ENERGY", "USE_POWER_TARGETS"),
    ),
    "USE_POWER_TARGETS": FeatureSpec(
        ("app/services/power/power_targets.cpp",), dependencies=("USE_POWER_FORCE_OFF",)
    ),
    "USE_ENERGY_STATUS9": FeatureSpec(
        ("app/services/energy/energy_status.cpp",), dependencies=("USE_ENERGY",)
    ),
    "USE_STATE_TEXT": FeatureSpec(("app/services/settings/state_text.cpp",)),
    "USE_GLOBAL_TEMPERATURE_COMMANDS": FeatureSpec(
        (
            "app/services/temperature/global_temperature_commands.cpp",
            "app/services/temperature/global_temperature_selection.cpp",
        ),
        dependencies=("USE_GLOBAL_TEMPERATURE",),
    ),
    "USE_POWER_PROVENANCE": FeatureSpec((), dependencies=("USE_POWER_FORCE_OFF",)),
    "USE_GLOBAL_TEMPERATURE": FeatureSpec(("app/services/temperature/global_temperature.cpp",)),
    "USE_POWER_FORCE_OFF": FeatureSpec(("app/services/power/power_safety.cpp",)),
    "USE_ENERGY_OVERTEMP": FeatureSpec(
        ("app/services/energy/energy_overtemperature.cpp",),
        dependencies=("USE_ENERGY", "USE_GLOBAL_TEMPERATURE", "USE_POWER_FORCE_OFF"),
    ),
    "USE_ENERGY_COUNTER_FILTER": FeatureSpec(
        ("app/services/energy/energy_counter_filter.cpp",), dependencies=("USE_ENERGY",)
    ),
    "USE_ENERGY_HARDWARE_TOTAL": FeatureSpec(
        ("app/services/energy/energy_hardware_total.cpp",),
        dependencies=("USE_ENERGY_TOTAL_COMPAT", "USE_ENERGY_COUNTER_FILTER"),
    ),
    "USE_ENERGY_CALIBRATION_LEARNING": FeatureSpec(
        ("app/services/energy/energy_calibration_learning.cpp",), dependencies=("USE_ENERGY",)
    ),
    "USE_ENERGY_POWER_POLICY": FeatureSpec(
        ("app/services/energy/energy_power_policy.cpp",), dependencies=("USE_ENERGY",)
    ),
    "USE_ENERGY_MARGINS": FeatureSpec(
        ("app/services/energy/energy_margins.cpp",), dependencies=("USE_ENERGY_POWER_DELTA",)
    ),
    "USE_ENERGY_POWER_DELTA": FeatureSpec(
        ("app/services/energy/energy_power_delta.cpp",), dependencies=("USE_ENERGY", "USE_MQTT")
    ),
    "USE_ENERGY_PRECISION": FeatureSpec(
        ("app/services/energy/energy_precision.cpp",), dependencies=("USE_ENERGY",)
    ),
    "USE_MODBUS_USER_PRECISION": FeatureSpec(
        (), dependencies=("USE_MODBUS_ENERGY", "USE_ENERGY_PRECISION")
    ),
    "USE_ENERGY_BALANCED_DAILY": FeatureSpec(
        ("app/services/energy/energy_balanced_daily.cpp",), dependencies=("USE_ENERGY",)
    ),
    "USE_ENERGY_PERIOD_COMPAT": FeatureSpec(
        ("app/services/energy/energy_period.cpp",), dependencies=("USE_ENERGY", "USE_MQTT")
    ),
    "USE_ENERGY_TARIFF_COMPAT": FeatureSpec(
        ("app/services/energy/energy_tariff_compat.cpp",),
        dependencies=("USE_ENERGY_TARIFF", "USE_ENERGY"),
    ),
    "USE_ENERGY_EXPORT_COMPAT": FeatureSpec(
        ("app/services/energy/energy_export_commands.cpp",),
        dependencies=("USE_ENERGY_TOTAL_COMPAT",),
    ),
    "USE_ENERGY_TOTAL_COMPAT": FeatureSpec(
        ("app/services/energy/energy_total_commands.cpp",),
        dependencies=("USE_ENERGY_DAILY_COMPAT",),
    ),
    "USE_ENERGY_DAILY_COMPAT": FeatureSpec(
        ("app/services/energy/energy_daily_commands.cpp",), dependencies=("USE_ENERGY_LIVE_DAILY",)
    ),
    "USE_ENERGY_TARIFF": FeatureSpec(
        ("app/services/energy/energy_tariff.cpp",), dependencies=("USE_ENERGY_CALENDAR",)
    ),
    "USE_ENERGY_CALENDAR": FeatureSpec(
        ("app/services/energy/energy_calendar.cpp",), dependencies=("USE_ENERGY", "USE_TIME")
    ),
    "USE_ENERGY_ANALOG": FeatureSpec(
        ("app/drivers/energy/analog_energy.cpp",),
        dependencies=("USE_ENERGY", "USE_ADC"),
        energy=EnergySpec(33, "create_analog_energy", "Analog voltage/current", 7),
    ),
    "USE_ADC": FeatureSpec(("app/drivers/adc/adc_service.cpp",)),
    "USE_BL09XX": FeatureSpec((), dependencies=("USE_BL0939", "USE_BL0940", "USE_BL0942")),
    "USE_BL0939": FeatureSpec(
        ("app/drivers/energy/bl09xx.cpp",),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY", "USE_SERIAL"),
        energy=EnergySpec(14, "create_bl0939", "BL0939", 2),
    ),
    "USE_BL0940": FeatureSpec(
        ("app/drivers/energy/bl09xx.cpp",),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY", "USE_SERIAL"),
        energy=EnergySpec(41, "create_bl0940", "BL0940", 1),
    ),
    "USE_BL0942": FeatureSpec(
        ("app/drivers/energy/bl09xx.cpp",),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY", "USE_SERIAL"),
        energy=EnergySpec(42, "create_bl0942", "BL0942", 1),
    ),
    "USE_BERRY_ENERGY_API": FeatureSpec(
        ("app/services/berry/modules/energy/energy_module.cpp",),
        dependencies=("USE_ENERGY", "USE_BERRY"),
    ),
    "USE_BERRY_ENERGY_ACCOUNTING_API": FeatureSpec(
        (), dependencies=("USE_BERRY_ENERGY_API", "USE_ENERGY_LIVE_DAILY")
    ),
    "USE_BERRY_ENERGY_BALANCED_API": FeatureSpec(
        (), dependencies=("USE_BERRY_ENERGY_API", "USE_ENERGY_BALANCED_DAILY")
    ),
    "USE_BERRY_ENERGY_PERIOD_API": FeatureSpec(
        (), dependencies=("USE_BERRY_ENERGY_API", "USE_ENERGY_PERIOD_COMPAT")
    ),
    "USE_BERRY_ENERGY": FeatureSpec(
        ("app/drivers/energy/berry_energy.cpp",),
        dependencies=("USE_BERRY_ENERGY_API",),
        energy=EnergySpec(27, "create_berry_energy", "Berry", 8, default_flags=27),
    ),
    "USE_V9240": FeatureSpec(
        ("app/drivers/energy/v9240.cpp",),
        dependencies=("USE_ENERGY", "USE_SERIAL", "USE_ENERGY_POWER_POLICY"),
        energy=EnergySpec(25, "create_v9240", "V9240", 1),
    ),
    "USE_ZERO_CROSS": FeatureSpec(
        ("app/drivers/gpio/zero_cross.cpp",), dependencies=("USE_EDGE_CAPTURE",)
    ),
    "USE_CSE7761": FeatureSpec(
        ("app/drivers/energy/cse7761.cpp",),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY", "USE_SERIAL"),
        energy=EnergySpec(19, "create_cse7761", "CSE7761 Dual R3", 2),
    ),
    "USE_CSE7761_POWCT": FeatureSpec(
        ("app/drivers/energy/cse7761.cpp",),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY", "USE_SERIAL"),
        energy=EnergySpec(40, "create_cse7761_powct", "CSE7761 PowCT", 1),
    ),
    "USE_MCP23008": FeatureSpec(
        ("app/drivers/io_expander/mcp23008.cpp", "app/drivers/i2c/i2c_device_claims.cpp"),
        dependencies=("USE_I2C",),
    ),
    "USE_BL0906": FeatureSpec(
        ("app/drivers/energy/bl0906.cpp",),
        dependencies=("USE_ENERGY", "USE_SERIAL"),
        energy=EnergySpec(23, "create_bl0906", "BL0906", 6),
    ),
    "USE_ADE7880": FeatureSpec(
        ("app/drivers/energy/ade7880.cpp", "app/drivers/i2c/i2c_device_claims.cpp"),
        dependencies=("USE_ENERGY", "USE_I2C", "USE_EDGE_CAPTURE"),
        energy=EnergySpec(22, "create_ade7880", "ADE7880", 3),
    ),
    "USE_ADE7953_EM": FeatureSpec(
        (
            "app/drivers/energy/ade7953.cpp",
            "app/core/hardware/runtime_pin_claims.cpp",
            "app/drivers/i2c/i2c_device_claims.cpp",
        ),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY", "USE_I2C"),
        energy=EnergySpec(35, "create_ade7953_em", "ADE7953 EM", 2),
    ),
    "USE_ADE7953_PLUS2PM": FeatureSpec(
        (
            "app/drivers/energy/ade7953.cpp",
            "app/core/hardware/runtime_pin_claims.cpp",
            "app/drivers/i2c/i2c_device_claims.cpp",
        ),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY", "USE_I2C"),
        energy=EnergySpec(36, "create_ade7953_plus2pm", "ADE7953 Plus 2PM", 2),
    ),
    "USE_ADE7953_PRO1PM": FeatureSpec(
        ("app/drivers/energy/ade7953.cpp",),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY", "USE_DEVICE_SPI"),
        energy=EnergySpec(37, "create_ade7953_pro1pm", "ADE7953 Pro 1PM", 1),
    ),
    "USE_ADE7953_PRO2PM": FeatureSpec(
        ("app/drivers/energy/ade7953.cpp",),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY", "USE_DEVICE_SPI"),
        energy=EnergySpec(38, "create_ade7953_pro2pm", "ADE7953 Pro 2PM", 2),
    ),
    "USE_ADE7953_PRO4PM": FeatureSpec(
        ("app/drivers/energy/ade7953.cpp",),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY", "USE_DEVICE_SPI"),
        energy=EnergySpec(39, "create_ade7953_pro4pm", "ADE7953 Pro 4PM", 4),
    ),
    "USE_DEVICE_SPI": FeatureSpec(
        ("app/drivers/spi/spi_service.cpp", "app/core/hardware/runtime_pin_claims.cpp")
    ),
    "USE_EDGE_CAPTURE": FeatureSpec(("app/drivers/gpio/edge_capture_service.cpp",)),
    "USE_SDCARD": FeatureSpec(
        (
            "app/core/storage/sd_card_mount.cpp",
            "app/core/storage/sd_volume.cpp",
            "third_party/fatfs/ff.c",
            "third_party/fatfs/ffsystem.c",
            "third_party/fatfs/ffunicode.c",
        ),
        platforms=("esp", "bouffalo", "amebad", "pico"),
    ),
    "USE_HLW8012": FeatureSpec(
        ("app/drivers/energy/hlw8012.cpp",),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY", "USE_EDGE_CAPTURE"),
        energy=EnergySpec(1, "create_hlw8012", "HLW8012", 1),
    ),
    "USE_BL0937": FeatureSpec(
        ("app/drivers/energy/hlw8012.cpp",),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY", "USE_EDGE_CAPTURE"),
        energy=EnergySpec(34, "create_bl0937", "BL0937", 1),
    ),
    "USE_ENERGY": FeatureSpec(
        (
            "app/services/energy/energy_service.cpp",
            "app/services/energy/energy_accounting.cpp",
            "app/services/energy/energy_storage.cpp",
            "app/services/energy/energy_types.cpp",
            "app/services/energy/energy_calibration.cpp",
            "app/services/energy/energy_configuration.cpp",
            "app/services/energy/energy_commands.cpp",
            "app/services/energy/energy_sensor.cpp",
            "app/services/energy/energy_output.cpp",
        ),
        dependencies=("USE_ENERGY_EXPORT_COMPAT",),
        # The meter is one more card on the sensor dashboard; StatusSNS keeps its ENERGY object.
        sensor=SensorSpec(
            6,
            "energy_sensor_driver",
            "ENERGY",
            "Voltage,Current,Power,Today,Yesterday,Total",
            output_fields=(
                "Voltage",
                "Frequency",
                "Current",
                "Power",
                "PowerTotal",
                "ApparentPower",
                "ReactivePower",
                "Factor",
                "Today",
                "Yesterday",
                "Total",
                "ExportActive",
            ),
            output_routes="GET /api/sensors/readings",
        ),
    ),
    "USE_ENERGY_DUMMY": FeatureSpec(
        ("app/drivers/energy/dummy.cpp",),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY"),
        energy=EnergySpec(30, "create_dummy", "Dummy", 8),
    ),
    "USE_CSE7766": FeatureSpec(
        ("app/drivers/energy/cse7766.cpp",),
        dependencies=(
            "USE_ENERGY",
            "USE_ENERGY_POWER_POLICY",
            "USE_ENERGY_CALIBRATION_LEARNING",
            "USE_SERIAL",
        ),
        energy=EnergySpec(2, "create_cse7766", "CSE7759/CSE7766/HLW8032", 1),
    ),
    "USE_PZEM004T": FeatureSpec(
        ("app/drivers/energy/pzem004t.cpp",),
        dependencies=("USE_ENERGY", "USE_SERIAL"),
        energy=EnergySpec(3, "create_pzem004t", "PZEM004T", 3),
    ),
    "USE_MCP39F501": FeatureSpec(
        ("app/drivers/energy/mcp39f501.cpp",),
        dependencies=("USE_ENERGY", "USE_ENERGY_POWER_POLICY", "USE_SERIAL"),
        energy=EnergySpec(4, "create_mcp39f501", "MCP39F501", 1),
    ),
    "USE_PZEM_AC": FeatureSpec(
        ("app/drivers/energy/pzem.cpp",),
        dependencies=("USE_ENERGY", "USE_MODBUS_RTU"),
        energy=EnergySpec(5, "create_pzem_ac", "PZEM AC", 3),
    ),
    "USE_PZEM_DC": FeatureSpec(
        ("app/drivers/energy/pzem.cpp",),
        dependencies=("USE_ENERGY", "USE_MODBUS_RTU"),
        energy=EnergySpec(6, "create_pzem_dc", "PZEM DC", 3),
    ),
    "USE_SDM120": FeatureSpec(
        ("app/drivers/energy/modbus_meter.cpp", "app/drivers/energy/modbus_meter_profiles.cpp"),
        dependencies=("USE_ENERGY", "USE_MODBUS_RTU"),
        energy=EnergySpec(8, "create_sdm120", "SDM120", 1),
    ),
    "USE_DDS2382": FeatureSpec(
        ("app/drivers/energy/modbus_meter.cpp", "app/drivers/energy/modbus_meter_profiles.cpp"),
        dependencies=("USE_ENERGY", "USE_MODBUS_RTU"),
        energy=EnergySpec(9, "create_dds2382", "DDS2382", 1),
    ),
    "USE_SDM630": FeatureSpec(
        ("app/drivers/energy/modbus_meter.cpp", "app/drivers/energy/modbus_meter_profiles.cpp"),
        dependencies=("USE_ENERGY", "USE_MODBUS_RTU"),
        energy=EnergySpec(10, "create_sdm630", "SDM630", 3),
    ),
    "USE_DDSU666": FeatureSpec(
        ("app/drivers/energy/modbus_meter.cpp", "app/drivers/energy/modbus_meter_profiles.cpp"),
        dependencies=("USE_ENERGY", "USE_MODBUS_RTU"),
        energy=EnergySpec(11, "create_ddsu666", "DDSU666", 1),
    ),
    "USE_FIF_LE01MR": FeatureSpec(
        ("app/drivers/energy/modbus_meter.cpp", "app/drivers/energy/modbus_meter_profiles.cpp"),
        dependencies=("USE_ENERGY", "USE_MODBUS_RTU"),
        energy=EnergySpec(13, "create_le01mr", "LE01MR", 1),
    ),
    "USE_IEM3000": FeatureSpec(
        ("app/drivers/energy/modbus_meter.cpp", "app/drivers/energy/modbus_meter_profiles.cpp"),
        dependencies=("USE_ENERGY", "USE_MODBUS_RTU"),
        energy=EnergySpec(16, "create_iem3000", "iEM3000", 3),
    ),
    "USE_ORNO_WE517": FeatureSpec(
        ("app/drivers/energy/modbus_meter.cpp", "app/drivers/energy/modbus_meter_profiles.cpp"),
        dependencies=("USE_ENERGY", "USE_MODBUS_RTU"),
        energy=EnergySpec(17, "create_we517", "WE517", 3),
    ),
    "USE_SDM72": FeatureSpec(
        ("app/drivers/energy/modbus_meter.cpp", "app/drivers/energy/modbus_meter_profiles.cpp"),
        dependencies=("USE_ENERGY", "USE_MODBUS_RTU"),
        energy=EnergySpec(18, "create_sdm72", "SDM72", 1),
    ),
    "USE_SDM230": FeatureSpec(
        ("app/drivers/energy/modbus_meter.cpp", "app/drivers/energy/modbus_meter_profiles.cpp"),
        dependencies=("USE_ENERGY", "USE_MODBUS_RTU"),
        energy=EnergySpec(20, "create_sdm230", "SDM230", 1),
    ),
    "USE_MODBUS_ENERGY": FeatureSpec(
        (
            "app/drivers/energy/modbus_meter.cpp",
            "app/drivers/energy/modbus_meter_config.cpp",
            "app/drivers/energy/configured_modbus.cpp",
        ),
        dependencies=("USE_ENERGY", "USE_MODBUS_RTU"),
        energy=EnergySpec(28, "create_configured_modbus", "Configured Modbus", 3),
    ),
    "USE_LVGL": FeatureSpec(
        ("app/services/berry/modules/lvgl/lvgl_runtime.cpp",), dependencies=("USE_BERRY",)
    ),
    "USE_DISPLAY": FeatureSpec(
        (
            "platform/display/descriptor.cpp",
            "platform/display/runtime.cpp",
            "app/drivers/display/display_commands.cpp",
            "app/drivers/display/display_service.cpp",
            "app/services/berry/modules/display/display_module.cpp",
        ),
        dependencies=("USE_BERRY", "USE_I2C", "USE_PWM"),
    ),
    "USE_DISPLAY_FONTS": FeatureSpec(
        (
            "app/services/berry/modules/font/font_module.cpp",
            "app/services/berry/modules/font/font_tamp.c",
        ),
        dependencies=("USE_DISPLAY", "USE_BERRY_PIXMAT"),
    ),
    "USE_DISPLAY_DRAW": FeatureSpec(
        ("app/services/berry/modules/display/draw_class.c",),
        dependencies=("USE_DISPLAY", "USE_BERRY_PIXMAT"),
    ),
    "USE_DISPLAY_FONT_5X8": FeatureSpec((), dependencies=("USE_DISPLAY_FONTS",)),
    "USE_DISPLAY_FONT_12X24": FeatureSpec((), dependencies=("USE_DISPLAY_FONTS",)),
    "USE_DISPLAY_FONT_16X32": FeatureSpec((), dependencies=("USE_DISPLAY_FONTS",)),
    "USE_UNIVERSAL_TOUCH": FeatureSpec(
        ("platform/display/touch.cpp", "app/drivers/i2c/i2c_device_claims.cpp"),
        dependencies=("USE_DISPLAY",),
    ),
    "USE_DISPLAY_QSPI": FeatureSpec(
        (), dependencies=("USE_DISPLAY",), platforms=("esp", "bouffalo", "pico")
    ),
    "USE_WEB_HTTPS": FeatureSpec(
        ("app/services/web/web_tls_identity.cpp",),
        dependencies=("USE_WEB",),
        profiles=("Large",),
        platforms=("esp", "beken", "bouffalo", "amebad", "pico"),
    ),
    "USE_MODBUS_RTU": FeatureSpec(
        (
            "app/drivers/modbus/rtu_service.cpp",
            "app/drivers/modbus/rtu_commands.cpp",
            "app/drivers/modbus/rtu_configuration.cpp",
        ),
        dependencies=("USE_SERIAL",),
    ),
    "USE_GPS": FeatureSpec(
        ("app/drivers/gps/ubx_protocol.cpp", "app/drivers/gps/gps_service.cpp"),
        dependencies=("USE_TIME", "USE_SERIAL"),
    ),
    "USE_SERIAL": FeatureSpec(("app/drivers/serial/serial_service.cpp",)),
    # No profile selects BLE until its footprint is measured and qualified.
    "USE_BLE": FeatureSpec(
        ("app/services/ble/ble_commands.cpp", "app/services/ble/ble_advertisements.cpp"),
        platforms=("esp", "bouffalo", "amebad", "pico"),
    ),
    # No profile selects Matter; the other families follow once their network prerequisites exist.
    "USE_MATTER": FeatureSpec(
        (
            "app/services/matter/berry/matter_module.cpp",
            "app/services/matter/berry/matter_classes.c",
            "app/services/matter/matter_cert.cpp",
            "app/services/matter/matter_dns_sd.cpp",
            "app/services/matter/matter_stack.cpp",
            "app/services/matter/matter_wire.cpp",
            "platform/lwip/matter_udp.cpp",
        ),
        dependencies=("USE_BERRY", "USE_WIFI"),
        platforms=("esp", "pico", "beken"),
        crypto=("AES", "CCM"),
        settings=(
            FeatureSetting(
                "passcode",
                "NEWMOTA_MATTER_PASSCODE",
                1,
                99999998,
                excluded=(
                    11111111,
                    22222222,
                    33333333,
                    44444444,
                    55555555,
                    66666666,
                    77777777,
                    88888888,
                    12345678,
                    87654321,
                ),
                personal=True,
            ),
            FeatureSetting("discriminator", "NEWMOTA_MATTER_DISCRIMINATOR", 0, 4095),
        ),
    ),
    "USE_MATTER_BLE": FeatureSpec(
        (
            "app/services/matter/matter_ble.cpp",
            "app/services/matter/matter_btp.cpp",
        ),
        dependencies=("USE_MATTER", "USE_BLE"),
        platforms=("esp", "pico"),
    ),
    "USE_MATTER_BRIDGE": FeatureSpec(
        (), dependencies=("USE_MATTER", "USE_MQTT"), platforms=("esp", "pico")
    ),
    "USE_MATTER_ENERGY": FeatureSpec((), dependencies=("USE_MATTER", "USE_ENERGY")),
    # Large messages over TCP, and the device as a client of its controllers.
    "USE_MATTER_TCP": FeatureSpec(
        ("platform/lwip/matter_tcp.cpp",), dependencies=("USE_MATTER",), platforms=("esp",)
    ),
    "USE_MATTER_CAMERA": FeatureSpec(
        (),
        dependencies=("USE_MATTER_TCP", "USE_CSI_WEBCAM", "USE_AUDIO_OPUS"),
        platforms=("esp",),
    ),
    "USE_I2S": FeatureSpec(
        (
            "app/drivers/audio/i2s_service.cpp",
            "app/drivers/audio/audio_session.cpp",
            "app/drivers/audio/audio_commands.cpp",
            "app/drivers/audio/audio_codecs.cpp",
            "app/drivers/audio/audio_store.cpp",
            "app/core/storage/driver_settings_file.cpp",
        ),
        platforms=("esp", "amebad", "pico"),
    ),
    "USE_AUDIO_MP3": FeatureSpec(
        (), dependencies=("USE_I2S",), platforms=("esp", "amebad", "pico")
    ),
    "USE_AUDIO_AAC": FeatureSpec(
        (), dependencies=("USE_I2S",), platforms=("esp", "amebad", "pico")
    ),
    "USE_AUDIO_OPUS": FeatureSpec(
        (), dependencies=("USE_I2S",), platforms=("esp", "amebad", "pico")
    ),
    "USE_AUDIO_SAM": FeatureSpec(
        (), dependencies=("USE_I2S",), platforms=("esp", "amebad", "pico")
    ),
    # Berry audio module with its level meter, spectrum analyser and tone generator.
    "USE_BERRY_AUDIO": FeatureSpec(
        (
            "app/services/berry/modules/audio/audio_module.cpp",
            "app/drivers/audio/audio_level.cpp",
            "app/drivers/audio/audio_spectrum.cpp",
            "app/drivers/audio/audio_tone.cpp",
        ),
        dependencies=("USE_BERRY", "USE_I2S"),
        platforms=("esp", "amebad", "pico"),
    ),
    "USE_WEB_AUDIO": FeatureSpec(
        ("app/services/web/audio_web.cpp",),
        dependencies=("USE_WEB", "USE_I2S"),
        platforms=("esp", "amebad", "pico"),
    ),
    "USE_ETHERNET": FeatureSpec(
        (
            "app/services/network/ethernet_service.cpp",
            "app/core/storage/driver_settings_file.cpp",
        ),
        dependencies=("USE_WIFI",),
        platforms=("esp",),
    ),
    # Tasmota's Berry ULP module for the ESP32 ULP-FSM, the S2/S3 ULP-RISC-V and the LP core.
    "USE_BERRY_ULP": FeatureSpec(
        ("app/services/berry/modules/ulp/ulp_module.cpp",),
        dependencies=("USE_BERRY",),
        platforms=("esp",),
    ),
    # Tasmota-style add_driver hooks, Driver class and add_cmd commands; no Rules engine.
    "USE_BERRY_DRIVERS": FeatureSpec(
        (
            "app/services/berry/modules/newmota/drivers.cpp",
            "app/services/berry/modules/newmota/commands.cpp",
            "app/services/berry/modules/newmota/driver_class.c",
        ),
        dependencies=("USE_BERRY",),
    ),
    # Tasmota's DeepSleepTime; the families add their native deep-sleep entry.
    "USE_DEEPSLEEP": FeatureSpec(
        (
            "app/core/system/deep_sleep_service.cpp",
            "app/core/storage/driver_settings_file.cpp",
        ),
        dependencies=("USE_TIME",),
        platforms=("esp", "beken", "bouffalo", "amebad", "pico"),
    ),
    "USE_WEBCAM": FeatureSpec(
        (
            "app/drivers/camera/camera_service.cpp",
            "app/services/berry/modules/cam/cam_module.cpp",
            "app/core/storage/driver_settings_file.cpp",
        ),
        dependencies=("USE_BERRY", "USE_I2C", "USE_WIFI"),
        platforms=("esp",),
    ),
    "USE_CSI_WEBCAM": FeatureSpec(
        (
            "app/drivers/camera/camera_service.cpp",
            "app/drivers/audio/audio_webrtc.cpp",
            "app/services/berry/modules/cam/cam_module.cpp",
            "app/core/storage/driver_settings_file.cpp",
        ),
        dependencies=("USE_BERRY", "USE_I2C"),
        platforms=("esp",),
        # WebRTC: STUN and SRTP authentication, the DTLS 1.2 handshake with
        # ECDHE-ECDSA-AES128-GCM-SHA256, and the WebSocket handshake.
        crypto=(
            "SHA1",
            "SHA224_SHA256",
            "HMAC",
            "AES",
            "GCM",
            "GHASH",
            "EC",
            "ECDSA_SIGN",
            "INT_I15",
            "TLS12_PRF",
        ),
    ),
    # Tasmota's Berry img class: one image in a kept native buffer, JPEG decoded by JPEGDEC.
    "USE_BERRY_IMAGE": FeatureSpec(
        (
            "app/services/berry/modules/img/img_module.cpp",
            "third_party/jpegdec/port/jpegdec_port.cpp",
            "third_party/jpegdec/port/jpegdec_simd.S",
        ),
        dependencies=("USE_BERRY",),
        platforms=("esp",),
    ),
    # Full-sensor stills past the ISP's 1920x1080, converted in software.
    "USE_CSI_STILL": FeatureSpec(
        ("platform/esp-idf/camera_csi_still.cpp",),
        dependencies=("USE_CSI_WEBCAM",),
        platforms=("esp",),
    ),
    "USE_WS2812": FeatureSpec(
        (
            "platform/led_strip_encoding.cpp",
            "app/drivers/led_strip/led_strip_service.cpp",
        ),
        dependencies=("USE_PWM",),
        roles=("application",),
        platforms=("esp", "bouffalo", "amebad", "pico"),
    ),
    "USE_LED_DRIVER": FeatureSpec(
        ("app/drivers/led_driver/led_driver.cpp",), dependencies=("USE_PWM",)
    ),
    "USE_TUYA_MCU": FeatureSpec(("app/drivers/tuya/tuya_mcu.cpp",), dependencies=("USE_SERIAL",)),
    # Tasmota's TuyaSNS (fnId 71-84) on the sensor service, which a relay-only build does without.
    "USE_TUYA_MCU_SENSORS": FeatureSpec(
        ("app/drivers/tuya/tuya_sensors.cpp",),
        dependencies=("USE_TUYA_MCU",),
        sensor=SensorSpec(
            7,
            "tuya_sensor_driver",
            "TuyaSNS",
            "Temperature,TempSet,Humidity,HumSet,Illuminance",
            output_fields=(
                "Temperature",
                "TempSet",
                "Humidity",
                "HumSet",
                "Illuminance",
                "TVOC",
                "eCO2",
                "CarbonDioxide",
                "Gas",
                "PM",
                "Timer1",
                "Timer2",
                "Timer3",
                "Timer4",
            ),
        ),
    ),
    # Tasmota's XNRG_32 (fnId 31-37) on the energy service, which a relay-only build does without.
    "USE_TUYA_MCU_ENERGY": FeatureSpec(
        ("app/drivers/tuya/tuya_energy.cpp",),
        dependencies=("USE_TUYA_MCU", "USE_ENERGY"),
        energy=EnergySpec(32, "create_tuya_energy", "TuyaMCU", 1),
    ),
    "USE_BP5758D": FeatureSpec(
        ("app/drivers/led_driver/bp5758d.cpp",), dependencies=("USE_LED_DRIVER",)
    ),
    "USE_BP1658CJ": FeatureSpec(
        ("app/drivers/led_driver/bp1658cj.cpp",), dependencies=("USE_LED_DRIVER",)
    ),
    "USE_SM2335": FeatureSpec(
        ("app/drivers/led_driver/sm2335.cpp",), dependencies=("USE_LED_DRIVER",)
    ),
    "USE_SM2135": FeatureSpec(
        ("app/drivers/led_driver/sm2135.cpp",), dependencies=("USE_LED_DRIVER",)
    ),
    "USE_I2C": FeatureSpec(
        (
            "platform/i2c.cpp",
            "app/drivers/i2c/i2c_service.cpp",
            "app/drivers/i2c/i2c_commands.cpp",
        )
    ),
    "USE_RTC_CHIPS": FeatureSpec(
        (
            "app/drivers/rtc/rtc_service.cpp",
            "app/drivers/i2c/i2c_device_claims.cpp",
        ),
        dependencies=("USE_TIME", "USE_I2C"),
    ),
    "USE_DS3231": FeatureSpec(("app/drivers/rtc/ds3231.cpp",), dependencies=("USE_RTC_CHIPS",)),
    "USE_DS1307": FeatureSpec(("app/drivers/rtc/ds1307.cpp",), dependencies=("USE_RTC_CHIPS",)),
    "USE_BM8563": FeatureSpec(("app/drivers/rtc/nxp.cpp",), dependencies=("USE_RTC_CHIPS",)),
    "USE_PCF85063": FeatureSpec(("app/drivers/rtc/nxp.cpp",), dependencies=("USE_RTC_CHIPS",)),
    "USE_PCF85363": FeatureSpec(("app/drivers/rtc/nxp.cpp",), dependencies=("USE_RTC_CHIPS",)),
    "USE_RV3028": FeatureSpec(("app/drivers/rtc/rv3028.cpp",), dependencies=("USE_RTC_CHIPS",)),
    "USE_RX8010": FeatureSpec(("app/drivers/rtc/epson.cpp",), dependencies=("USE_RTC_CHIPS",)),
    "USE_RX8025": FeatureSpec(("app/drivers/rtc/epson.cpp",), dependencies=("USE_RTC_CHIPS",)),
    "USE_RX8130": FeatureSpec(("app/drivers/rtc/epson.cpp",), dependencies=("USE_RTC_CHIPS",)),
    # Upstream spells RX8130CE as RX8030; both names select one physical model.
    "USE_RX8030": FeatureSpec((), dependencies=("USE_RX8130",)),
    "USE_BH1750": FeatureSpec(
        ("app/drivers/sensors/bh1750.cpp",),
        sensor=SensorSpec(
            1, "bh1750_driver", "BH1750", "Illuminance", output_fields=("Illuminance",)
        ),
        dependencies=("USE_I2C",),
    ),
    "USE_TC74": FeatureSpec(
        ("app/drivers/sensors/tc74.cpp",),
        sensor=SensorSpec(2, "tc74_driver", "TC74", "Temperature", output_fields=("Temperature",)),
        dependencies=("USE_I2C",),
    ),
    "USE_BME280": FeatureSpec(
        ("app/drivers/sensors/bme280.cpp",),
        sensor=SensorSpec(
            3,
            "bme280_driver",
            "BME280",
            "Temperature,Pressure,Humidity",
            output_fields=("Temperature", "Pressure", "Humidity"),
        ),
        dependencies=("USE_I2C",),
    ),
    "USE_DHT": FeatureSpec(
        ("app/drivers/sensors/dht.cpp",),
        sensor=SensorSpec(
            4,
            "dht_driver",
            "DHT",
            "Temperature,Humidity,DewPoint",
            output_fields=("Temperature", "Humidity", "DewPoint"),
        ),
    ),
    "USE_BLE_SENSORS": FeatureSpec(
        ("app/drivers/sensors/ble_sensors.cpp",),
        sensor=SensorSpec(
            5,
            "ble_sensors_driver",
            "BLE",
            "Temperature,Humidity,DewPoint,Illuminance,Moisture,Fertility,Battery,Weight,RSSI",
            output_fields=(
                "Temperature",
                "Humidity",
                "DewPoint",
                "Illuminance",
                "Moisture",
                "Fertility",
                "Door",
                "Leak",
                "NMT",
                "Battery",
                "Weight",
                "Impedance",
                "RSSI",
            ),
            remote=True,
        ),
        dependencies=("USE_BLE",),
        platforms=("esp", "bouffalo", "amebad", "pico"),
        crypto=("AES", "CCM"),
    ),
}
FEATURE_DEPENDENCIES.update({name: spec.dependencies for name, spec in OPTIONAL_FEATURES.items()})


# Berry exposure is independent from portable C requirements. A native consumer
# may select a primitive without a VM. Only the all-features qualification profile
# includes the extra test bindings.
CRYPTO_BERRY_REQUIREMENTS = {
    "MD5": ("MD5",),
    "SHA256": ("SHA224_SHA256",),
    "HMAC_SHA256": ("HMAC", "SHA224_SHA256"),
    "HKDF_SHA256": ("HKDF_PBKDF2", "HMAC", "SHA224_SHA256"),
    "PBKDF2_HMAC_SHA256": ("PBKDF2_SHA256", "HMAC", "SHA224_SHA256"),
    "AES_CBC": ("AES",),
    "AES_CTR": ("AES",),
    "AES_GCM": ("AES", "GCM", "GHASH"),
    "AES_CCM": ("AES", "CCM"),
    "CHACHA20_POLY1305": ("CHACHA20", "POLY1305", "RAW_POLY1305"),
    "EC_C25519": ("EC",),
    "EC_P256": ("EC", "ECDSA_VERIFY", "ECDSA_SIGN", "INT_I15", "HMAC", "SHA224_SHA256"),
    "ED25519": ("ED25519", "SHA384_SHA512", "INT_I15"),
    "RSA": ("RSA_SIGN", "INT_I15", "SHA224_SHA256"),
    "SPAKE2P_MATTER": (
        "EC",
        "ECDSA_VERIFY",
        "ECDSA_SIGN",
        "INT_I15",
        "HMAC",
        "SHA224_SHA256",
        "HKDF_PBKDF2",
        "PBKDF2_SHA256",
        "AES",
        "CCM",
    ),
    "RANDOM": (),
    "TEST_BLAKE2S": ("BLAKE2S",),
    "TEST_XCHACHA20_POLY1305": ("XCHACHA20_POLY1305", "CHACHA20", "POLY1305"),
}
CRYPTO_PRIMITIVE_DEPENDENCIES = {
    "HKDF_PBKDF2": ("HMAC", "SHA224_SHA256"),
    "PBKDF2_SHA256": ("HMAC", "SHA224_SHA256"),
    "ECDSA_SIGN": ("ECDSA_VERIFY", "EC", "HMAC", "SHA224_SHA256", "INT_I15"),
    "ECDSA_VERIFY": ("EC", "INT_I15"),
    "EC": ("INT_I15",),
    "TLS12_PRF": ("HMAC", "SHA224_SHA256"),
    "RSA_SIGN": ("INT_I15", "SHA224_SHA256"),
    "GCM": ("AES", "GHASH"),
    "CCM": ("AES",),
    "EAX": ("AES",),
    "POLY1305": ("INT_I15",),
    "ED25519": ("SHA384_SHA512", "INT_I15"),
    "XCHACHA20_POLY1305": ("CHACHA20", "POLY1305"),
}


def crypto_selection(
    environment: Environment, values: dict[str, bool] | None = None
) -> tuple[dict[str, bool], tuple[str, ...]]:
    """Resolve the explicit crypto profile, selected native users, and source sets."""
    from newmota.crypto import (
        BEARSSL_SOURCE_ROOT,
        EXTENSION_SOURCES,
        PRIMITIVE_SOURCES,
        extension_sources,
        hardware_crypto_sources,
        resolve_primitive_sources,
    )

    selection = environment.crypto
    all_primitives = set(PRIMITIVE_SOURCES) | set(EXTENSION_SOURCES)
    if selection is None:
        berry = {"MD5", "SHA256", "RANDOM"} if environment.role == "application" else set()
        if (
            environment.role == "application"
            and environment.platform == "esp"
            and environment.profile in {"Standard", "Large"}
        ):
            berry.update(
                {
                    "EC_P256",
                    "HMAC_SHA256",
                    "HKDF_SHA256",
                    "AES_CCM",
                    "AES_CTR",
                    "PBKDF2_HMAC_SHA256",
                    "SPAKE2P_MATTER",
                    "AES_GCM",
                }
            )
        primitives = set()
    elif selection == "all":
        berry = set(CRYPTO_BERRY_REQUIREMENTS)
        primitives = set(all_primitives)
    elif selection == "none":
        berry, primitives = set(), set()
    elif isinstance(selection, dict) and set(selection) <= {"berry", "primitives"}:
        for key in ("berry", "primitives"):
            names = selection.get(key, [])
            if not isinstance(names, list) or any(not isinstance(name, str) for name in names):
                raise BuildError(f"crypto.{key} must be a list of names")
        berry = set(selection.get("berry", []))
        primitives = set(selection.get("primitives", []))
    else:
        raise BuildError("crypto must be 'all', 'none', or a berry/primitives mapping")
    # The Berry stack above the native session layer handshakes with these classes.
    if values is not None and values.get("USE_MATTER", False):
        berry.update({"SPAKE2P_MATTER", "AES_CCM"})
    if values is not None and not values.get("USE_BERRY", True):
        if isinstance(selection, dict) and selection.get("berry"):
            raise BuildError("crypto.berry requires USE_BERRY, explicitly disabled in features")
        berry = set()
    if berry - CRYPTO_BERRY_REQUIREMENTS.keys():
        raise BuildError(
            f"unknown Berry crypto selection: {sorted(berry - CRYPTO_BERRY_REQUIREMENTS.keys())}"
        )
    if primitives - all_primitives:
        raise BuildError(f"unknown C crypto selection: {sorted(primitives - all_primitives)}")
    if berry and environment.role != "application":
        raise BuildError("Berry crypto requires application role")
    # The Matter closure calls these public classes itself.
    if "SPAKE2P_MATTER" in berry:
        berry.update(
            {"EC_P256", "SHA256", "HMAC_SHA256", "HKDF_SHA256", "PBKDF2_HMAC_SHA256", "RANDOM"}
        )
    for name in berry:
        primitives.update(CRYPTO_BERRY_REQUIREMENTS[name])
    for name, spec in OPTIONAL_FEATURES.items():
        if values is not None and values.get(name):
            primitives.update(spec.crypto)
    # The ESP-IDF fork leaves the SDK's application-side image hash (bootloader_sha256_*)
    # to the application when no requirement links mbedTLS; Newmota supplies it with
    # BearSSL SHA-256 (src/platform/esp-idf/sdk_image_sha256.c).
    if environment.platform == "esp" and values is not None:
        from newmota.platforms.esp.build import links_sdk_mbedtls

        if not links_sdk_mbedtls(environment, values):
            primitives.add("SHA224_SHA256")
    # The prebuilt Beken supplicant and the Bouffalo Wi-Fi 6 supplicant built with
    # CONFIG_WIFI6_EXTERNAL_CRYPTO take HMAC, PBKDF2-SHA1 and SHA-384/512 from their backend
    # (src/platform/crypto/hostap_hash_bearssl.c); ESP-IDF compiles hostap's own.
    if (
        environment.platform in {"beken", "bouffalo"}
        and values is not None
        and values.get("USE_WIFI_WPA3_BEARSSL", False)
    ):
        primitives.update({"HMAC", "SHA384_SHA512"})
    while True:
        expanded = primitives | {
            dep for name in primitives for dep in CRYPTO_PRIMITIVE_DEPENDENCIES.get(name, ())
        }
        if expanded == primitives:
            break
        primitives = expanded
    if primitives or berry:
        primitives.update({"CODEC", "SUPPORT"})
    flags = {f"USE_BERRY_CRYPTO_{name}": name in berry for name in CRYPTO_BERRY_REQUIREMENTS}
    flags.update({f"USE_CRYPTO_{name}": name in primitives for name in sorted(all_primitives)})
    flags["USE_BERRY_CRYPTO"] = bool(berry)
    flags["USE_CRYPTO"] = bool(primitives)
    flags["USE_CRYPTO_RANDOM"] = "RANDOM" in berry
    sources = tuple(
        str((BEARSSL_SOURCE_ROOT / name).relative_to(ROOT))
        for name in resolve_primitive_sources(tuple(sorted(primitives & PRIMITIVE_SOURCES.keys())))
    )
    sources += extension_sources(tuple(sorted(primitives & EXTENSION_SOURCES.keys())))
    if values is not None:
        sources = hardware_crypto_sources(environment, values, sources)
    if berry:
        sources += ("src/app/services/berry/modules/crypto/crypto_module.cpp",)
    if "SPAKE2P_MATTER" in berry:
        sources += ("src/app/services/berry/modules/crypto/crypto_spake2p.c",)
    if "RANDOM" in berry:
        sources += (
            ("src/platform/esp-idf/crypto_random.cpp",)
            if environment.platform == "esp"
            else ("src/platform/pico-sdk/crypto_random.cpp",)
            if environment.platform == "pico"
            else ("src/platform/beken/crypto_random.cpp",)
            if environment.platform == "beken"
            else ("src/platform/crypto_random_unavailable.cpp",)
        )
    return flags, tuple(sorted(set(sources)))


def _requirements(
    environment: Environment, values: dict[str, bool], layout: LayoutFacts | None
) -> tuple[tuple[str, ...], tuple[str, ...], dict[str, str]]:
    """Resolve owned sources and verified native requirements before SDK configuration."""

    for feature, dependencies in FEATURE_DEPENDENCIES.items():
        for name in dependencies:
            if values[feature] and not values[name]:
                raise BuildError(f"{feature} requires {name}: {dependency_reason(feature, name)}")
    from newmota.platforms import platform_for_build

    platform = environment.platform_directory
    native_sources, sdk_requirements, native_settings = platform_for_build(
        environment.platform
    ).build.native_requirements(environment, values, layout)
    sources = [*CORE_SOURCES, "platform/gpio_topology.cpp", *native_sources]
    # ESP-IDF stubs the exception runtime and Beken links libstdc++_nano; these
    # toolchains would otherwise link libstdc++'s throwing helpers. Pico's SDK
    # already provides non-throwing static guards.
    if environment.platform in {"bouffalo", "amebad", "pico"}:
        sources.append("platform/cxx_runtime.cpp")
    if environment.platform in {"bouffalo", "amebad"}:
        sources.append("platform/cxx_guard.cpp")
    for feature, owned in {
        **FEATURE_SOURCES,
        **{name: spec.sources for name, spec in OPTIONAL_FEATURES.items()},
    }.items():
        if values[feature]:
            sources.extend(owned)
    if (
        values["USE_ENERGY"]
        or values["USE_BERRY"]
        or values["USE_SERIAL"]
        or values["USE_WS2812"]
        or values["USE_I2S"]
        or values["USE_EDGE_CAPTURE"]
        or values["USE_ADC"]
    ):
        sources.append("app/core/hardware/runtime_pin_claims.cpp")
    if (
        environment.platform == "bouffalo"
        and environment.sdk_target == "bl602"
        and values["USE_WIFI_WPA3_BEARSSL"]
    ):
        # The BL602 supplicant keeps SAE on the SDK's blcrypto_suite; only its hashes move.
        sources.remove("platform/crypto/hostap_sae_crypto.c")
    if values["USE_SENSORS"] or values["USE_ENERGY"] or values["USE_SWITCHES"]:
        sources.append("app/services/telemetry/measurement_document.cpp")
    if values["USE_SENSORS"]:
        sources.append("app/drivers/sensors/sensor_service.cpp")
        sources.append("app/drivers/sensors/sensor_history.cpp")
    if values["USE_ADC"]:
        sources.append(f"platform/{platform}/adc.cpp")
        if environment.platform == "beken":
            sources.append("platform/beken/adc_native.c")
    if values["USE_EDGE_CAPTURE"]:
        sources.append(f"platform/{platform}/edge_capture.cpp")
        if environment.platform == "beken":
            sources.append("platform/beken/edge_capture_native.c")
    if values["USE_SERIAL"]:
        sources.append(f"platform/{platform}/serial.cpp")
        if environment.platform == "beken":
            sources.append("platform/beken/serial_native.c")
    if values["USE_I2S"]:
        sources.append(f"platform/{platform}/i2s.cpp")
        sources.append(f"platform/{platform}/audio_worker.cpp")
        if values["USE_WIFI"] and any(
            values[name] for name in ("USE_AUDIO_MP3", "USE_AUDIO_AAC", "USE_AUDIO_OPUS")
        ):
            sources.append("app/drivers/audio/audio_network.cpp")
        from newmota.audio import codec_sources

        sources.extend(codec_sources(values))
    if values["USE_CMSIS_DSP"]:
        sources.append("platform/cmsis_fft.cpp")
        sources.extend(f"third_party/cmsis_dsp/Source/{source}" for source in CMSIS_DSP_SOURCES)
    if values["USE_WS2812"]:
        sources.append(f"platform/{platform}/led_strip.cpp")
    spi_controller = environment.platform in SPI_CONTROLLER_PLATFORMS
    if values["USE_SDCARD"]:
        # ESP-IDF has its own transports, the controller families the SPI-mode protocol; the
        # rest report the card unsupported.
        if environment.platform == "esp":
            sources.append(f"platform/{platform}/sd_card.cpp")
        elif spi_controller:
            sources.append("platform/sd_card_spi.cpp")
            # BL616 and BL618DG add their SD host for SDIO cards.
            sources.append(
                "platform/bouffalo/sd_card_sdh.cpp"
                if environment.sdk_target in SDH_TARGETS
                else "platform/sd_card_sdio_unavailable.cpp"
            )
        else:
            sources.append("platform/sd_card_unavailable.cpp")
    if spi_controller and (values["USE_SDCARD"] or values["USE_DISPLAY"]):
        sources.append(f"platform/{platform}/spi_controller.cpp")
    if values["USE_BERRY_SOLIDIFY"]:
        sources.append("app/services/berry/modules/newmota/solidified_drivers.c")
    if values["USE_BERRY"] and values["USE_WS2812"]:
        sources.append("app/services/berry/modules/led_strip/led_strip_module.cpp")
    if values["USE_BERRY"] and values["USE_SERIAL"]:
        sources.append("app/services/berry/modules/serial/serial_module.cpp")
    if values["USE_BERRY"] and values["USE_I2C"]:
        sources.append("app/services/berry/modules/wire/wire_module.cpp")
    if values["USE_WIFI"]:
        sources.append(f"platform/{platform}/network.cpp")
    if values["USE_WEB"] or values["USE_RECOVERY_WEB"]:
        sources.append("platform/web_response.cpp")
    if values["USE_WEB"] and values["USE_PWM"]:
        sources.append("app/services/web/light_web.cpp")
    if values["USE_WEB"] and values["USE_BERRY"]:
        sources.extend(("app/services/web/widgets.cpp", "app/services/web/berry/widget_module.cpp"))
    if values["USE_BERRY"] and values["USE_TIME"]:
        sources.append("app/services/berry/modules/newmota/cron.cpp")
    if values["USE_BERRY"] and values["USE_MQTT"]:
        sources.append("app/services/mqtt/berry/mqtt_module.cpp")
    if values["USE_BERRY"] and values["USE_BLE"]:
        # The cb module gives scripts native callback pointers for BLE.conn_cb().
        sources.extend(
            (
                "app/services/berry/modules/ble/ble_module.cpp",
                "third_party/berry_mapping/src/be_cb_module.c",
            )
        )
    if values["USE_BERRY"]:
        sources.extend(
            str(path.relative_to(ROOT / "src"))
            for path in sorted((ROOT / "src/third_party/berry/src").glob("*.c"))
        )
    if values["USE_LVGL"]:
        from newmota.lvgl import lvgl_sources

        sources.extend(lvgl_sources())
    if values["USE_DISPLAY_QSPI"]:
        sources.append(f"platform/{platform}/display_qspi.cpp")
    if values["USE_DISPLAY"] and environment.platform != "esp":
        # Controller families share one transport over spi_controller.cpp; Beken has a stub.
        sources.append(
            "platform/display/native_spi.cpp"
            if spi_controller
            else f"platform/{platform}/display.cpp"
        )
    _, crypto_sources = crypto_selection(environment, values)
    sources.extend(source.removeprefix("src/") for source in crypto_sources)
    sources = list(dict.fromkeys(sources))
    for source in sources:
        if not (ROOT / "src" / source).is_file():
            raise BuildError(f"required source is missing: src/{source}")
    return tuple(f"src/{source}" for source in sources), tuple(sdk_requirements), native_settings


def feature_values(environment: Environment, layout: LayoutFacts | None = None) -> dict[str, bool]:
    """Apply role and profile defaults, explicit overrides, and dependencies."""

    if environment.role == "application":
        use_berry = True
    elif environment.role == "safeboot":
        use_berry = False
    else:
        raise BuildError(
            f"environment '{environment.name}' has unsupported role '{environment.role}'"
        )

    # Chips without a radio reach Wi-Fi through an ESP-Hosted companion, as in Tasmota.
    use_hosted = (
        environment.platform == "esp"
        and environment.capabilities.get("wifi") is not True
        and environment.capabilities.get("wifi-hosted") is True
    )
    use_wifi = environment.capabilities.get("wifi") is True or use_hosted
    use_mqtt = use_wifi and environment.platform in {"esp", "beken", "bouffalo", "amebad", "pico"}
    use_message_routing = environment.role == "application" or use_mqtt
    use_web = (
        use_wifi
        and environment.role == "application"
        and environment.platform in {"esp", "beken", "bouffalo", "amebad", "pico"}
    )
    critical_settings = layout is not None and not layout.settings_filesystem_preserved
    assignments = (
        ("USE_PWM", environment.role == "application"),
        ("USE_BERRY", use_berry),
        ("USE_BERRY_PERSIST", use_berry),
        ("USE_BERRY_GPIO", use_berry),
        ("USE_BERRY_PIXMAT", use_berry),
        ("USE_HARDWARE_COMMANDS", environment.role == "application"),
        ("USE_DEFER", environment.role == "application"),
        ("USE_STATUS_LEDS", environment.role == "application"),
        ("USE_SWITCHES", environment.role == "application"),
        ("USE_SLEEP", environment.role == "application"),
        # WifiPower needs a native adapter and chip limits, which only ESP-IDF has so far.
        (
            "USE_WIFI_POWER",
            use_wifi and environment.role == "application" and environment.platform == "esp",
        ),
        # Tasmota's full STATE and Status fields; Safeboot keeps UptimeSec, the power fields
        # and its short Status sections.
        ("USE_TASMOTA_STATE", environment.role == "application"),
        # App-owned sensor policy is retained even when no driver is selected.
        # Safeboot must not link its persistence or alter its private file.
        ("USE_SENSOR_SETTINGS", environment.role == "application"),
        ("USE_MESSAGE_ROUTING", use_message_routing),
        # Application roles also serve browser/state consumers without MQTT.
        # Safeboot owns its router inside the lazy MQTT runtime instead.
        ("USE_APP_MESSAGE_ROUTING", environment.role == "application"),
        ("USE_WIFI", use_wifi),
        ("USE_ESP_HOSTED", use_hosted),
        # A normal application needs a declared paired Safeboot image before
        # URL pulls can hand off safely. Beken is the native-writer exception;
        # Safeboot roles pull directly into their recovery writer.
        ("USE_WGET", use_wifi and environment.role == "application"),
        ("USE_PING", use_wifi and environment.role == "application"),
        (
            "USE_OTA_PULL",
            use_wifi
            and (
                environment.role == "safeboot"
                or environment.safeboot is not None
                or environment.platform == "beken"
            ),
        ),
        # MQTT follows Tasmota into every network-capable role by default.
        # The durable settings record remains present when this is off.
        ("USE_MQTT", use_mqtt),
        # Network-capable application profiles carry the web baseline by default
        # once their native HTTP adapter is implemented.  Safeboot remains
        # limited to its dedicated recovery listener and page.
        ("USE_WEB", use_web),
        # Web panels that small builds can leave out; each also drops its part of the page.
        ("USE_WEB_CONSOLE", use_web),
        ("USE_WEB_FILESYSTEM", use_web),
        ("USE_WEB_GPIO", use_web),
        (
            "USE_RECOVERY_WEB",
            use_wifi
            and environment.role == "safeboot"
            and environment.platform in {"esp", "bouffalo", "amebad", "pico"},
        ),
        (
            "USE_BEKEN_UPDATE_WEB",
            use_wifi and environment.role == "application" and environment.platform == "beken",
        ),
        ("USE_TIME", environment.role == "application"),
        ("USE_TIMERS", environment.role == "application"),
        ("USE_NTP", use_wifi and environment.role == "application"),
        ("USE_UDP", use_wifi and environment.role == "application"),
        ("NEWMOTA_CRITICAL_SETTINGS_STORE", critical_settings),
        # Tasmota 15.5.0.2 shares the imported credential fields across these
        # ESP targets. Select by SDK target so profile and personal variants work.
        (
            "USE_ESP_TASMOTA_SETTINGS_IMPORT",
            environment.platform == "esp"
            and environment.role == "application"
            and environment.sdk_target
            in {
                "esp32",
                "esp32s3",
                "esp32s2",
                "esp32c3",
                "esp32c2",
                "esp32c6",
                "esp32p4",
                "esp32c5",
            },
        ),
    )
    crypto_flags, _ = crypto_selection(environment)
    assignments += tuple(sorted(crypto_flags.items()))
    values = dict(assignments)
    values.update(
        {
            name: environment.profile in spec.profiles and (name != "USE_WEB_HTTPS" or use_wifi)
            for name, spec in OPTIONAL_FEATURES.items()
        }
    )
    # ESP Wi-Fi builds (ESP-IDF 5.5 and 6.1) that keep WPA3 run it on BearSSL, with the hardware
    # SHA-1, SHA-256 and P-256 providers the target has. Beken Wi-Fi builds run the BDK's
    # prebuilt supplicant on the same backend. Off elsewhere; each can be turned off per build.
    if (
        use_wifi
        and not use_hosted  # the supplicant runs on the ESP-Hosted companion
        and environment.platform == "esp"
        and esp_keeps_wpa3(environment, bearssl=True)
    ):
        from newmota.crypto import HARDWARE_CRYPTO_TARGETS

        values["USE_WIFI_WPA3_BEARSSL"] = True
        for name in ("USE_CRYPTO_HW_SHA1", "USE_CRYPTO_HW_SHA256", "USE_CRYPTO_HW_P256"):
            values[name] = environment.sdk_target in HARDWARE_CRYPTO_TARGETS[name]
    if use_wifi and environment.platform == "beken":
        from newmota.platforms.beken.profile import BEARSSL_SUPPLICANT_TARGETS

        values["USE_WIFI_WPA3_BEARSSL"] = environment.sdk_target in BEARSSL_SUPPLICANT_TARGETS
    overrides = environment.features
    if not isinstance(overrides, dict):
        raise BuildError("features must be a mapping of USE_* names to booleans")
    disabled = set()
    for name, requested in overrides.items():
        if not isinstance(name, str) or name not in values or not name.startswith("USE_"):
            raise BuildError(f"unknown feature '{name}'")
        if name in crypto_flags:
            raise BuildError(f"{name} is selected through crypto, not features")
        enabled = _requested_enabled(name, requested)
        if enabled and name in CORE_FEATURES and not values[name]:
            raise BuildError(
                f"{name} is unavailable for this environment: "
                f"{unavailable_reason(name, environment.role)}"
            )
        values[name] = enabled
        if not enabled:
            disabled.add(name)
    # LVGL renders to the descriptor-selected panel on every native family.
    if values["USE_LVGL"] and environment.role == "application":
        if overrides.get("USE_DISPLAY") is False:
            raise BuildError("USE_LVGL requires USE_DISPLAY")
        values["USE_DISPLAY"] = True
    # Turning a feature off also drops what is only on by default above it.
    while True:
        changed = False
        for name, dependencies in FEATURE_DEPENDENCIES.items():
            if (
                values[name]
                and name not in overrides
                and any(dependency in disabled for dependency in dependencies)
            ):
                values[name] = False
                disabled.add(name)
                changed = True
        if not changed:
            break
    while True:
        changed = False
        for name, dependencies in FEATURE_DEPENDENCIES.items():
            if not values[name]:
                continue
            for dependency in dependencies:
                if dependency in disabled:
                    raise BuildError(
                        f"{name} requires {dependency}, explicitly disabled in features: "
                        f"{dependency_reason(name, dependency)}"
                    )
                if not values[dependency]:
                    values[dependency] = True
                    changed = True
        if not changed:
            break
    # pixmat draws for LED strips and displays, so only those builds get it unless it is set.
    if "USE_BERRY_PIXMAT" not in overrides:
        values["USE_BERRY_PIXMAT"] = values["USE_BERRY"] and (
            values["USE_WS2812"] or values["USE_DISPLAY"]
        )
    # Matter shows the energy meter only where both are built, unless it is set.
    if "USE_MATTER_ENERGY" not in overrides:
        values["USE_MATTER_ENERGY"] = values["USE_MATTER"] and values["USE_ENERGY"]
    # Selected native modules may add C crypto primitives to the profile's own set.
    values.update(crypto_selection(environment, values)[0])
    # These parts have no safe model ID, especially with an invalid calendar.
    # A global compile-time choice prevents trying incompatible layouts on one address.
    for address, models in (
        ("0x68", ("USE_DS1307", "USE_DS3231")),
        ("0x51", ("USE_BM8563", "USE_PCF85063", "USE_PCF85363")),
        ("0x32", ("USE_RX8010", "USE_RX8025", "USE_RX8130")),
    ):
        selected = [name for name in models if values[name]]
        if len(selected) > 1:
            raise BuildError(
                f"RTC address {address} requires one model selection: {', '.join(selected)}"
            )
    if values["USE_NTP_SERVER"] and not environment.base_environment:
        raise BuildError("USE_NTP_SERVER is available only in custom application variants")
    if values["USE_NTP_SERVER"] and not use_wifi:
        raise BuildError("USE_NTP_SERVER requires native Wi-Fi capability")
    if (
        values["USE_GPS"]
        and environment.platform == "bouffalo"
        and environment.sdk_target != "bl602"
    ):
        raise BuildError(
            "USE_GPS requires an available auxiliary UART; only BL602 supports it on Bouffalo"
        )
    if values["USE_I2S"] and environment.flash_kb < AUDIO_MINIMUM_FLASH_KB:
        raise BuildError("USE_I2S requires at least 4 MiB flash")
    if values["USE_WEBCAM"] and environment.capabilities.get("camera-dvp") is not True:
        raise BuildError(f"USE_WEBCAM requires a DVP camera interface; {environment.chip} has none")
    if values["USE_WEBCAM"] and environment.board_data.get("psram") is None:
        raise BuildError("USE_WEBCAM requires a board with PSRAM for its frame buffers")
    if values["USE_CSI_WEBCAM"] and environment.capabilities.get("mipi-csi") is not True:
        raise BuildError(f"USE_CSI_WEBCAM requires MIPI-CSI; {environment.chip} has none")
    if values["USE_CSI_WEBCAM"] and environment.board_data.get("psram") is None:
        raise BuildError("USE_CSI_WEBCAM requires a board with PSRAM for its frame buffers")
    if values["USE_BERRY_IMAGE"] and environment.board_data.get("psram") is None:
        raise BuildError("USE_BERRY_IMAGE requires a board with PSRAM for its image buffers")
    if values["USE_WEBCAM"] and values["USE_CSI_WEBCAM"]:
        raise BuildError("USE_WEBCAM and USE_CSI_WEBCAM exclude each other: one camera per device")
    matter_small_flash = (
        environment.flash_kb < MATTER_MINIMUM_FLASH_KB
        and environment.platform not in MATTER_SMALL_FLASH_PLATFORMS
    )
    if values["USE_MATTER"] and (matter_small_flash or environment.chip == "rp2040"):
        raise BuildError(
            "USE_MATTER requires at least 4 MiB flash and hardware entropy; "
            f"{environment.name} is unsupported"
        )
    for name, spec in OPTIONAL_FEATURES.items():
        if values[name] and (
            environment.role not in spec.roles or environment.platform not in spec.platforms
        ):
            raise BuildError(
                f"{name} is unsupported for {environment.role} on {environment.platform}"
            )
    # Code shared by the DVP and CSI cameras: the Berry module, commands and settings.
    values["USE_CAMERA"] = values["USE_WEBCAM"] or values["USE_CSI_WEBCAM"]
    # Driver classes a personal environment builds in; derived from its solidify list.
    values["USE_BERRY_SOLIDIFY"] = values["USE_BERRY"] and bool(environment.solidify)
    values["USE_SENSORS"] = any(
        values[name] and spec.sensor is not None for name, spec in OPTIONAL_FEATURES.items()
    )
    # The spectrum FFT runs on esp-dsp's kernels on the S3 and P4; derived, never selected.
    values["USE_ESP_DSP"] = (
        values["USE_BERRY_AUDIO"]
        and environment.platform == "esp"
        and environment.sdk_target in ESP_DSP_TARGETS
    )
    # And on CMSIS-DSP on the RP2350 and AmebaD; derived, never selected.
    values["USE_CMSIS_DSP"] = (
        values["USE_BERRY_AUDIO"] and environment.sdk_target in CMSIS_DSP_TARGETS
    )
    # Energy command paths that only some drivers can serve; derived, never selected directly.
    values["USE_ENERGY_REGISTERS"] = values["USE_ENERGY"] and any(
        values[name] for name in ENERGY_REGISTER_DRIVERS
    )
    values["USE_ENERGY_INJECT"] = values["USE_ENERGY"] and any(
        values[name] for name in ENERGY_INJECT_FEATURES
    )
    return values


def _requested_enabled(name: object, value: object) -> bool:
    """Return a feature's requested state, checking settings written under its key."""

    if isinstance(value, bool):
        return value
    spec = OPTIONAL_FEATURES.get(name) if isinstance(name, str) else None
    if spec is None or not spec.settings or not isinstance(value, dict):
        raise BuildError(f"features.{name} must be boolean")
    allowed = {setting.name: setting for setting in spec.settings}
    unknown = sorted(str(key) for key in value if key not in allowed)
    if unknown:
        raise BuildError(f"features.{name} has unknown settings: {', '.join(unknown)}")
    missing = [key for key in allowed if key not in value]
    if missing:
        raise BuildError(
            f"features.{name} requires all of its settings; missing {', '.join(missing)}"
        )
    for key, setting in allowed.items():
        number = value[key]
        if (
            not isinstance(number, int)
            or isinstance(number, bool)
            or not setting.minimum <= number <= setting.maximum
            or number in setting.excluded
        ):
            raise BuildError(
                f"features.{name}.{key} must be a whole number from {setting.minimum} to "
                f"{setting.maximum}"
                + (", but not a value Matter forbids" if setting.excluded else "")
            )
    return True


def personal_settings(features: dict[str, object]) -> list[str]:
    """Name the requested settings that belong only in a personal configuration."""

    return [
        f"{name}.{setting.name}"
        for name, value in features.items()
        if isinstance(value, dict)
        for setting in (OPTIONAL_FEATURES[name].settings if name in OPTIONAL_FEATURES else ())
        if setting.personal and setting.name in value
    ]


def feature_setting_lines(features: dict[str, object]) -> list[str]:
    """Emit the settings requested under enabled features."""

    return [
        f"#define {setting.macro} {value[setting.name]}"
        for name, value in features.items()
        if isinstance(value, dict)
        for setting in OPTIONAL_FEATURES[name].settings
    ]


def template_lines(template: dict[str, object] | None) -> list[str]:
    """Emit a checked default GPIO template for the settings defaults."""

    if template is None:
        return []
    name = json.dumps(template.get("NAME", "Generic"), ensure_ascii=False)
    return [
        f"#define NEWMOTA_DEFAULT_TEMPLATE_NAME {name}",
        f"#define NEWMOTA_DEFAULT_TEMPLATE_GPIO {','.join(str(value) for value in template['GPIO'])}",
        f"#define NEWMOTA_DEFAULT_TEMPLATE_FLAG {template.get('FLAG', 0)}u",
        f"#define NEWMOTA_DEFAULT_TEMPLATE_BASE {template.get('BASE', 0)}u",
    ]


def sensor_registry_lines(values: dict[str, bool]) -> list[str]:
    """Emit constant descriptor entries from the same metadata as source selection."""

    sensors = [
        spec.sensor
        for name, spec in OPTIONAL_FEATURES.items()
        if spec.sensor is not None and values[name]
    ]
    all_ids = [
        spec.sensor.driver_id for spec in OPTIONAL_FEATURES.values() if spec.sensor is not None
    ]
    if len(set(all_ids)) != len(all_ids) or any(not 1 <= value <= 128 for value in all_ids):
        raise BuildError("sensor driver IDs must be unique and within 1..128")
    all_names = [spec.sensor.name for spec in OPTIONAL_FEATURES.values() if spec.sensor is not None]
    if any(
        not 1 <= len(name) <= 16
        or not name.isascii()
        or any(not (character.isalnum() or character in " _-") for character in name)
        for name in all_names
    ):
        raise BuildError(
            "sensor driver names must be 1..16 ASCII letters, digits, spaces, '_' or '-'"
        )
    entries = [
        f'    X({sensor.symbol}, {sensor.driver_id}, "{sensor.name}", '
        f'"{sensor.output_field}", "{sensor_output_routes(sensor, values)}", '
        f"{'true' if sensor.remote else 'false'})"
        for sensor in sensors
    ]
    continuation = " \\"
    return [
        f"#define NEWMOTA_SENSOR_DRIVER_COUNT {len(sensors)}",
        "#define NEWMOTA_SENSOR_DRIVERS(X)" + (continuation if entries else ""),
        *(
            entry + (continuation if index + 1 < len(entries) else "")
            for index, entry in enumerate(entries)
        ),
    ]


def energy_registry_lines(values: dict[str, bool]) -> list[str]:
    """Generate only implemented selected drivers; inventory entries alone enable no code."""
    from newmota.energy_inventory import ENERGY_INVENTORY

    selected = [
        spec.energy
        for name, spec in OPTIONAL_FEATURES.items()
        if spec.energy is not None and values[name]
    ]
    identities = {entry.driver_id: entry for entry in ENERGY_INVENTORY}
    if len(identities) != len(ENERGY_INVENTORY):
        raise BuildError("energy inventory IDs must be unique")
    for driver in selected:
        if (
            driver.driver_id in identities
            and identities[driver.driver_id].support_scope != "included"
        ):
            raise BuildError(
                f"energy driver ID {driver.driver_id} is permanently reserved and unavailable under current user scope"
            )
        if driver.driver_id not in identities or not 1 <= driver.maximum_channels <= 32:
            raise BuildError("energy driver requires an inventoried ID and 1..32 channels")
        if (
            not 0 <= driver.default_flags <= 255
            or not 1 <= driver.default_invalid_power_frames <= 65535
        ):
            raise BuildError("energy driver defaults must fit the persisted options fields")
    if len({driver.driver_id for driver in selected}) != len(selected):
        raise BuildError("selected energy IDs must be unique")
    entries = [
        f'    X({driver.symbol}, {driver.driver_id}, "{driver.name}", {driver.maximum_channels}, '
        f"{driver.default_flags}, {driver.default_invalid_power_frames})"
        for driver in selected
    ]
    continuation = " \\"
    return [
        f"#define NEWMOTA_ENERGY_DRIVER_COUNT {len(selected)}",
        "#define NEWMOTA_ENERGY_DRIVERS(X)" + (continuation if entries else ""),
        *(
            entry + (continuation if index + 1 < len(entries) else "")
            for index, entry in enumerate(entries)
        ),
    ]


def sensor_output_routes(sensor: SensorSpec, values: dict[str, bool]) -> str:
    """Return only telemetry surfaces compiled into this environment."""

    enabled = {
        "MQTT SENSOR": values["USE_MQTT"],
        "GET /api/sensors": values["USE_WEB"],
        "GET /api/sensors/readings": values["USE_WEB"],
    }
    return ", ".join(
        route
        for route in (item.strip() for item in sensor.output_routes.split(","))
        if enabled.get(route, True)
    )


def resolve_features(
    environment: Environment, work_dir: Path, layout: LayoutFacts | None = None
) -> ResolvedFeatures:
    """Resolve validated features and emit C and JSON representations."""

    from newmota.energy_inventory import ENERGY_INVENTORY, UPSTREAM_ENERGY_BASELINE

    assignments = tuple(feature_values(environment, layout).items())
    sources, sdk_requirements, native_settings = _requirements(
        environment, dict(assignments), layout
    )
    psram = environment.board_data.get("psram")
    psram_mode = (
        {"quad": "QSPI", "octal": "OPI", "hex": "HEX"}.get(psram.get("mode"))
        if isinstance(psram, dict)
        else None
    )
    identity_input = {
        "environment": environment.name,
        "role": environment.role,
        "profile": environment.profile,
        "features": dict(assignments),
        "requested_features": environment.features,
        "device_name": environment.device_name,
        "template": environment.template,
        "psram_mode": psram_mode,
        "sources": sources,
        "sdk_requirements": sdk_requirements,
        "native_settings": native_settings,
        "dependencies": FEATURE_DEPENDENCIES,
        "sensors": {
            name: {
                **vars(spec.sensor),
                "output_routes": sensor_output_routes(spec.sensor, dict(assignments)),
            }
            for name, spec in OPTIONAL_FEATURES.items()
            if spec.sensor is not None and dict(assignments)[name]
        },
        "energy_inventory": {
            "baseline": UPSTREAM_ENERGY_BASELINE,
            "drivers": [vars(entry) for entry in ENERGY_INVENTORY],
        }
        if dict(assignments)["USE_ENERGY"]
        else None,
        "energy": {
            name: vars(spec.energy)
            for name, spec in OPTIONAL_FEATURES.items()
            if spec.energy is not None and dict(assignments)[name]
        },
        "layout": layout.identity if layout is not None else None,
    }
    encoded = json.dumps(identity_input, sort_keys=True, separators=(",", ":")).encode("utf-8")
    identity = hashlib.sha256(encoded).hexdigest()

    directory = work_dir / "config"
    header = directory / "newmota_features.h"
    manifest = directory / "feature_map.json"
    selected_values = dict(assignments)
    if selected_values["USE_WEB"]:
        web_request_body_size = 2048
    elif selected_values["USE_RECOVERY_WEB"] or selected_values["USE_BEKEN_UPDATE_WEB"]:
        web_request_body_size = 1024
    else:
        web_request_body_size = 512
    header_content = "\n".join(
        [
            "/* Generated by tools/newmota/features.py; do not edit. */",
            "#pragma once",
            "",
            *(f"#define {name} {1 if enabled else 0}" for name, enabled in assignments),
            f"#define NEWMOTA_WEB_REQUEST_BODY_SIZE {web_request_body_size}",
            *([f"#define NEWMOTA_PSRAM_MODE {json.dumps(psram_mode)}"] if psram_mode else []),
            *feature_setting_lines(environment.features),
            *(
                [
                    f"#define NEWMOTA_DEVICE_NAME {json.dumps(environment.device_name, ensure_ascii=False)}"
                ]
                if environment.device_name
                else []
            ),
            *template_lines(environment.template),
            *sensor_registry_lines(selected_values),
            *energy_registry_lines(selected_values),
            "",
        ]
    )
    manifest_content = (
        json.dumps({"schema": 1, "identity": identity, **identity_input}, indent=2) + "\n"
    )
    write_if_changed(header, header_content)
    write_if_changed(manifest, manifest_content)
    cmake = [
        "# Generated from the resolved feature requirements; do not edit.",
        *(f"set(NEWMOTA_{name} {'ON' if enabled else 'OFF'})" for name, enabled in assignments),
        "set(NEWMOTA_SOURCES",
        *(f'  "${{NEWMOTA_ROOT}}/{source}"' for source in sources),
        ")",
        "set(NEWMOTA_SDK_REQUIREMENTS " + " ".join(sdk_requirements) + ")",
        "set(NEWMOTA_CRYPTO_INCLUDE_DIRS",
        '  "${NEWMOTA_ROOT}/src/third_party/bearssl/src"',
        '  "${NEWMOTA_ROOT}/src/third_party/bearssl/extensions"',
        ")",
        "set(NEWMOTA_CRYPTO_DEFINES BR_LOMUL=1)",
        "",
    ]
    from newmota.audio import cmake_codec_settings

    cmake.extend(cmake_codec_settings(sources))
    write_if_changed(directory / "requirements.cmake", "\n".join(cmake))
    if selected_values["USE_DISPLAY_FONTS"]:
        from newmota.fonts import generate_fonts

        generate_fonts(selected_values, directory)
    return ResolvedFeatures(
        assignments, header, manifest, identity, sources, sdk_requirements, native_settings
    )
