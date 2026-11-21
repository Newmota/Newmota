/*
  platform.cpp - Bouffalo platform implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file platform.cpp
 * @brief Native identity, memory reporting and boot partition control.
 */

#include "newmota/platform/platform.h"

#include "bflb_mtimer.h"

extern "C" {
#include "bflb_boot2.h"
#include "bflb_flash.h"
#include "bflb_irq.h"
#include "bflb_uart.h"
#include "FreeRTOS.h"
#include "mm.h"
#include "partition.h"
#include "task.h"
#if defined(BL602)
#include "bl602_hbn.h"
#elif defined(BL616)
#include "bl616_hbn.h"
#elif defined(BL618DG)
#include "bl618dg_hbn.h"
#elif defined(BL702)
#include "bl702_hbn.h"
#endif
}

#include "support/crc32.h"
#include "boot_header.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

/** @brief Invokes the BouffaloSDK power-on-reset primitive. */
extern "C" int bl_sys_reset_por(void);

/**
 * @brief Receives the printf calls of the precompiled Wi-Fi6 host and MAC libraries.
 *
 * The native CMake links copies of those libraries with printf redirected here. Their
 * diagnostics include Wi-Fi credentials, so they are dropped.
 */
extern "C" int newmota_sdk_printf(const char*, ...) {
    return 0;
}

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

#ifndef CONFIG_CONSOLE_UART_BAUDRATE
#error "CONFIG_CONSOLE_UART_BAUDRATE is required"
#endif

#ifndef NEWMOTA_APPLICATION_OFFSET
#error "NEWMOTA_APPLICATION_OFFSET is required"
#endif

#ifndef NEWMOTA_APPLICATION_SIZE
#error "NEWMOTA_APPLICATION_SIZE is required"
#endif

#ifndef NEWMOTA_SAFEBOOT_OFFSET
#error "NEWMOTA_SAFEBOOT_OFFSET is required"
#endif

#ifndef NEWMOTA_SAFEBOOT_SIZE
#error "NEWMOTA_SAFEBOOT_SIZE is required"
#endif

