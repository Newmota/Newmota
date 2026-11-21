/*
  platform.cpp - Realtek AmebaD platform implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/platform.h"

#include "image_size.h"

#include "FreeRTOS.h"
#include "task.h"

extern "C" {
#include "ameba_soc.h"
#include "flash_api.h"
#include "rtl8721d_ota.h"
#include "rtl8721d_flash.h"
}

/** @brief Writes one byte through the AmebaD ROM logging UART. */
extern "C" void LOGUART_PutChar(unsigned char character);
/** @brief Reads one byte from the AmebaD ROM logging UART. */
extern "C" unsigned char LOGUART_GetChar(unsigned char pull_mode);
/** @brief Reports whether the AmebaD LOGUART has a byte waiting. */
extern "C" unsigned char LOGUART_Readable(void);
/** @brief Invokes the AmebaD SDK system reset primitive. */
extern "C" void sys_reset(void);

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

#ifndef NEWMOTA_OTA1_OFFSET
#error "NEWMOTA_OTA1_OFFSET is required"
#endif

#ifndef NEWMOTA_OTA2_OFFSET
#error "NEWMOTA_OTA2_OFFSET is required"
#endif

#ifndef NEWMOTA_OTA1_SIZE
#error "NEWMOTA_OTA1_SIZE is required"
#endif

#ifndef NEWMOTA_OTA2_SIZE
#error "NEWMOTA_OTA2_SIZE is required"
#endif

#ifndef NEWMOTA_UPDATE_FLASH_BYTES
#error "NEWMOTA_UPDATE_FLASH_BYTES is required"
#endif

namespace newmota::platform {
namespace {

constexpr std::uint32_t kLogUartBaudrate = 115200U;
constexpr std::uint32_t kOta1Offset = NEWMOTA_OTA1_OFFSET;
constexpr std::uint32_t kOta2Offset = NEWMOTA_OTA2_OFFSET;
constexpr std::uint32_t kImageSignature0 = 0x35393138U;
constexpr std::uint32_t kImageSignature1 = 0x31313738U;
/**
 * @brief One SDK user backup word, separate from deep sleep's REG1/4/5.
 * REG2 is used by the SDK factory tests. The pinned SDK has no active REG3 user;
 * its old flash-calibration write is commented out. Neither core's startup nor
 * Safeboot owns this word. Keep this allocation when updating the SDK.
 */
constexpr std::uint32_t kFastRestartRegister = BKUP_REG3;
constexpr std::uint32_t kFastRestartMagic = 0x4e4d0000U;
static_assert(sizeof(IMAGE_HEADER) == amebad_detail::kImageHeaderBytes);

/** @brief Validates a single-word magic, count and inverse-count record. */
[[nodiscard]] bool valid_fast_restart_record(const std::uint32_t record) {
    const auto count = static_cast<std::uint8_t>(record);
    const auto inverse = static_cast<std::uint8_t>(record >> 8U);
    return (record & 0xffff0000U) == kFastRestartMagic &&
           inverse == static_cast<std::uint8_t>(~count);
}

/** @brief Atomically replaces the backup word and verifies that it was stored. */
[[nodiscard]] bool store_fast_restart_count(const std::uint8_t count) {
    const std::uint32_t record =
        kFastRestartMagic | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(~count)) << 8U) |
        count;
    BKUP_Write(kFastRestartRegister, record);
    return BKUP_Read(kFastRestartRegister) == record;
}

enum class OtaImage : std::uint8_t {
    none,
    ota1,
    ota2,
};

struct BootTogglePlan final {
    OtaImage target{};
    OtaImage current{};
    bool restore_target{};

    [[nodiscard]] constexpr bool available() const {
        return target != OtaImage::none && current != OtaImage::none;
    }
};

/** @brief Plans a toggle from the signatures observed before changing flash. */
[[nodiscard]] constexpr BootTogglePlan boot_toggle_plan(
    const bool ota1_valid, const bool ota2_valid) {
    if (ota2_valid) {
        // The bootloader prefers OTA2 when both signatures are valid.
        return {OtaImage::ota1, OtaImage::ota2, !ota1_valid};
    }
    if (ota1_valid) {
        return {OtaImage::ota2, OtaImage::ota1, true};
    }
    return {};
}

