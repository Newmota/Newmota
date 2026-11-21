/*
  platform.cpp - Raspberry Pi Pico platform implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/platform.h"

#include "boot_select.h"
#include "boot_trace.h"
#include "newmota_features.h"
#if USE_DEEPSLEEP
#include "deep_sleep_wake.h"
#endif
#include "flash_access.h"
#include "memory_info.h"

#if defined(PICO_CYW43_ARCH_POLL) && PICO_CYW43_ARCH_POLL
#include "pico/cyw43_arch.h"
#endif
#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#if PICO_RP2040
#include "hardware/structs/nvic.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/sync.h"
#include "rp2040_image.h"
#endif
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "pico/platform/sections.h"
#if PICO_RP2350
#include "pico/bootrom.h"
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <malloc.h>
#include <unistd.h>

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

#ifndef NEWMOTA_SAFEBOOT_OFFSET
#error "NEWMOTA_SAFEBOOT_OFFSET is required"
#endif

#ifndef NEWMOTA_SAFEBOOT_SIZE
#error "NEWMOTA_SAFEBOOT_SIZE is required"
#endif

#ifndef NEWMOTA_APPLICATION_OFFSET
#error "NEWMOTA_APPLICATION_OFFSET is required"
#endif

#ifndef NEWMOTA_APPLICATION_SIZE
#error "NEWMOTA_APPLICATION_SIZE is required"
#endif

#ifndef NEWMOTA_UPDATE_FLASH_BYTES
#error "NEWMOTA_UPDATE_FLASH_BYTES is required"
#endif

#ifndef NEWMOTA_BOOT_MARKER_OFFSET
#error "NEWMOTA_BOOT_MARKER_OFFSET is required"
#endif

namespace newmota::platform {
namespace {

constexpr std::uint32_t kStackWatermarkPattern = 0xD15EA5EDU;
extern "C" char __StackBottom[];
extern "C" char __StackTop[];
extern "C" char __StackLimit[];
extern "C" char __logical_binary_start[];
extern "C" char __flash_binary_end[];
bool g_stack_watermark_initialized = false;

constexpr PlatformInfo kInformation{
    NEWMOTA_TARGET_NAME,
    NEWMOTA_CHIP_NAME,
    "Pico SDK",
    NEWMOTA_SDK_VERSION,
    NEWMOTA_FRAMEWORK_VERSION,
    NEWMOTA_BUILD_DATE " " NEWMOTA_BUILD_TIME,
};

static_assert(NEWMOTA_SAFEBOOT_OFFSET == 0U, "Pico Safeboot must start at flash offset zero");
#if PICO_RP2350
/** @brief XIP address and size of the RP2350 ROM-chain application. */
constexpr std::uint32_t kApplicationBase =
    static_cast<std::uint32_t>(XIP_BASE) + NEWMOTA_APPLICATION_OFFSET;
#endif

/** @brief Counts free allocator blocks and uncommitted Newlib heap space. */
[[nodiscard]] std::uint32_t free_heap_bytes() {
    const auto allocator = ::mallinfo();
    const auto free_blocks =
        allocator.fordblks > 0 ? static_cast<std::uint32_t>(allocator.fordblks) : 0U;
    const auto heap_break = reinterpret_cast<std::uintptr_t>(::sbrk(0));
    const auto heap_limit = reinterpret_cast<std::uintptr_t>(__StackLimit);
    if (heap_break > heap_limit) {
        return free_blocks;
    }
    return free_blocks + static_cast<std::uint32_t>(heap_limit - heap_break);
}

/** @brief Returns the linker-sized current image and its assigned flash slot. */
[[nodiscard]] bool running_program_info(std::uint32_t& image_size, std::uint32_t& capacity) {
    const auto start = reinterpret_cast<std::uintptr_t>(__logical_binary_start);
    const auto end = reinterpret_cast<std::uintptr_t>(__flash_binary_end);
    if (end <= start || end - start > UINT32_MAX) {
        return false;
    }

#if defined(NEWMOTA_IMAGE_APPLICATION)
    capacity = NEWMOTA_APPLICATION_SIZE;
#elif defined(NEWMOTA_IMAGE_SAFEBOOT)
    capacity = NEWMOTA_SAFEBOOT_SIZE;
#else
    return false;
#endif
    image_size = static_cast<std::uint32_t>(end - start);
    return capacity != 0U && image_size <= capacity;
}