namespace newmota::platform {
namespace {

constexpr PlatformInfo kInformation{
    NEWMOTA_TARGET_NAME,
    NEWMOTA_CHIP_NAME,
    "BouffaloSDK",
    NEWMOTA_SDK_VERSION,
    NEWMOTA_FRAMEWORK_VERSION,
    NEWMOTA_BUILD_DATE " " NEWMOTA_BUILD_TIME,
};

/** @brief Native BouffaloSDK name of the dual-slot firmware table entry. */
constexpr char kFirmwareEntryName[] = "FW";
constexpr std::size_t kBootHeaderSize = boot_header::size;
constexpr std::uint32_t kImageSectorSize = 4096U;
constexpr std::uint32_t kBootHeaderMagic = 0x504E4642U;
constexpr std::uint32_t kBootHeaderImageLengthOffset = boot_header::length_offset;
constexpr std::uint32_t kBootHeaderCrcOffset = boot_header::crc_offset;
constexpr std::uint32_t kFastRestartMagic = 0x4e4d0000U;

/**
 * @brief Candidate restart record in the SDK's no-init SRAM, outside BSS and heap.
 * One aligned word contains magic, inverse count and count, avoiding a torn pair.
 * Startup exclusion does not qualify bootloader preservation or power-cycle freshness.
 */
__attribute__((section(".noinit_data.newmota_fast_restart"), aligned(4),
    used)) volatile std::uint32_t retained_fast_restart;

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

// The UART0 RX FIFO holds 32 bytes, about 0.2 ms at 2 Mbaud. The ISR drains
// it into this single-producer, single-consumer ring so pasted lines survive.
constexpr std::uint32_t kConsoleRxCapacity = 1024U;
static_assert((kConsoleRxCapacity & (kConsoleRxCapacity - 1U)) == 0U);
std::array<char, kConsoleRxCapacity> g_console_rx{};
std::atomic<std::uint32_t> g_console_rx_head{};
std::atomic<std::uint32_t> g_console_rx_tail{};
bflb_device_s* g_console_uart = nullptr;
bool g_console_rx_interrupt = false;

/** @brief Moves every pending UART0 byte into the console ring; drops bytes when full. */
void console_rx_isr(int, void*) {
    auto* const device = g_console_uart;
    if ((bflb_uart_get_intstatus(device) & UART_INTSTS_RTO) != 0U) {
        bflb_uart_int_clear(device, UART_INTCLR_RTO);
    }
    auto head = g_console_rx_head.load(std::memory_order_relaxed);
    const auto tail = g_console_rx_tail.load(std::memory_order_acquire);
    while (bflb_uart_rxavailable(device)) {
        const int value = bflb_uart_getchar(device);
        if (value < 0) {
            break;
        }
        if (head - tail < kConsoleRxCapacity) {
            g_console_rx[head % kConsoleRxCapacity] = static_cast<char>(value);
            ++head;
        }
    }
    g_console_rx_head.store(head, std::memory_order_release);
}

/** @brief Reads a little-endian 32-bit value from a BL602 boot header. */
[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* const bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

/** @brief Program measurements; read once, the FW table only changes before a restart. */
MemoryInfo g_program{};
/** @brief 0 unread, 1 being read by one task, 2 ready. */
std::atomic<int> g_program_state{0};

/** @brief Measures the image and slot selected by the active Bouffalo FW table. */
void collect_program_info(MemoryInfo& memory) {
    if (bflb_boot2_init() != 0) {
        return;
    }
    auto* const config = bflb_boot2_get_pt_config();
    pt_table_entry_config firmware{};
    if (config == nullptr ||
        pt_table_get_active_entries_by_name(config,
            reinterpret_cast<std::uint8_t*>(const_cast<char*>(kFirmwareEntryName)),
            &firmware) != PT_ERROR_SUCCESS) {
        return;
    }

    const auto slot = static_cast<std::uint8_t>(firmware.active_index & 0x01U);
    const auto capacity = firmware.max_len[slot];
    const auto address = firmware.start_address[slot];
    const auto expected_address = static_cast<std::uint64_t>(
        slot == 0U ? NEWMOTA_APPLICATION_OFFSET : NEWMOTA_SAFEBOOT_OFFSET);
    const auto expected_capacity =
        static_cast<std::uint64_t>(slot == 0U ? NEWMOTA_APPLICATION_SIZE : NEWMOTA_SAFEBOOT_SIZE);
    if (firmware.type != PT_ENTRY_FW_CPU0 || (firmware.active_index & 0xFEU) != 0U ||
        address != expected_address || capacity != expected_capacity ||
        capacity <= kImageSectorSize) {
        return;
    }

    std::array<std::uint8_t, kBootHeaderSize> header{};
    if (bflb_flash_read(address, header.data(), static_cast<std::uint32_t>(header.size())) != 0 ||
        read_u32(header.data()) != kBootHeaderMagic) {
        return;
    }
    const auto expected_crc = read_u32(header.data() + kBootHeaderCrcOffset);
    const auto actual_crc = support::crc32_finalize(
        support::crc32_update(support::kCrc32InitialState, header.data(), kBootHeaderCrcOffset));
    const auto image_length = read_u32(header.data() + kBootHeaderImageLengthOffset);
    const auto image_size = static_cast<std::uint64_t>(kImageSectorSize) + image_length;
    if (expected_crc != actual_crc || image_length == 0U || image_size > capacity) {
        return;
    }

    memory.program_size_bytes = static_cast<std::uint32_t>(image_size);
    memory.program_capacity_bytes = capacity;
    memory.program_available = true;
}

} // namespace

/** @brief Returns immutable identity for the active Bouffalo environment. */
const PlatformInfo& info() {
    return kInformation;
}

/** @brief Reports nothing yet; Status omits clock and flash identity on Bouffalo. */
HardwareInfo hardware_info() {
    return {};
}

/** @brief Collects the running Bouffalo image, heap, stack, and flash measurements. */
MemoryInfo memory_info() {
    MemoryInfo memory{};
    memory.heap_free_bytes = static_cast<std::uint32_t>(kfree_size(0U));
    memory.heap_available = true;
    memory.stack_low_watermark_bytes =
        static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) *
        static_cast<std::uint32_t>(sizeof(StackType_t));
    memory.stack_available = true;

    // bflb_boot2_init re-reads both table copies and prints them on every call.
    int expected = 0;
    if (g_program_state.compare_exchange_strong(expected, 1)) {
        collect_program_info(g_program);
        g_program_state.store(2, std::memory_order_release);
    }
    if (g_program_state.load(std::memory_order_acquire) == 2) {
        memory.program_size_bytes = g_program.program_size_bytes;
        memory.program_capacity_bytes = g_program.program_capacity_bytes;
        memory.program_available = g_program.program_available;
    }

