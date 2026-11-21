/*
  platform.cpp - Beken platform implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/platform.h"
#include "newmota/platform/gpio_detail.h"
#include "newmota/platform/beken/update_config.h"
#include "image_size.h"

#include <cstdint>
#include <string_view>

extern "C" {
#include "BkDriverFlash.h"
#include "drv_model_pub.h"
#include "flash_pub.h"
#include "FreeRTOS.h"
#include "rtos_pub.h"
#include "target_util_pub.h"
#include "start_type_pub.h"
#include "task.h"
#include "uart_pub.h"
}

extern "C" {
extern char _vector_start[];
extern char _data_flash_begin[];
extern char _data_ram_begin[];
extern char _data_ram_end[];
}

#if defined(CFG_SARADC_VERIFY) && CFG_SARADC_VERIFY
#define NEWMOTA_BEKEN_DEFAULT_BAUDRATE 921600
#else
#define NEWMOTA_BEKEN_DEFAULT_BAUDRATE 115200
#endif

#ifndef UART1_BAUD_RATE
#define UART1_BAUD_RATE NEWMOTA_BEKEN_DEFAULT_BAUDRATE
#endif
#ifndef UART2_BAUD_RATE
#define UART2_BAUD_RATE NEWMOTA_BEKEN_DEFAULT_BAUDRATE
#endif

/** @brief Invokes the Beken SDK restart primitive. */
extern "C" void bk_reboot(void);

/** @brief Silence prebuilt BDK bk_printf calls at SDK_LOG_LEVEL=none. */
extern "C" void __wrap_bk_printf(const char*, ...) {}

#ifndef NEWMOTA_TARGET_NAME
#error "NEWMOTA_TARGET_NAME is required"
#endif

#ifndef NEWMOTA_CHIP_NAME
#error "NEWMOTA_CHIP_NAME is required"
#endif

#ifndef NEWMOTA_SDK_VERSION
#error "NEWMOTA_SDK_VERSION is required"
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

DD_HANDLE console_handle = DD_HANDLE_UNVALID;
int console_port = -1;
bool console_released{};

constexpr std::uint32_t kFastRestartMagic = 0x4e4d0000U;

/**
 * @brief Candidate warm-reset record, outside BSS initialization and the SDK heap.
 * One aligned word contains magic, inverse count and count, avoiding a torn pair.
 * The bootloader's treatment of this SRAM still needs qualification on each chip.
 */
__attribute__((
    section(".noinit.newmota_fast_restart"), used)) volatile std::uint32_t retained_fast_restart;

/** @brief Rejects uninitialized or damaged SRAM before reusing its count. */
[[nodiscard]] bool valid_fast_restart_record(const std::uint32_t record) {
    const auto count = static_cast<std::uint8_t>(record);
    const auto inverse = static_cast<std::uint8_t>(record >> 8U);
    return (record & 0xffff0000U) == kFastRestartMagic &&
           inverse == static_cast<std::uint8_t>(~count);
}

/** @brief Replaces the complete retained record in one aligned SRAM write. */
void store_fast_restart_count(const std::uint8_t count) {
    retained_fast_restart = kFastRestartMagic |
                            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(~count)) << 8U) |
                            count;
}

bool write_uart(const std::string_view text) {
    if (console_released) {
        return true;
    }
    bool success = true;
    for (const char character : text) {
        if (uart_write_byte(uart_print_port, character) < 0) {
            success = false;
        }
    }
    return success;
}

constexpr PlatformInfo kInformation{
    NEWMOTA_TARGET_NAME,
    NEWMOTA_CHIP_NAME,
    "framework-beken-bdk",
    NEWMOTA_SDK_VERSION,
    NEWMOTA_FRAMEWORK_VERSION,
    NEWMOTA_BUILD_DATE " " NEWMOTA_BUILD_TIME,
};

} // namespace

/** @brief Returns immutable identity for the active Beken environment. */
const PlatformInfo& info() {
    return kInformation;
}

/** @brief Reports nothing yet; Status omits clock and flash identity on Beken. */
HardwareInfo hardware_info() {
    return {};
}