constexpr std::uint32_t kApplicationSize = NEWMOTA_APPLICATION_SIZE;
/** @brief Flash offset of the platform-owned sticky-boot marker sector. */
constexpr std::uint32_t kMarkerOffset = NEWMOTA_BOOT_MARKER_OFFSET;
/** @brief Magic identifying a programmed sticky-boot marker ("NBMS"). */
constexpr std::uint32_t kMarkerMagic = 0x534D424EU;
/** @brief Marker target selecting the Safeboot slot. */
constexpr std::uint32_t kMarkerTargetSafeboot = 1U;
/** @brief Watchdog scratch registers retained across soft resets. */
constexpr std::uint32_t kScratchMagic = 0x52535441U;
constexpr int kScratchMagicIndex = 0;
constexpr int kScratchCountIndex = 1;
/** @brief Cap for the consecutive fast-restart counter. */
constexpr std::uint32_t kMaxFastRestarts = 255U;

/** @brief Set once at console start, because enabling the watchdog changes what it reports. */
RestartReason startup_restart_reason{RestartReason::unknown};

/** @brief Separates a watchdog timeout from a software reboot, which also uses the watchdog. */
RestartReason read_restart_reason() {
#if USE_DEEPSLEEP
    if (pico_detail::woke_from_deep_sleep())
        return RestartReason::deep_sleep;
#endif
    if (!watchdog_caused_reboot())
        return RestartReason::power_on;
    return watchdog_enable_caused_reboot() ? RestartReason::watchdog : RestartReason::software;
}

struct Marker final {
    std::uint32_t magic;
    std::uint32_t target;
    std::uint32_t check;
};

static_assert(sizeof(Marker) == 12U, "marker layout is fixed");

#if PICO_RP2040 && defined(NEWMOTA_IMAGE_SAFEBOOT)
/** @brief Marker scanned by the host to identify an installed RP2040 layout. */
constexpr char kRp2040LayoutIdentity[] = "NEWMOTA-RP2040-LAYOUT-V1:" NEWMOTA_UPDATE_LAYOUT_SHA256;
#endif

/** @brief Reads the sticky-boot request without touching the filesystem. */
[[nodiscard]] bool read_marker(Marker& marker) {
    return pico_detail::flash_read(kMarkerOffset, &marker, sizeof(marker)) &&
           marker.magic == kMarkerMagic && marker.check == (kMarkerMagic ^ marker.target);
}

/** @brief Programs or clears the permission-checked sticky-boot marker. */
[[nodiscard]] bool write_marker(const bool program, const std::uint32_t target) {
    if (!pico_detail::flash_erase(kMarkerOffset, FLASH_SECTOR_SIZE)) {
        return false;
    }
    if (!program) {
        Marker erased{};
        return pico_detail::flash_read(kMarkerOffset, &erased, sizeof(erased)) &&
               erased.magic == UINT32_MAX && erased.target == UINT32_MAX &&
               erased.check == UINT32_MAX;
    }

    // The sector is freshly erased, so one page is sufficient. Padding with
    // zeros only clears additional bits and never reaches another partition.
    alignas(4) static std::uint8_t page[FLASH_PAGE_SIZE];
    const Marker marker{kMarkerMagic, target, kMarkerMagic ^ target};
    std::memset(page, 0, sizeof(page));
    std::memcpy(page, &marker, sizeof(marker));
    if (!pico_detail::flash_program(kMarkerOffset, page, sizeof(page))) {
        return false;
    }
    Marker observed{};
    return read_marker(observed) && observed.magic == marker.magic &&
           observed.target == marker.target && observed.check == marker.check;
}

#if PICO_RP2040
/** @brief Reads a bounded range from the physical RP2040 application image. */
[[nodiscard]] bool read_application_image(
    void*, const std::uint32_t offset, std::uint8_t* const buffer, const std::size_t size) {
    if (size > UINT32_MAX || offset > NEWMOTA_APPLICATION_SIZE ||
        size > NEWMOTA_APPLICATION_SIZE - offset) {
        return false;
    }
    return pico_detail::flash_read(NEWMOTA_APPLICATION_OFFSET + offset, buffer, size);
}