/** @brief Resolves a selectable image to its configured physical offset. */
[[nodiscard]] constexpr std::uint32_t image_offset(const OtaImage image) {
    return image == OtaImage::ota1 ? kOta1Offset : kOta2Offset;
}

constexpr BootTogglePlan kNoValidImage = boot_toggle_plan(false, false);
constexpr BootTogglePlan kOta1Only = boot_toggle_plan(true, false);
constexpr BootTogglePlan kOta2Only = boot_toggle_plan(false, true);
constexpr BootTogglePlan kBothValid = boot_toggle_plan(true, true);
static_assert(!kNoValidImage.available());
static_assert(kOta1Only.target == OtaImage::ota2 && kOta1Only.current == OtaImage::ota1 &&
              kOta1Only.restore_target);
static_assert(kOta2Only.target == OtaImage::ota1 && kOta2Only.current == OtaImage::ota2 &&
              kOta2Only.restore_target);
static_assert(kBothValid.target == OtaImage::ota1 && kBothValid.current == OtaImage::ota2 &&
              !kBothValid.restore_target);

constexpr PlatformInfo kInformation{
    NEWMOTA_TARGET_NAME,
    NEWMOTA_CHIP_NAME,
    "Realtek AmebaD SDK",
    NEWMOTA_SDK_VERSION,
    NEWMOTA_FRAMEWORK_VERSION,
    NEWMOTA_BUILD_DATE " " NEWMOTA_BUILD_TIME,
};

/** @brief Resolves the actual LP flash mapping against the Newmota OTA layout. */
[[nodiscard]] OtaImage running_image() {
    const auto& entry = RSIP_BASE->FLASH_MMU[0];
    const std::uint32_t control = entry.MMU_ENTRYx_CTRL;
    if ((control & MMU_BIT_ENTRY_VALID) == 0U)
        return OtaImage::none;
    const std::uint32_t start = entry.MMU_ENTRYx_STRADDR;
    const std::uint32_t delta = entry.MMU_ENTRYx_OFFSET;
    const bool subtract = (control & MMU_BIT_ENTRY_OFFSET_MINUS) != 0U;
    if (subtract && delta > start)
        return OtaImage::none;
    const std::uint64_t physical = subtract ? static_cast<std::uint64_t>(start) - delta
                                            : static_cast<std::uint64_t>(start) + delta;
    if (physical == SPI_FLASH_BASE + kOta1Offset)
        return OtaImage::ota1;
    if (physical == SPI_FLASH_BASE + kOta2Offset)
        return OtaImage::ota2;
    return OtaImage::none;
}

/** @brief Reads a header at a relative offset in the already bounded active slot. */
[[nodiscard]] bool read_image_header(
    void* context, const std::uint32_t offset, std::uint8_t* bytes, const std::size_t size) {
    const auto slot_offset = *static_cast<const std::uint32_t*>(context);
    return flash_stream_read(
               nullptr, slot_offset + offset, static_cast<std::uint32_t>(size), bytes) == 1;
}

} // namespace

/** @brief Returns immutable identity for the active AmebaD environment. */
const PlatformInfo& info() {
    return kInformation;
}

/** @brief Checks an AmebaD combined-image signature at a physical offset. */
IMAGE2_RAM_TEXT_SECTION
bool valid_image_signature(const std::uint32_t offset) {
    std::uint32_t signature[2]{};
    // The SDK exports FLASH_WriteStream but omits the matching read
    // declaration.  The public mbed flash API provides the same cached read
    // operation and does not dereference its flash_t argument.
    if (flash_stream_read(
            nullptr, offset, sizeof(signature), reinterpret_cast<uint8_t*>(signature)) != 1) {
        return false;
    }
    return signature[0] == kImageSignature0 && signature[1] == kImageSignature1;
}