    const auto flash_size = bflb_flash_get_size();
    if (flash_size != 0U) {
        memory.flash_size_bytes = flash_size;
        memory.flash_available = true;
    }
    return memory;
}

/** @brief Returns the Bouffalo machine-timer millisecond count. */
std::uint32_t monotonic_milliseconds() {
    return static_cast<std::uint32_t>(bflb_mtimer_get_time_ms());
}

/** @brief Treats BL602 POR-style startup as a power-on reset. */
bool reset_was_power_on() {
    return true;
}

/**
 * @brief Recognizes a wake from hibernation by the flag HBN entry leaves in HBN_RSV0.
 *
 * Every other Bouffalo start looks like a power-on reset. The flag survives a software reset,
 * so the first call consumes it.
 */
RestartReason restart_reason() {
    static bool known = false;
    static bool hibernated = false;
    if (!known) {
        hibernated = HBN_Get_Status_Flag() == HBN_STATUS_ENTER_FLAG;
        if (hibernated)
            (void)HBN_Set_Status_Flag(0U);
        known = true;
    }
    return hibernated ? RestartReason::deep_sleep : RestartReason::unknown;
}

/**
 * @brief Counts candidate restarts without enabling unqualified boot protection.
 * Bouffalo cannot distinguish non-HBN reset causes here, so an unknown reset
 * continues a valid record. Deep-sleep wakes and invalid records start at one.
 * Retention remains unavailable to the boot guard until hardware qualification.
 */
FastRestartInfo begin_fast_restart_tracking() {
    const auto reason = restart_reason();
    const std::uint32_t record = retained_fast_restart;
    std::uint8_t count = 1U;
    if (reason == RestartReason::unknown && valid_fast_restart_record(record)) {
        const auto previous = static_cast<std::uint8_t>(record);
        count = previous == UINT8_MAX ? UINT8_MAX : static_cast<std::uint8_t>(previous + 1U);
    }
    store_fast_restart_count(count);
    return {count, false};
}

/** @brief Clears the candidate count and confirms firmware after the stable window. */
void mark_boot_stable() {
    store_fast_restart_count(0U);
    // The SDK clears the high retry bits of the FW entry age when they are
    // set and performs no flash write otherwise.  Initialization is required
    // first because the partition helpers need their flash operations.
    if (bflb_boot2_init() != 0) {
        return;
    }
    (void)pt_table_set_fw_boot_success(PT_ENTRY_FW_CPU0);
}

/** @brief Blocks the current task using rounded-up FreeRTOS ticks. */
/** @brief Uses the native SDK busy wait for peripheral edge timing. */
void delay_microseconds(const std::uint32_t duration) {
    bflb_mtimer_delay_us(duration);
}

void sleep_for_milliseconds(const std::uint32_t duration) {
    if (duration == 0U)
        return;
    // Use a wide product so milliseconds-to-ticks conversion cannot overflow.
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(duration) * configTICK_RATE_HZ + 999U) / 1000U;
    // Split durations beyond TickType_t range; never request portMAX_DELAY.
    constexpr auto max_ticks = static_cast<std::uint64_t>(portMAX_DELAY) - 1U;
    auto remaining = ticks > 0U ? ticks : 1U;
    while (remaining > max_ticks) {
        vTaskDelay(static_cast<TickType_t>(max_ticks));
        remaining -= max_ticks;
    }
    vTaskDelay(static_cast<TickType_t>(remaining));
}

/** @brief Provides the platform execution-yield opportunity. */
void yield_execution() {
    vTaskDelay(0);
}

/** @brief Does nothing: no topology here lets a configuration take console pins. */
bool release_console() {
    return false;
}

/** @brief Attaches the UART0 receive interrupt to the console ring. */
void initialize_console() {
    // Board startup owns UART0 line configuration; only receive handling changes here.
    auto* const device = bflb_device_get_by_name("uart0");
    if (device == nullptr || g_console_rx_interrupt) {
        return;
    }
    g_console_uart = device;
    bflb_uart_rxint_mask(device, false);
    bflb_irq_attach(device->irq_num, console_rx_isr, nullptr);
    bflb_irq_enable(device->irq_num);
    g_console_rx_interrupt = true;
}

/** @brief Describes the SDK-owned Bouffalo serial console. */
const char* console_status() {
    return "Using Bouffalo UART0 console";
}

