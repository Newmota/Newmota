/*
  platform.h - portable platform interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstdint>
#include <string_view>

namespace newmota::platform {

enum class BootPartitionSwitchResult : std::uint8_t {
    prepared,
    unavailable,
    failed,
};

/** @brief Which recovery image an alternate-image switch may enter. */
enum class AlternateImage : std::uint8_t {
    /** Only the paired Newmota recovery image; required for automatic update handoff. */
    paired,
    /** Also a verified foreign recovery image the operator chose, such as Tasmota Safeboot. */
    any_recovery,
};

enum class RadioCalibrationResetResult : std::uint8_t {
    erased,
    unavailable,
    failed,
};

/**
 * @brief Native memory snapshot; every quantity is expressed in bytes.
 *
 * Availability flags gate fields that cannot be measured for the active image
 * or hardware. A zero measurement is still valid when its flag is true.
 */
struct MemoryInfo final {
    std::uint32_t program_size_bytes{};
    std::uint32_t program_capacity_bytes{};
    std::uint32_t flash_size_bytes{};
    std::uint32_t heap_free_bytes{};
    std::uint32_t stack_low_watermark_bytes{};
    std::uint32_t psram_total_bytes{};
    std::uint32_t psram_free_bytes{};
    bool program_available{};
    bool flash_available{};
    bool heap_available{};
    bool stack_available{};
    bool psram_available{};
};

struct FastRestartInfo final {
    std::uint8_t count{};
    bool available{};
};

/** @brief Why the device last started, as far as the platform can tell. */
enum class RestartReason : std::uint8_t {
    unknown,
    power_on,
    software,
    watchdog,
    panic,
    brownout,
    deep_sleep,
    external,
};

/** @brief Returns the reported name of a reason, or nullptr when it is unknown. */
[[nodiscard]] constexpr const char* restart_reason_name(const RestartReason reason) {
    switch (reason) {
    case RestartReason::power_on:
        return "Power On";
    case RestartReason::software:
        return "Software";
    case RestartReason::watchdog:
        return "Watchdog";
    case RestartReason::panic:
        return "Panic";
    case RestartReason::brownout:
        return "Brownout";
    case RestartReason::deep_sleep:
        return "Deep Sleep";
    case RestartReason::external:
        return "External";
    case RestartReason::unknown:
        break;
    }
    return nullptr;
}

struct HardwareInfo final {
    std::uint32_t cpu_frequency_mhz{};
    std::uint32_t flash_chip_id{};
    std::uint32_t flash_frequency_mhz{};
    const char* flash_mode{};
};

class PlatformInfo final {
  public:
    /**
     * @brief Constructs immutable platform identity from static strings.
     */
    constexpr PlatformInfo(const char* target, const char* chip, const char* sdk_name,
        const char* sdk_version, const char* framework_version, const char* build_datetime)
        : target_(target), chip_(chip), sdk_name_(sdk_name), sdk_version_(sdk_version),
          framework_version_(framework_version), build_datetime_(build_datetime) {}

    /** @brief Returns the selected environment name. */
    [[nodiscard]] constexpr const char* target() const { return target_; }

    /** @brief Returns the native SDK chip target. */
    [[nodiscard]] constexpr const char* chip() const { return chip_; }

    /** @brief Returns the platform SDK name. */
    [[nodiscard]] constexpr const char* sdk_name() const { return sdk_name_; }

    /** @brief Returns the platform SDK version. */
    [[nodiscard]] constexpr const char* sdk_version() const { return sdk_version_; }

    /** @brief Returns the Newmota firmware version. */
    [[nodiscard]] constexpr const char* framework_version() const { return framework_version_; }

    /** @brief Returns the build date and time in YYYY.MM.DD HH:MM:SS form. */
    [[nodiscard]] constexpr const char* build_datetime() const { return build_datetime_; }

  private:
    const char* target_;
    const char* chip_;
    const char* sdk_name_;
    const char* sdk_version_;
    const char* framework_version_;
    const char* build_datetime_;
};

/** @brief Returns immutable identity for the running target. */
[[nodiscard]] const PlatformInfo& info();
/** @brief Collects the memory measurements supported by the platform. */
[[nodiscard]] MemoryInfo memory_info();
/**
 * @brief Collects clock and flash identity; zero or null marks what the platform cannot report.
 * Reads the flash chip, so it serves on-demand status rather than frequent snapshots.
 */
[[nodiscard]] HardwareInfo hardware_info();
/** @brief Returns a wrapping monotonic millisecond counter. */
[[nodiscard]] std::uint32_t monotonic_milliseconds();
/** @brief Reports whether startup followed a power-on-equivalent reset. */
[[nodiscard]] bool reset_was_power_on();
/** @brief Reports why the device last started, for diagnostics. */
[[nodiscard]] RestartReason restart_reason();
/**
 * @brief Increments and returns retained consecutive fast-restart state.
 * @return The counter and whether the platform provides qualified retention.
 */
[[nodiscard]] FastRestartInfo begin_fast_restart_tracking();
/** @brief Clears retained fast-restart state after a stable boot window. */
void mark_boot_stable();
/** @brief Suspends cooperatively for at least @p duration milliseconds. */
void sleep_for_milliseconds(std::uint32_t duration);
/** @brief Busy waits for a bounded peripheral timing interval. */
void delay_microseconds(std::uint32_t duration);
/** @brief Yields to ready equal-priority RTOS tasks, or sleeps 1 ms on Pico. */
void yield_execution();
/** @brief Selects and initializes the runtime console backend. */
void initialize_console();
/**
 * @brief Stops the console so a configuration can use its pins; it stays off until restart.
 * @return true when an active console was stopped by this call.
 */
bool release_console();
/** @brief Describes the active runtime console backend. */
[[nodiscard]] const char* console_status();
/**
 * @brief Returns the active UART console baud rate, or zero for USB/unknown.
 */
[[nodiscard]] std::uint32_t console_baudrate();
/** @brief Writes one already-formatted log message. */
void write_log(std::string_view message);
/** @brief Reads one console character without blocking. */
[[nodiscard]] bool read_console_character(char& character);
/** @brief Writes response text to the active console. */
[[nodiscard]] bool write_console(std::string_view text);
/** @brief Prepares the alternate application partition for the next boot. */
[[nodiscard]] BootPartitionSwitchResult prepare_boot_partition_toggle(AlternateImage image);
/** @brief Erases persisted radio calibration for regeneration on next boot. */
[[nodiscard]] RadioCalibrationResetResult erase_radio_calibration();
/** @brief Restarts the platform and does not return. */
[[noreturn]] void restart();

} // namespace newmota::platform