/** @brief Invalidates one image without erasing its remaining contents. */
IMAGE2_RAM_TEXT_SECTION
bool invalidate_image(const std::uint32_t offset) {
    u8 empty_signature[8]{};
    return FLASH_WriteStream(offset, sizeof(empty_signature), empty_signature) == 1;
}

/** @brief Restores one image signature while preserving its image sector. */
IMAGE2_RAM_TEXT_SECTION
bool restore_image(const std::uint32_t offset) {
    u32 signature[2] = {kImageSignature0, kImageSignature1};
    FLASH_EreaseDwordsXIP(offset, 2);
    return FLASH_WriteStream(offset, sizeof(signature), reinterpret_cast<u8*>(signature)) == 1;
}

/** @brief Reports nothing yet; Status omits clock and flash identity on AmebaD. */
HardwareInfo hardware_info() {
    return {};
}

/** @brief Collects the running AmebaD image, heap, stack, and flash measurements. */
MemoryInfo memory_info() {
    MemoryInfo memory{};
    memory.heap_free_bytes = static_cast<std::uint32_t>(xPortGetFreeHeapSize());
    memory.heap_available = true;

    memory.stack_low_watermark_bytes =
        static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) *
        static_cast<std::uint32_t>(sizeof(StackType_t));
    memory.stack_available = true;

    const auto current = running_image();
    auto slot_offset = image_offset(current);
    const auto slot_capacity =
        current == OtaImage::ota1   ? static_cast<std::uint32_t>(NEWMOTA_OTA1_SIZE)
        : current == OtaImage::ota2 ? static_cast<std::uint32_t>(NEWMOTA_OTA2_SIZE)
                                    : 0U;
    std::uint32_t image_size = 0U;
    if (slot_capacity != 0U && slot_offset <= NEWMOTA_UPDATE_FLASH_BYTES &&
        slot_capacity <= NEWMOTA_UPDATE_FLASH_BYTES - slot_offset &&
        amebad_detail::combined_image_size(
            slot_capacity, read_image_header, &slot_offset, image_size)) {
        memory.program_size_bytes = image_size;
        memory.program_capacity_bytes = slot_capacity;
        memory.program_available = true;
    }

    memory.flash_size_bytes = NEWMOTA_UPDATE_FLASH_BYTES;
    memory.flash_available = true;
    return memory;
}

/** @brief Converts the FreeRTOS tick count to wrapping milliseconds. */
std::uint32_t monotonic_milliseconds() {
    return static_cast<std::uint32_t>(xTaskGetTickCount()) * portTICK_PERIOD_MS;
}

/** @brief Recognizes an AmebaD boot without retained reset-cause flags. */
bool reset_was_power_on() {
    return BOOT_Reason() == 0U;
}

/** @brief Decodes the SDK's boot-reason flags of either AmebaD core. */
RestartReason restart_reason() {
    const auto reason = BOOT_Reason();
    if (reason == 0U)
        return RestartReason::power_on;
    if ((reason & BIT_BOOT_DSLP_RESET_HAPPEN) != 0U)
        return RestartReason::deep_sleep;
    if ((reason & BIT_BOOT_BOD_RESET_HAPPEN) != 0U)
        return RestartReason::brownout;
    if ((reason & (BIT_BOOT_KM4WDG_RESET_HAPPEN | BIT_BOOT_WDG_RESET_HAPPEN)) != 0U)
        return RestartReason::watchdog;
    if ((reason & (BIT_BOOT_KM4SYS_RESET_HAPPEN | BIT_BOOT_SYS_RESET_HAPPEN)) != 0U)
        return RestartReason::software;
    return RestartReason::unknown;
}

/**
 * @brief Counts quick application boots in the SDK's user backup register.
 * Only software and watchdog resets reuse a valid record. Power-on, brownout,
 * deep-sleep wakes and unknown causes start fresh even if a word survived.
 * The single-word record avoids partially updated magic/count pairs.
 */
