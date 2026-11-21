/*
  platform.cpp - ESP-IDF platform implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/platform.h"

#include "console.h"
#include "newmota_features.h"
#include "sdkconfig.h"

#include "esp_attr.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_private/esp_clk.h"
#include "esp_app_desc.h"
#include "image_descriptor.h"
#if USE_WIFI && CONFIG_SOC_WIFI_SUPPORTED
#include "esp_phy_init.h"
#include "nvs_flash.h"
#endif
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
#include "esp_psram.h"
#endif
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <array>
#include <cstdlib>
#include <cstring>

#ifndef NEWMOTA_TARGET_NAME
#error "NEWMOTA_TARGET_NAME is required"
#endif

#ifndef NEWMOTA_FRAMEWORK_VERSION
#error "NEWMOTA_FRAMEWORK_VERSION is required"
#endif

#ifndef NEWMOTA_BUILD_DATE
#error "NEWMOTA_BUILD_DATE is required"
#endif

#ifndef NEWMOTA_BUILD_TIME
#error "NEWMOTA_BUILD_TIME is required"
#endif

namespace newmota::platform {
namespace {

constexpr char kExpectedImageSdkTarget[16] = NEWMOTA_IMAGE_SDK_TARGET;
constexpr char kExpectedImageLayoutId[65] = NEWMOTA_IMAGE_LAYOUT_ID;
constexpr std::array<char, 8> kSafebootDescriptorMagic{'N', 'E', 'W', 'M', 'O', 'T', 'A', '\0'};

/** @brief Confirms the installed ESP recovery image belongs to this layout. */
bool compatible_safeboot_partition(const esp_partition_t* const partition) {
    using esp_idf::kCustomDescriptorOffset;
    using esp_idf::NewmotaEspImageDescriptor;
    if (partition == nullptr ||
        partition->size < kCustomDescriptorOffset + sizeof(NewmotaEspImageDescriptor)) {
        return false;
    }

    esp_app_desc_t app_description{};
    if (esp_ota_get_partition_description(partition, &app_description) != ESP_OK ||
        std::memcmp(app_description.project_name, "newmota", 7U) != 0 ||
        app_description.project_name[7] != '\0') {
        return false;
    }

    NewmotaEspImageDescriptor descriptor{};
    if (esp_partition_read(partition, kCustomDescriptorOffset, &descriptor, sizeof(descriptor)) !=
        ESP_OK) {
        return false;
    }
    return std::memcmp(descriptor.magic, kSafebootDescriptorMagic.data(),
               kSafebootDescriptorMagic.size()) == 0 &&
           descriptor.schema == esp_idf::kCustomDescriptorSchema &&
           descriptor.descriptor_size == sizeof(descriptor) &&
           descriptor.image_role == esp_idf::kSafebootImageRole &&
           descriptor.app0_offset == NEWMOTA_IMAGE_APP0_OFFSET &&
           descriptor.safeboot_offset == NEWMOTA_IMAGE_SAFEBOOT_OFFSET &&
           descriptor.otadata_offset == NEWMOTA_IMAGE_OTADATA_OFFSET &&
           descriptor.nvs_offset == NEWMOTA_IMAGE_NVS_OFFSET &&
           std::memcmp(descriptor.sdk_target, kExpectedImageSdkTarget,
               sizeof(descriptor.sdk_target)) == 0 &&
           std::memcmp(
               descriptor.layout_id, kExpectedImageLayoutId, sizeof(descriptor.layout_id)) == 0 &&
           descriptor.psram_bytes == NEWMOTA_IMAGE_PSRAM_BYTES;
}

/**
 * @brief Accepts a foreign recovery image the operator asked to enter.
 *
 * Migrated devices keep Tasmota's Safeboot. It is not identified by name: ESP-IDF's image
 * check (magic, chip, segment bounds, checksum, appended SHA-256) must accept it.
 */
bool verified_recovery_image(const esp_partition_t* const partition) {
    esp_app_desc_t description{};
    if (partition == nullptr ||
        esp_ota_get_partition_description(partition, &description) != ESP_OK) {
        return false;
    }
    const esp_partition_pos_t position{partition->address, partition->size};
    esp_image_metadata_t metadata{};
    return esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata) == ESP_OK;
}

#if CONFIG_IDF_TARGET_ESP32C2 && defined(__NOINIT_ATTR)
#define NEWMOTA_FAST_RESTART_ATTR __NOINIT_ATTR
#define NEWMOTA_HAS_RETAINED_FAST_RESTART 1
#elif CONFIG_SOC_RTC_FAST_MEM_SUPPORTED || CONFIG_SOC_RTC_SLOW_MEM_SUPPORTED
#define NEWMOTA_FAST_RESTART_ATTR RTC_NOINIT_ATTR
#define NEWMOTA_HAS_RETAINED_FAST_RESTART 1
#else
#define NEWMOTA_HAS_RETAINED_FAST_RESTART 0
#endif

#if NEWMOTA_HAS_RETAINED_FAST_RESTART
constexpr std::uint32_t kFastRestartMagic = 0x4e4d4652U;

