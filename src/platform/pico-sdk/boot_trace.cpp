/*
  boot_trace.cpp - retained boot-progress trace for Pico SDK qualification

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "boot_trace.h"
#include "memory_info.h"

#include <cstddef>
#include <cstdint>

#if defined(NEWMOTA_PICO_BOOT_TRACE)

#include "pico/platform/sections.h"

#include "hardware/regs/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"

#include "pico/stdlib.h"

namespace newmota::platform::pico_detail {
namespace {

/**
 * RP2040 watchdog scratch registers survive a watchdog restart and are cleared
 * only by a power-on, RUN-pin, or POWMAN reset. Indices 0 and 1 are already
 * owned by fast-restart counting, so the trace uses the remaining registers.
 */
constexpr int kStageIndex = 2;
constexpr int kFaultPcIndex = 3;
constexpr int kFaultLrIndex = 4;
constexpr int kFaultXpsrIndex = 5;
constexpr int kFaultSpIndex = 6;
constexpr int kStackStatusIndex = 7;
constexpr std::uint32_t kStackMeasured = 0x40000000U;
constexpr std::uint32_t kStackDamaged = 0x80000000U;
extern "C" char __StackTop[];
extern "C" char __StackBottom[];
bool stack_initialized = false;
std::uintptr_t minimum_sampled_sp = 0U;

// Fill only unused stack below this function's live frame. IRQs are masked
// during initialization; the margin excludes helper frames and current locals.
void initialize_stack_measurement() {
    pico_detail::initialize_stack_watermark();
    std::uintptr_t sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    stack_initialized = pico_detail::stack_watermark_initialized();
    if (stack_initialized)
        minimum_sampled_sp = sp;
}

std::uint32_t __no_inline_not_in_flash_func(stack_status)() {
    if (!stack_initialized)
        return 0U;
    const auto free_bytes = pico_detail::stack_watermark_bytes();
    return kStackMeasured | (free_bytes == 0U ? kStackDamaged : 0U) | free_bytes;
}

/**
 * A stall is turned into a restart so the next boot can report it. The budget
 * only has to exceed the longest interval between two marks, which mark every
 * boot phase and every loop iteration.
 */
constexpr std::uint32_t kWatchdogTimeoutMilliseconds = 8000U;

[[nodiscard]] constexpr const char* stage_name(const BootStage stage) {
    switch (stage) {
    case BootStage::entry:
        return "entry";
    case BootStage::console_ready:
        return "console_ready";
    case BootStage::filesystem_mapped:
        return "filesystem_mapped";
    case BootStage::filesystem_read:
        return "filesystem_read";
    case BootStage::logging:
        return "logging";
    case BootStage::flash_erase_enter:
        return "flash_erase_enter";
    case BootStage::flash_erase_done:
        return "flash_erase_done";
    case BootStage::flash_program_enter:
        return "flash_program_enter";
    case BootStage::flash_program_done:
        return "flash_program_done";
    case BootStage::application_loop:
        return "application_loop";
    }
    return "unknown";
}

constexpr std::size_t kLineCapacity = 224;

/** @brief A fixed-capacity line builder for the retained record report. */
class Line final {
  public:
    void text(const char* const value) {
        for (const char* cursor = value; *cursor != '\0'; ++cursor) {
            buffer_[length_++] = *cursor;
        }
    }

    void character(const char value) { buffer_[length_++] = value; }

    void hex(std::uint32_t value) {
        static constexpr char kDigits[] = "0123456789abcdef";
        text("0x");
        for (int shift = 28; shift >= 0; shift -= 4) {
            buffer_[length_++] = kDigits[(value >> shift) & 0xFU];
        }
    }

    void decimal(std::uint32_t value) {
        char digits[10];
        std::size_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + (value % 10U));
            value /= 10U;
        } while (value != 0U);
        while (count > 0U) {
            buffer_[length_++] = digits[--count];
        }
    }

    void flush() {
        buffer_[length_] = '\0';
        (void)stdio_put_string(buffer_, -1, false, false);
        (void)stdio_put_string("\r\n", 2, false, false);
    }

  private:
    char buffer_[kLineCapacity]{};
    std::size_t length_{0};
};

/** @brief Prints the retained record of the previous run, if there is one. */
void report_retained_record() {
    const auto stage = static_cast<BootStage>(watchdog_hw->scratch[kStageIndex]);
    const auto fault_pc = watchdog_hw->scratch[kFaultPcIndex];

    Line line;
    if (stage == static_cast<BootStage>(0)) {
        line.text("NBT: no retained record; power-on reset or first traced boot");
        line.flush();
        return;
    }

    line.text("NBT: previous boot stopped at ");
    line.text(stage_name(stage));
    line.character('(');
    line.decimal(watchdog_hw->scratch[kStageIndex]);
    line.character(')');
    if (fault_pc != 0U) {
        line.text(" hardfault pc=");
        line.hex(fault_pc);
        line.text(" lr=");
        line.hex(watchdog_hw->scratch[kFaultLrIndex]);
        line.text(" sp=");
        line.hex(watchdog_hw->scratch[kFaultSpIndex]);
        line.text(" stacktop=");
        line.hex(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__StackTop)));
        line.text(" xpsr=");
        line.hex(watchdog_hw->scratch[kFaultXpsrIndex]);
    } else if (watchdog_hw->scratch[kFaultSpIndex] != 0U) {
        line.text(" hardfault frame outside stack/alignment bounds sp=");
        line.hex(watchdog_hw->scratch[kFaultSpIndex]);
    } else {
        line.text(" (no fault recorded; a stall or an uninstrumented fault)");
    }
    line.flush();
    const auto status = watchdog_hw->scratch[kStackStatusIndex];
    if ((status & kStackMeasured) != 0U) {
        Line stack_line;
        stack_line.text("NBT: previous stack untouched_bytes=");
        stack_line.decimal(status & ~(kStackMeasured | kStackDamaged));
        stack_line.text((status & kStackDamaged) != 0U ? " boundary=CHANGED" : " boundary=intact");
        stack_line.flush();
    }
}