/** @brief Collects the running Beken image, heap, stack, and flash measurements. */
MemoryInfo memory_info() {
    MemoryInfo memory{};
    memory.heap_free_bytes = static_cast<std::uint32_t>(xPortGetFreeHeapSize());
    memory.heap_available = true;

    memory.stack_low_watermark_bytes =
        static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) *
        static_cast<std::uint32_t>(sizeof(StackType_t));
    memory.stack_available = true;

    const auto* const application = bk_flash_get_info(BK_PARTITION_APPLICATION);
    if (application != nullptr && application->partition_owner == BK_FLASH_EMBEDDED &&
        application->partition_start_addr == beken::update_config::kApplication.offset &&
        application->partition_length >= beken::update_config::kApplicationRawLimit) {
        if (beken::image_size_from_linker_bounds(reinterpret_cast<std::uintptr_t>(_vector_start),
                reinterpret_cast<std::uintptr_t>(_data_flash_begin),
                reinterpret_cast<std::uintptr_t>(_data_ram_begin),
                reinterpret_cast<std::uintptr_t>(_data_ram_end),
                beken::update_config::kApplicationRawLimit, memory.program_size_bytes)) {
            memory.program_capacity_bytes = beken::update_config::kApplicationRawLimit;
            memory.program_available = true;
        }
    }

    DD_HANDLE flash_handle = DD_HANDLE_UNVALID;
    UINT32 open_status = DRV_FAILURE;
    flash_handle = ddev_open(const_cast<char*>(FLASH_DEV_NAME), &open_status, 0U);
    if (flash_handle != DD_HANDLE_UNVALID) {
        UINT32 flash_size = 0U;
        const auto size_status = ddev_control(flash_handle, CMD_FLASH_GET_SIZE, &flash_size);
        (void)ddev_close(flash_handle);
        if (size_status == DRV_SUCCESS && flash_size != 0U) {
            memory.flash_size_bytes = flash_size;
            memory.flash_available = true;
        }
    }
    return memory;
}

/** @brief Returns the Beken RTOS monotonic millisecond count. */
std::uint32_t monotonic_milliseconds() {
    return rtos_get_time();
}

/** @brief Recognizes the Beken SDK's retained power-on reset source. */
bool reset_was_power_on() {
    return bk_misc_get_start_type() == RESET_SOURCE_POWERON;
}

/** @brief Groups the Beken SDK's retained reset sources into the reported reasons. */
RestartReason restart_reason() {
    switch (bk_misc_get_start_type()) {
    case RESET_SOURCE_POWERON:
        return RestartReason::power_on;
    case RESET_SOURCE_REBOOT:
        return RestartReason::software;
    case RESET_SOURCE_WATCHDOG:
        return RestartReason::watchdog;
    case RESET_SOURCE_DEEPPS_GPIO:
    case RESET_SOURCE_DEEPPS_RTC:
    case RESET_SOURCE_DEEPPS_USB:
        return RestartReason::deep_sleep;
    case RESET_SOURCE_CRASH_XAT0:
    case RESET_SOURCE_CRASH_UNDEFINED:
    case RESET_SOURCE_CRASH_PREFETCH_ABORT:
    case RESET_SOURCE_CRASH_DATA_ABORT:
    case RESET_SOURCE_CRASH_UNUSED:
    case RESET_SOURCE_CRASH_PER_XAT0:
        return RestartReason::panic;
    default:
        return RestartReason::unknown;
    }
}

/**
 * @brief Counts candidate warm resets without enabling unqualified boot protection.
 * Only software, watchdog and panic resets continue a valid record. Every other
 * cause starts at one. The count remains diagnostic until SRAM retention through
 * the bootloader has been qualified; availability stays false so autoexec fails closed.
 */
FastRestartInfo begin_fast_restart_tracking() {
    const auto reason = restart_reason();
    const bool warm_reset = reason == RestartReason::software ||
                            reason == RestartReason::watchdog || reason == RestartReason::panic;
    const std::uint32_t record = retained_fast_restart;
    std::uint8_t count = 1U;
    if (warm_reset && valid_fast_restart_record(record)) {
        const auto previous = static_cast<std::uint8_t>(record);
        count = previous == UINT8_MAX ? UINT8_MAX : static_cast<std::uint8_t>(previous + 1U);
    }
    store_fast_restart_count(count);
    return {count, false};
}