/** @brief Removes Safeboot's inherited interrupt state before app CRT startup. */
void reset_rp2040_interrupt_state() {
    disable_interrupts();
    nvic_hw->icer = UINT32_MAX;
    nvic_hw->icpr = UINT32_MAX;
    systick_hw->csr = 0U;
    scb_hw->icsr = M0PLUS_ICSR_PENDSTCLR_BITS | M0PLUS_ICSR_PENDSVCLR_BITS;
    __asm volatile("dsb\n"
                   "isb\n"
        :
        :
        : "memory");
}
#endif

/** @brief Chains from first-stage Safeboot into the table-free application. */
[[nodiscard]] int chain_to_application() {
#if PICO_RP2040
    const auto runtime_base = static_cast<std::uint32_t>(XIP_BASE) + NEWMOTA_APPLICATION_OFFSET;
    if (!pico_detail::valid_rp2040_application(
            kApplicationSize, runtime_base, SRAM_BASE, SRAM_END, read_application_image, nullptr)) {
        return -1;
    }
    const auto vector_offset = NEWMOTA_APPLICATION_OFFSET + 0x100U;
    std::array<std::uint32_t, 2> vectors{};
    if (!pico_detail::flash_read(vector_offset, vectors.data(), sizeof(vectors))) {
        return -1;
    }
    const auto stack = vectors[0];
    const auto reset = vectors[1];
    const auto vector_address = static_cast<std::uint32_t>(XIP_BASE) + vector_offset;

    // RP2040 has no ROM image-chain service. Safeboot validates the image's
    // boot2 checksum, initial stack, and reset vector, then enters the
    // application while retaining the physical XIP mapping. Clear SDK alarm,
    // SysTick, and NVIC state first so its CRT starts with a clean IRQ state.
    reset_rp2040_interrupt_state();
    scb_hw->vtor = vector_address;
    __asm volatile("dsb\n"
                   "isb\n"
                   "msr msp, %0\n"
                   // Release PRIMASK only after installing the application
                   // vectors and stack, before its SDK CRT reset handler runs.
                   "cpsie i\n"
                   "bx %1\n"
        :
        : "r"(stack), "r"(reset)
        : "memory");
    __builtin_unreachable();
#else
    alignas(4) static std::uint8_t workarea[4096];
    return rom_chain_image(workarea, sizeof(workarea), kApplicationBase, kApplicationSize);
#endif
}

} // namespace

/** @brief Returns immutable identity for the active Pico SDK environment. */
const PlatformInfo& info() {
    return kInformation;
}

/** @brief Paints the free main stack and records its watermark before services start. */
void pico_detail::initialize_stack_watermark() {
    if (g_stack_watermark_initialized) {
        return;
    }

    const auto interrupt_state = save_and_disable_interrupts();
    std::uintptr_t stack_pointer = 0U;
    __asm volatile("mov %0, sp" : "=r"(stack_pointer));
    const auto bottom = reinterpret_cast<std::uintptr_t>(__StackBottom);
    const auto top = reinterpret_cast<std::uintptr_t>(__StackTop);
    if (stack_pointer > bottom && stack_pointer <= top) {
        for (auto address = bottom; address < stack_pointer; address += sizeof(std::uint32_t)) {
            *reinterpret_cast<volatile std::uint32_t*>(address) = kStackWatermarkPattern;
        }
        g_stack_watermark_initialized = true;
    }
    restore_interrupts(interrupt_state);
}

/** @brief Returns the unused stack bytes found below the deepest painted word. */
namespace pico_detail {

std::uint32_t __no_inline_not_in_flash_func(stack_watermark_bytes)() {
    if (!g_stack_watermark_initialized) {
        return 0U;
    }
    const auto bottom = reinterpret_cast<std::uintptr_t>(__StackBottom);
    const auto top = reinterpret_cast<std::uintptr_t>(__StackTop);
    auto address = bottom;
    while (address + sizeof(std::uint32_t) <= top &&
           *reinterpret_cast<const volatile std::uint32_t*>(address) == kStackWatermarkPattern) {
        address += sizeof(std::uint32_t);
    }
    return static_cast<std::uint32_t>(address - bottom);
}

} // namespace pico_detail

/** @brief Reports whether the Pico main-stack watermark is ready to query. */
bool pico_detail::stack_watermark_initialized() {
    return g_stack_watermark_initialized;
}