struct RetainedFastRestart final {
    std::uint32_t magic;
    std::uint8_t count;
    std::uint8_t inverse;
    std::uint16_t reserved;
};

/**
 * @brief Keeps the boot count outside startup initialization.
 * C2 uses ordinary no-init RAM like Tasmota, not RTC memory. Power-on, brownout and
 * deep-sleep wakes discard the record; warm-reset retention needs board qualification.
 */
NEWMOTA_FAST_RESTART_ATTR RetainedFastRestart retained_fast_restart;

/** @brief Validates the retained fast-restart record against stale RAM data. */
[[nodiscard]] bool ValidFastRestartRecord() {
    return retained_fast_restart.magic == kFastRestartMagic &&
           retained_fast_restart.inverse == static_cast<std::uint8_t>(~retained_fast_restart.count);
}

/** @brief Replaces the retained fast-restart count and its integrity byte. */
void StoreFastRestartCount(const std::uint8_t count) {
    retained_fast_restart.magic = kFastRestartMagic;
    retained_fast_restart.count = count;
    retained_fast_restart.inverse = static_cast<std::uint8_t>(~count);
    retained_fast_restart.reserved = 0U;
}
#endif

} // namespace

/** @brief Returns immutable identity for the active ESP-IDF environment. */
const PlatformInfo& info() {
    static const PlatformInfo information{
        NEWMOTA_TARGET_NAME,
        CONFIG_IDF_TARGET,
        "ESP-IDF",
        esp_get_idf_version(),
        NEWMOTA_FRAMEWORK_VERSION,
        NEWMOTA_BUILD_DATE " " NEWMOTA_BUILD_TIME,
    };
    return information;
}

/** @brief Reports the running CPU clock and the flash chip and boot settings. */
HardwareInfo hardware_info() {
    HardwareInfo hardware{};
    hardware.cpu_frequency_mhz = static_cast<std::uint32_t>(esp_clk_cpu_freq() / 1000000);
    std::uint32_t chip_id = 0U;
    if (esp_flash_read_id(esp_flash_default_chip, &chip_id) == ESP_OK)
        hardware.flash_chip_id = chip_id;
    hardware.flash_frequency_mhz =
        static_cast<std::uint32_t>(std::atoi(CONFIG_ESPTOOLPY_FLASHFREQ));
#if CONFIG_ESPTOOLPY_FLASHMODE_QIO
    hardware.flash_mode = "QIO";
#elif CONFIG_ESPTOOLPY_FLASHMODE_QOUT
    hardware.flash_mode = "QOUT";
#elif CONFIG_ESPTOOLPY_FLASHMODE_DIO
    hardware.flash_mode = "DIO";
#elif CONFIG_ESPTOOLPY_FLASHMODE_DOUT
    hardware.flash_mode = "DOUT";
#elif CONFIG_ESPTOOLPY_FLASHMODE_OPI
    hardware.flash_mode = "OPI";
#endif
    return hardware;
}

/** @brief Collects available ESP-IDF program, flash, heap, stack, and PSRAM data. */
MemoryInfo memory_info() {
    MemoryInfo memory{};

    memory.heap_free_bytes =
        static_cast<std::uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    memory.heap_available = true;
    memory.stack_low_watermark_bytes =
        static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    memory.stack_available = true;

    const auto* running = esp_ota_get_running_partition();
    if (running != nullptr) {
        const esp_partition_pos_t position{running->address, running->size};
        esp_image_metadata_t metadata{};
        if (esp_image_get_metadata(&position, &metadata) == ESP_OK &&
            metadata.image_len <= running->size) {
            memory.program_size_bytes = metadata.image_len;
            memory.program_capacity_bytes = running->size;
            memory.program_available = true;
        }
    }

    std::uint32_t flash_size = 0;
    if (esp_flash_get_size(esp_flash_default_chip, &flash_size) == ESP_OK) {
        memory.flash_size_bytes = flash_size;
        memory.flash_available = true;
    }

#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
    if (esp_psram_is_initialized()) {
        const auto total = esp_psram_get_size();
        if (total != 0) {
            memory.psram_total_bytes = static_cast<std::uint32_t>(total);
            memory.psram_free_bytes = static_cast<std::uint32_t>(
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            memory.psram_available = true;
        }
    }
#endif

    return memory;
}

/** @brief Returns the low 32 bits of ESP-IDF monotonic milliseconds. */
std::uint32_t monotonic_milliseconds() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

/** @brief Recognizes the ESP-IDF power-on reset used by PowerOnState. */
bool reset_was_power_on() {
    return esp_reset_reason() == ESP_RST_POWERON;
}

/** @brief Groups ESP-IDF's reset reasons into the reported ones. */
RestartReason restart_reason() {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return RestartReason::power_on;
    case ESP_RST_SW:
        return RestartReason::software;
    case ESP_RST_PANIC:
    case ESP_RST_CPU_LOCKUP:
        return RestartReason::panic;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return RestartReason::watchdog;
    case ESP_RST_BROWNOUT:
    case ESP_RST_PWR_GLITCH:
        return RestartReason::brownout;
    case ESP_RST_DEEPSLEEP:
        return RestartReason::deep_sleep;
    case ESP_RST_EXT:
    case ESP_RST_SDIO:
    case ESP_RST_USB:
    case ESP_RST_JTAG:
        return RestartReason::external;
    default:
        return RestartReason::unknown;
    }
}