/** @brief Discards the previous run's record so it is reported only once. */
void clear_retained_record() {
    watchdog_hw->scratch[kStageIndex] = 0U;
    watchdog_hw->scratch[kFaultPcIndex] = 0U;
    watchdog_hw->scratch[kFaultLrIndex] = 0U;
    watchdog_hw->scratch[kFaultXpsrIndex] = 0U;
    watchdog_hw->scratch[kFaultSpIndex] = 0U;
    watchdog_hw->scratch[kStackStatusIndex] = 0U;
}

} // namespace

// The handler switches to a dedicated RAM stack before entering C, so a
// damaged application stack cannot corrupt capture itself.
extern "C" {
alignas(8) std::uint8_t newmota_pico_fault_stack[512];
}

extern "C" __attribute__((naked)) void __no_inline_not_in_flash_func(isr_hardfault)() {
    __asm volatile("cpsid i\n"
                   "mov r1, lr\n"
                   "movs r2, #4\n"
                   "tst r1, r2\n"
                   "beq 1f\n"
                   "mrs r0, psp\n"
                   "b 2f\n"
                   "1: mrs r0, msp\n"
                   "2: ldr r2, =newmota_pico_fault_stack + 512\n"
                   "msr msp, r2\n"
                   "b newmota_pico_record_fault\n");
}

extern "C" [[noreturn]] void __no_inline_not_in_flash_func(newmota_pico_record_fault)(
    const std::uint32_t frame_address) {
    watchdog_hw->scratch[kFaultSpIndex] = frame_address;
    // Do not dereference a frame outside the allocated application stack.
    const auto bottom = reinterpret_cast<std::uintptr_t>(__StackBottom);
    const auto top = reinterpret_cast<std::uintptr_t>(__StackTop);
    if (frame_address >= bottom && frame_address <= top - 32U && (frame_address & 3U) == 0U) {
        const auto* frame = reinterpret_cast<const volatile std::uint32_t*>(frame_address);
        watchdog_hw->scratch[kFaultPcIndex] = frame[6];
        watchdog_hw->scratch[kFaultLrIndex] = frame[5];
        watchdog_hw->scratch[kFaultXpsrIndex] = frame[7];
    }
    watchdog_hw->scratch[kStackStatusIndex] = stack_status();
    // No USB, flash access, allocation, or SDK callbacks in fault context.
    watchdog_hw->ctrl = WATCHDOG_CTRL_TRIGGER_BITS;
    for (;;) {
        __asm volatile("nop");
    }
}

void __no_inline_not_in_flash_func(mark_boot_stage)(const BootStage stage) {
    std::uintptr_t sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    if (stack_initialized && sp < minimum_sampled_sp)
        minimum_sampled_sp = sp;
    watchdog_hw->scratch[kStageIndex] = static_cast<std::uint32_t>(stage);
    watchdog_update();
}

void begin_boot_trace() {
    report_retained_record();
    clear_retained_record();

    Line diagnostic;
    diagnostic.text("NBT: diagnostic cached-XIP reads; normal filesystem/settings writes enabled");
    diagnostic.flush();
    initialize_stack_measurement();

    watchdog_enable(kWatchdogTimeoutMilliseconds, false);
    mark_boot_stage(BootStage::entry);
}

void service_boot_trace() {
    static absolute_time_t next_report{};
    if (time_reached(next_report)) {
        const auto status = stack_status();
        watchdog_hw->scratch[kStackStatusIndex] = status;
        Line line;
        line.text("NBT: stack untouched_bytes=");
        line.decimal(status & ~(kStackMeasured | kStackDamaged));
        line.text(" min_sampled_sp=");
        line.hex(static_cast<std::uint32_t>(minimum_sampled_sp));
        line.text((status & kStackDamaged) != 0U ? " boundary=CHANGED" : " boundary=intact");
        line.flush();
        next_report = make_timeout_time_ms(10000U);
    }
    static bool loop_recorded = false;
    if (!loop_recorded) {
        loop_recorded = true;
        mark_boot_stage(BootStage::application_loop);
        return;
    }
    watchdog_update();
}

} // namespace newmota::platform::pico_detail

#else

namespace newmota::platform::pico_detail {

void begin_boot_trace() {}

void mark_boot_stage(BootStage) {}

void service_boot_trace() {}

} // namespace newmota::platform::pico_detail

#endif