FastRestartInfo begin_fast_restart_tracking() {
    const auto reason = restart_reason();
    const bool warm_reset = reason == RestartReason::software || reason == RestartReason::watchdog;
    const std::uint32_t record = BKUP_Read(kFastRestartRegister);
    std::uint8_t count = 1U;
    if (warm_reset && valid_fast_restart_record(record)) {
        const auto previous = static_cast<std::uint8_t>(record);
        count = previous == UINT8_MAX ? UINT8_MAX : static_cast<std::uint8_t>(previous + 1U);
    }
    if (!store_fast_restart_count(count))
        return {};
    return {count, true};
}

/** @brief Clears the count after stable execution or before the Safeboot handoff. */
void mark_boot_stable() {
    (void)store_fast_restart_count(0U);
}

/** @brief Delays the current FreeRTOS task for at least one scheduler tick. */
/** @brief Uses the native SDK busy wait for peripheral edge timing. */
void delay_microseconds(const std::uint32_t duration) {
    DelayUs(duration);
}

void sleep_for_milliseconds(const std::uint32_t duration) {
    auto ticks = static_cast<TickType_t>(duration / portTICK_PERIOD_MS);
    if ((duration % portTICK_PERIOD_MS) != 0) {
        ++ticks;
    }
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

/** @brief Accepts console initialization because LOGUART is SDK-initialized. */
void initialize_console() {}

/** @brief Describes the AmebaD LOGUART console backend. */
const char* console_status() {
    return "Using LOGUART";
}

/** @brief Reports the qualified AmebaD LOGUART default. */
std::uint32_t console_baudrate() {
    return kLogUartBaudrate;
}

/** @brief Writes one application log line through LOGUART. */
void write_log(const std::string_view message) {
    for (const char character : message) {
        LOGUART_PutChar(static_cast<unsigned char>(character));
    }
    LOGUART_PutChar('\r');
    LOGUART_PutChar('\n');
}

/** @brief Reads one pending byte from the AmebaD ROM logging UART. */
bool read_console_character(char& character) {
    if (LOGUART_Readable() == 0U) {
        return false;
    }
    character = static_cast<char>(LOGUART_GetChar(0U));
    return true;
}

/** @brief Writes command response text through LOGUART. */
bool write_console(const std::string_view text) {
    for (const char character : text) {
        LOGUART_PutChar(static_cast<unsigned char>(character));
    }
    return true;
}

/**
 * @brief Selects the opposite AmebaD image using the paired signature scheme.
 *
 * OTA2 is the normal application and is preferred by the vendor bootloader
 * when both signatures are valid.  The signature update is intentionally
 * ordered so a reset or power loss leaves at least the current image valid.
 */
BootPartitionSwitchResult prepare_boot_partition_toggle(AlternateImage) {
    const bool ota1_valid = valid_image_signature(kOta1Offset);
    const bool ota2_valid = valid_image_signature(kOta2Offset);
    const BootTogglePlan plan = boot_toggle_plan(ota1_valid, ota2_valid);
    if (!plan.available()) {
        return BootPartitionSwitchResult::unavailable;
    }

    const std::uint32_t target_offset = image_offset(plan.target);
    const std::uint32_t current_offset = image_offset(plan.current);
    if (plan.restore_target &&
        (!restore_image(target_offset) || !valid_image_signature(target_offset))) {
        return BootPartitionSwitchResult::failed;
    }
    if (!invalidate_image(current_offset)) {
        return BootPartitionSwitchResult::failed;
    }

    return valid_image_signature(target_offset) && !valid_image_signature(current_offset)
               ? BootPartitionSwitchResult::prepared
               : BootPartitionSwitchResult::failed;
}

/** @brief Reports unavailable until AmebaD calibration storage is qualified. */
RadioCalibrationResetResult erase_radio_calibration() {
    return RadioCalibrationResetResult::unavailable;
}

/** @brief Restarts the AmebaD system and does not return. */
[[noreturn]] void restart() {
    sys_reset();
    while (true) {}
}

} // namespace newmota::platform