/** @brief Reports the system clock; Status omits flash identity on Pico. */
HardwareInfo hardware_info() {
    HardwareInfo hardware{};
    hardware.cpu_frequency_mhz = clock_get_hz(clk_sys) / 1000000U;
    return hardware;
}

/** @brief Collects Pico linker image, allocator heap, stack, and flash data. */
MemoryInfo memory_info() {
    pico_detail::initialize_stack_watermark();

    MemoryInfo memory{};
    memory.heap_free_bytes = free_heap_bytes();
    memory.heap_available = true;
    memory.stack_low_watermark_bytes = pico_detail::stack_watermark_bytes();
    memory.stack_available = pico_detail::stack_watermark_initialized();

    if (running_program_info(memory.program_size_bytes, memory.program_capacity_bytes)) {
        memory.program_available = true;
    }

    memory.flash_size_bytes = NEWMOTA_UPDATE_FLASH_BYTES;
    memory.flash_available = memory.flash_size_bytes != 0U;
    return memory;
}

/** @brief Returns the low 32 bits of Pico SDK monotonic milliseconds. */
std::uint32_t monotonic_milliseconds() {
    return static_cast<std::uint32_t>(to_ms_since_boot(get_absolute_time()));
}

/** @brief Distinguishes watchdog software reboot from a power-on/reset. */
bool reset_was_power_on() {
    return !watchdog_caused_reboot();
}

/** @brief Returns the reason captured before the boot trace could change it. */
RestartReason restart_reason() {
    return startup_restart_reason;
}

/** @brief Counts consecutive soft resets in retained watchdog scratch. */
FastRestartInfo begin_fast_restart_tracking() {
    // Scratch survives watchdog/soft resets but clears on power-on, RUN pin,
    // or POWMAN reset, which is exactly the fast-restart window.
    std::uint32_t count = 1U;
#if USE_DEEPSLEEP
    // RP2040 reboots through the watchdog after deep sleep; that wake is a fresh start.
    const bool fresh = pico_detail::woke_from_deep_sleep();
#else
    const bool fresh = false;
#endif
    if (!fresh && watchdog_hw->scratch[kScratchMagicIndex] == kScratchMagic) {
        count = watchdog_hw->scratch[kScratchCountIndex] + 1U;
        if (count > kMaxFastRestarts) {
            count = kMaxFastRestarts;
        }
    } else {
        watchdog_hw->scratch[kScratchMagicIndex] = kScratchMagic;
    }
    watchdog_hw->scratch[kScratchCountIndex] = count;
    return FastRestartInfo{static_cast<std::uint8_t>(count), true};
}

/** @brief Clears fast-restart state and buys a pending TBYB trial image. */
void mark_boot_stable() {
    watchdog_hw->scratch[kScratchMagicIndex] = 0U;
    watchdog_hw->scratch[kScratchCountIndex] = 0U;
#if PICO_RP2350
    // Both shipped images boot without the TBYB flag, so a pending trial can
    // only arise from an explicit flash-update boot (for example a wired
    // trial write). Buying after a stable boot is exactly the confirm step.
    boot_info_t boot{};
    if (rom_get_boot_info(&boot) &&
        (boot.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING) != 0U) {
        alignas(4) static std::uint8_t buy_buffer[4096];
        (void)rom_explicit_buy(buy_buffer, sizeof(buy_buffer));
    }
#endif
}

/** @brief Sleeps through the Pico SDK alarm infrastructure. */
/** @brief Uses the native SDK busy wait for peripheral edge timing. */
void delay_microseconds(const std::uint32_t duration) {
    busy_wait_us_32(duration);
}

void sleep_for_milliseconds(const std::uint32_t duration) {
    // The cooperative loop calls this once per iteration. In polling mode,
    // service CYW43/lwIP here so TLS callbacks run on the application stack,
    // not in the SDK's low-priority IRQ context.
#if defined(PICO_CYW43_ARCH_POLL) && PICO_CYW43_ARCH_POLL
    if (cyw43_arch_async_context() != nullptr)
        cyw43_arch_poll();
#endif
    pico_detail::service_boot_trace();
    sleep_ms(duration);
}

/** @brief Provides the platform execution-yield opportunity. */
void yield_execution() {
    sleep_for_milliseconds(1U);
}

/** @brief Does nothing: no topology here lets a configuration take console pins. */
bool release_console() {
    return false;
}