/** @brief Returns the SDK-configured UART0 console rate. */
std::uint32_t console_baudrate() {
    return static_cast<std::uint32_t>(CONFIG_CONSOLE_UART_BAUDRATE);
}

/** @brief Writes one application log line through standard output. */
void write_log(const std::string_view message) {
    std::printf("%.*s\r\n", static_cast<int>(message.size()), message.data());
}

/** @brief Reads one buffered console byte, or polls UART0 before its interrupt is attached. */
bool read_console_character(char& character) {
    if (g_console_rx_interrupt) {
        const auto tail = g_console_rx_tail.load(std::memory_order_relaxed);
        if (tail == g_console_rx_head.load(std::memory_order_acquire)) {
            return false;
        }
        character = g_console_rx[tail % kConsoleRxCapacity];
        g_console_rx_tail.store(tail + 1U, std::memory_order_release);
        return true;
    }
    auto* const device = bflb_device_get_by_name("uart0");
    if (device == nullptr || !bflb_uart_rxavailable(device)) {
        return false;
    }
    // bflb_uart_getchar is non-blocking: it returns -1 when the FIFO drained
    // between the availability check and the read.
    const int value = bflb_uart_getchar(device);
    if (value < 0) {
        return false;
    }
    character = static_cast<char>(value);
    return true;
}

/** @brief Writes command response text through standard output. */
bool write_console(const std::string_view text) {
    return std::printf("%.*s", static_cast<int>(text.size()), text.data()) >= 0;
}

/**
 * @brief Selects the inactive FW slot for the next boot.
 *
 * The flipped entry is written to the currently inactive partition-table
 * copy with a bumped age, so that copy becomes authoritative.  A torn write
 * fails its CRC check and the previous table keeps booting the current
 * image; the follow-up re-read verifies the switch before reporting it.
 */
BootPartitionSwitchResult prepare_boot_partition_toggle(AlternateImage) {
    if (bflb_boot2_init() != 0) {
        return BootPartitionSwitchResult::failed;
    }
    const auto active_id = bflb_boot2_get_active_partition();
    if (active_id > PT_TABLE_ID_1) {
        return BootPartitionSwitchResult::failed;
    }
    pt_table_entry_config entry{};
    if (pt_table_get_active_entries_by_name(bflb_boot2_get_pt_config(),
            reinterpret_cast<uint8_t*>(const_cast<char*>(kFirmwareEntryName)),
            &entry) != PT_ERROR_SUCCESS) {
        return BootPartitionSwitchResult::unavailable;
    }
    // Normalize with the SDK's own convention: only the low bit selects the
    // boot slot, higher values request a boot2-side OTA copy.
    const auto current = static_cast<std::uint8_t>(entry.active_index & 0x01);
    const auto target = static_cast<std::uint8_t>(current == 0 ? 1 : 0);
    entry.active_index = target;
    entry.age++;
    const auto inactive_id =
        static_cast<pt_table_id_type>(active_id == PT_TABLE_ID_0 ? PT_TABLE_ID_1 : PT_TABLE_ID_0);
    if (pt_table_update_entry(inactive_id, bflb_boot2_get_pt_config(), &entry) !=
        PT_ERROR_SUCCESS) {
        return BootPartitionSwitchResult::failed;
    }
    // Re-read both copies from flash so the reported result reflects the
    // authoritative table rather than the RAM cache.
    if (bflb_boot2_init() != 0) {
        return BootPartitionSwitchResult::failed;
    }
    pt_table_entry_config check{};
    if (pt_table_get_active_entries_by_name(bflb_boot2_get_pt_config(),
            reinterpret_cast<uint8_t*>(const_cast<char*>(kFirmwareEntryName)),
            &check) != PT_ERROR_SUCCESS) {
        return BootPartitionSwitchResult::failed;
    }
    return (check.active_index & 0x01) == target ? BootPartitionSwitchResult::prepared
                                                 : BootPartitionSwitchResult::failed;
}

/** @brief Reports unavailable until Bouffalo calibration storage is qualified. */
RadioCalibrationResetResult erase_radio_calibration() {
    // BL602 RF parameters live in the linked image RFTLV and efuse; the SDK
    // exposes no qualified flash-backed calibration erase path.
    return RadioCalibrationResetResult::unavailable;
}

/** @brief Requests a Bouffalo power-on reset and does not return. */
[[noreturn]] void restart() {
    (void)bl_sys_reset_por();
    while (true) {}
}

} // namespace newmota::platform