/** @brief Clears the candidate count after the shared ten-second stable window. */
void mark_boot_stable() {
    store_fast_restart_count(0U);
}

/** @brief Delays the current Beken RTOS task for @p duration milliseconds. */
/** @brief Uses the native SDK busy wait for peripheral edge timing. */
void delay_microseconds(const std::uint32_t duration) {
    delay_us(duration);
}

void sleep_for_milliseconds(const std::uint32_t duration) {
    (void)rtos_delay_milliseconds(duration);
}

/** @brief Provides the platform execution-yield opportunity. */
void yield_execution() {
    vTaskDelay(0);
}

/** @brief Opens the SDK log UART's interrupt-fed software receive FIFO. */
void initialize_console() {
    if (console_handle != DD_HANDLE_UNVALID) {
        return;
    }
    // bk_printf uses uart_print_port. Follow that SDK choice for RX too;
    // both currently supported BDK targets select UART1, despite the UART2
    // "debug purpose" label in the vendor header.
    console_port = uart_print_port;
    const char* device = console_port == UART1_PORT   ? UART1_DEV_NAME
                         : console_port == UART2_PORT ? UART2_DEV_NAME
                                                      : nullptr;
    if (device == nullptr) {
        return;
    }
    UINT32 status = DRV_FAILURE;
    console_handle = ddev_open(const_cast<char*>(device), &status, 0);
}

/** @brief Stops console input and output; GPIO setup then takes the UART pads. */
bool release_console() {
    if (console_released) {
        return false;
    }
    // The pad's GPIO configuration clears its UART function. The transmitter
    // stays enabled because the SDK's byte writer waits on it.
    if (console_handle != DD_HANDLE_UNVALID) {
        (void)ddev_close(console_handle);
        console_handle = DD_HANDLE_UNVALID;
    }
    console_port = -1;
    console_released = true;
    detail::release_console_pins();
    return true;
}

bool beken_console_released() {
    return console_released;
}

/** @brief Describes the selected Beken console and its input capability. */
const char* console_status() {
    if (console_released) {
        return "Beken console off, the template uses its pins";
    }
    if (console_handle == DD_HANDLE_UNVALID) {
        return "Beken console output only";
    }
    return console_port == UART1_PORT ? "Using Beken UART1 console" : "Using Beken UART2 console";
}

/** @brief Returns the BDK-configured rate of the selected log UART. */
std::uint32_t console_baudrate() {
    if (console_port == UART1_PORT) {
        return static_cast<std::uint32_t>(UART1_BAUD_RATE);
    }
    if (console_port == UART2_PORT) {
        return static_cast<std::uint32_t>(UART2_BAUD_RATE);
    }
#if defined(UART3_PORT) && defined(UART3_BAUD_RATE)
    if (console_port == UART3_PORT) {
        return static_cast<std::uint32_t>(UART3_BAUD_RATE);
    }
#endif
    return 0U;
}

/** @brief Writes one application log line through the Beken console. */
void write_log(const std::string_view message) {
    (void)write_uart(message);
    (void)write_uart("\r\n");
}

/** @brief Reads one byte nonblocking from the SDK's software receive FIFO. */
bool read_console_character(char& character) {
    // The UART ISR already drains the hardware FIFO into kfifo. ddev_read
    // consumes that buffer and returns zero immediately when it is empty.
    return console_handle != DD_HANDLE_UNVALID && ddev_read(console_handle, &character, 1, 0) == 1;
}

/** @brief Writes command response text through the Beken console. */
bool write_console(const std::string_view text) {
    return write_uart(text);
}

/** @brief Reports unavailable until a Beken safeboot layout is qualified. */
BootPartitionSwitchResult prepare_boot_partition_toggle(AlternateImage) {
    return BootPartitionSwitchResult::unavailable;
}

/** @brief Reports unavailable until Beken radio calibration storage is qualified. */
RadioCalibrationResetResult erase_radio_calibration() {
    return RadioCalibrationResetResult::unavailable;
}

/** @brief Restarts the Beken system and does not return. */
[[noreturn]] void restart() {
    bk_reboot();
    while (true) {}
}

} // namespace newmota::platform