/** @brief Initializes all Pico SDK standard-I/O backends. */
void initialize_console() {
    // This is the image's only USB standard-I/O initialization: the Pico SDK's
    // initialization is not idempotent, so a second call re-enters TinyUSB and
    // re-registers its interrupt. The retained record is therefore reported
    // here rather than from image entry.
    stdio_init_all();
    startup_restart_reason = read_restart_reason();
    pico_detail::begin_boot_trace();
    pico_detail::mark_boot_stage(pico_detail::BootStage::console_ready);
}

/** @brief Describes the Pico USB CDC console backend. */
const char* console_status() {
    return "Using USB CDC";
}

/** @brief USB CDC has no firmware-side UART baud rate. */
std::uint32_t console_baudrate() {
    return 0U;
}

/** @brief Writes one application log line through Pico standard I/O. */
void write_log(const std::string_view message) {
    // Use the SDK's buffered USB output without an explicit flush per line.
    // The SDK serializes foreground and background USB servicing with its
    // stdio mutex; the observed boot faults did not establish a USB race.
    pico_detail::mark_boot_stage(pico_detail::BootStage::logging);
    (void)stdio_put_string(message.data(), static_cast<int>(message.size()), false, false);
    (void)stdio_put_string("\r\n", 2, false, false);
}

/** @brief Reads one character without blocking from Pico standard input. */
bool read_console_character(char& character) {
    const int value = getchar_timeout_us(0);
    if (value == PICO_ERROR_TIMEOUT) {
        return false;
    }
    character = static_cast<char>(value);
    return true;
}

/** @brief Writes command response text through Pico standard output. */
bool write_console(const std::string_view text) {
    return stdio_put_string(text.data(), static_cast<int>(text.size()), false, false) >= 0;
}

/** @brief Arms the sticky alternate-slot request for the upcoming restart. */
BootPartitionSwitchResult prepare_boot_partition_toggle(AlternateImage) {
#if defined(NEWMOTA_IMAGE_APPLICATION)
    // The marker persists across power cycles in the platform-owned sector
    // outside LittleFS; entry honors it on every boot until cleared.
    if (!write_marker(true, kMarkerTargetSafeboot)) {
        return BootPartitionSwitchResult::failed;
    }
    return BootPartitionSwitchResult::prepared;
#elif defined(NEWMOTA_IMAGE_SAFEBOOT)
    // Returning clears the recovery request. The following boot still enters
    // Safeboot first, which then chains into the verified application slot.
    if (!write_marker(false, 0U)) {
        return BootPartitionSwitchResult::failed;
    }
    return BootPartitionSwitchResult::prepared;
#else
    return BootPartitionSwitchResult::unavailable;
#endif
}

/** @brief Reports unavailable: CYW43 persists no flash calibration. */
RadioCalibrationResetResult erase_radio_calibration() {
    return RadioCalibrationResetResult::unavailable;
}

/** @brief Reboots the Pico through its watchdog and does not return. */
[[noreturn]] void restart() {
    watchdog_reboot(0, 0, 0);
    while (true) {}
}

namespace pico_detail {

/** @brief Reports a valid sticky recovery request without mounting LittleFS. */
bool recovery_mode_requested() {
    Marker marker{};
    return read_marker(marker) && marker.target == kMarkerTargetSafeboot;
}

/** @brief Clears the raw recovery marker after successful application verify. */
bool clear_recovery_mode() {
    return write_marker(false, 0U);
}

const char* compiled_layout_identity() {
#if PICO_RP2040 && defined(NEWMOTA_IMAGE_SAFEBOOT)
    return kRp2040LayoutIdentity;
#else
    return "";
#endif
}

/** @brief Chains to the application unless Safeboot recovery was requested. */
void honor_sticky_boot_request() {
#if defined(NEWMOTA_IMAGE_SAFEBOOT)
    if (recovery_mode_requested()) {
        return;
    }
    if (chain_to_application() == 0) {
        while (true) {}
    }
    // A failed chain must persist recovery mode so the writer accepts a new
    // application even when the previous marker was absent or damaged.
    (void)write_marker(true, kMarkerTargetSafeboot);
#else
    // The table-free application is entered only through Safeboot.
#endif
}

} // namespace pico_detail
} // namespace newmota::platform