/** @brief Tracks ESP fast restarts in no-init memory when available. */
FastRestartInfo begin_fast_restart_tracking() {
#if NEWMOTA_HAS_RETAINED_FAST_RESTART
    const auto reason = esp_reset_reason();
    const bool fresh_boot =
        reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT || reason == ESP_RST_DEEPSLEEP;
    std::uint8_t count = 1U;
    if (!fresh_boot && ValidFastRestartRecord()) {
        count = retained_fast_restart.count == UINT8_MAX
                    ? UINT8_MAX
                    : static_cast<std::uint8_t>(retained_fast_restart.count + 1U);
    }
    StoreFastRestartCount(count);
    return {count, true};
#else
    // Without supported no-init storage, keep autoexec.be disabled.
    return {};
#endif
}

/** @brief Clears the ESP no-init fast-restart count after stable execution. */
void mark_boot_stable() {
#if NEWMOTA_HAS_RETAINED_FAST_RESTART
    StoreFastRestartCount(0U);
#endif
}

/** @brief Delays the current FreeRTOS task for at least one scheduler tick. */
/** @brief Uses the native SDK busy wait for peripheral edge timing. */
void delay_microseconds(const std::uint32_t duration) {
    esp_rom_delay_us(duration);
}

void sleep_for_milliseconds(const std::uint32_t duration) {
    auto ticks = pdMS_TO_TICKS(duration);
    if (ticks == 0) {
        ticks = 1;
    }
    vTaskDelay(ticks);
}

/** @brief Provides the platform execution-yield opportunity. */
void yield_execution() {
    vTaskDelay(0);
}

/** @brief Does nothing: no topology here lets a configuration take console pins. */
bool release_console() {
    return false;
}

/** @brief Initializes the ESP-IDF runtime console. */
void initialize_console() {
    console().initialize();
}

/** @brief Describes the active ESP-IDF console backend. */
const char* console_status() {
    return console().status();
}

/** @brief Returns the active UART rate, or zero for USB consoles. */
std::uint32_t console_baudrate() {
    return console().baudrate();
}

/** @brief Sends an application log line through the internal console sink. */
void write_log(const std::string_view message) {
    (void)console().write(message);
    (void)console().write("\r\n");
}

/** @brief Reads one character without blocking from the active console. */
bool read_console_character(char& character) {
    return console().read_character(character);
}

/** @brief Writes command response text through the active console. */
bool write_console(const std::string_view text) {
    return console().write(text);
}

/**
 * @brief Selects the opposite safeboot or application partition for next boot.
 */
BootPartitionSwitchResult prepare_boot_partition_toggle(const AlternateImage image) {
    if (esp_ota_get_app_partition_count() != 1) {
        return BootPartitionSwitchResult::unavailable;
    }

    const auto* safeboot = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "safeboot");
    const auto* application =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "app0");
    const auto* ota_data = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata");
    const auto* running = esp_ota_get_running_partition();
    if (safeboot == nullptr || application == nullptr || ota_data == nullptr ||
        running == nullptr) {
        return BootPartitionSwitchResult::unavailable;
    }

    if (running->address == application->address &&
        std::strcmp(running->label, application->label) == 0) {
        if (!compatible_safeboot_partition(safeboot) &&
            (image != AlternateImage::any_recovery || !verified_recovery_image(safeboot))) {
            return BootPartitionSwitchResult::unavailable;
        }
        return esp_partition_erase_range(ota_data, 0, ota_data->size) == ESP_OK
                   ? BootPartitionSwitchResult::prepared
                   : BootPartitionSwitchResult::failed;
    }
    if (running->address == safeboot->address &&
        std::strcmp(running->label, safeboot->label) == 0) {
        return esp_ota_set_boot_partition(application) == ESP_OK
                   ? BootPartitionSwitchResult::prepared
                   : BootPartitionSwitchResult::failed;
    }
    return BootPartitionSwitchResult::unavailable;
}

/** @brief Erases ESP PHY calibration so the next boot performs full calibration. */
RadioCalibrationResetResult erase_radio_calibration() {
#if USE_WIFI && CONFIG_SOC_WIFI_SUPPORTED
    if (nvs_flash_init() != ESP_OK) {
        return RadioCalibrationResetResult::failed;
    }
    return esp_phy_erase_cal_data_in_nvs() == ESP_OK ? RadioCalibrationResetResult::erased
                                                     : RadioCalibrationResetResult::failed;
#else
    return RadioCalibrationResetResult::unavailable;
#endif
}

/** @brief Restarts the ESP chip immediately. */
[[noreturn]] void restart() {
    esp_restart();
}

} // namespace newmota::platform
