/*
  display_service.cpp - descriptor-driven display startup

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file display_service.cpp
 * @brief Loads UDisplay descriptors, resolves configured pins, and retains display ownership.
 */

#include "newmota_features.h"
#include "drivers/display/display_service.h"

#include "core/hardware/hardware.h"
#include "core/hardware/runtime_pin_claims.h"
#include "core/storage/filesystem.h"
#include "gpio_ids.h"
#include "newmota/log.h"
#include "newmota/platform/display.h"
#if USE_UNIVERSAL_TOUCH
#include "drivers/i2c/i2c_device_claims.h"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>

namespace newmota {
namespace {

constexpr char kDescriptorPath[] = "/display.ini";
constexpr std::size_t kDescriptorCapacity = 8192U;
constexpr std::size_t kErrorCapacity = 160U;
constexpr std::size_t kMaxHeaderFields = 48U;

/** @brief Owns descriptor text and parsing workspace only for display startup. */
struct InitializationWorkspace {
    std::array<char, kDescriptorCapacity> descriptor{};
    std::array<char, kDescriptorCapacity> resolved_descriptor{};
    std::array<char, kErrorCapacity> error{};
    std::array<PinId, 40> authorized_configured_pins{};
    platform::display::Info parsed_info{};
};

bool initialization_attempted{};
#if USE_UNIVERSAL_TOUCH
i2c_devices::Lease touch_lease{};
#endif

struct Writer {
    char* data{};
    std::size_t capacity{};
    std::size_t size{};

    bool append(const std::string_view value) {
        if (value.size() > capacity - size)
            return false;
        std::memcpy(data + size, value.data(), value.size());
        size += value.size();
        return true;
    }

    bool append(char value) {
        if (size == capacity)
            return false;
        data[size++] = value;
        return true;
    }
};

/** @brief Converts a display result into a short stable diagnostic. */
const char* result_name(const platform::display::Result result) {
    using platform::display::Result;
    switch (result) {
    case Result::success:
        return "success";
    case Result::invalid_descriptor:
        return "invalid descriptor";
    case Result::unsupported:
        return "unsupported interface";
    case Result::busy:
        return "resource busy";
    case Result::no_memory:
        return "out of memory";
    case Result::failed:
        return "hardware failure";
    case Result::timeout:
        return "timeout";
    }
    return "unknown error";
}

/** @brief Removes descriptor field whitespace without allocating. */
std::string_view trim(std::string_view value) {
    while (
        !value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
        value.remove_suffix(1);
    return value;
}

/** @brief Resolves one generated GPIO function to its unique active assignment. */
bool assigned_pin(const Hardware& hardware, std::uint16_t base, std::uint8_t instance, PinId& pin,
    bool& assigned) {
    const auto function = generated::gpio::encode(base, instance);
    const PinBinding* match{};
    std::size_t matches{};
    for (std::size_t index = 0; index < hardware.binding_count(); ++index) {
        const auto* binding = hardware.binding(index);
        if (binding->function == function) {
            match = binding;
            ++matches;
        }
    }
    assigned = matches != 0U;
    if (!assigned) {
        pin = PinId{};
        return true;
    }
    if (matches != 1U || !match || !match->active())
        return false;
    pin = match->pin;
    return true;
}

/** @brief Adds a resolved configured pad to the descriptor's authorized set. */
bool authorize_pin(InitializationWorkspace& workspace, PinId pin, std::size_t& count) {
    if (!pin.valid())
        return true;
    for (std::size_t index = 0; index < count; ++index)
        if (workspace.authorized_configured_pins[index].value() == pin.value())
            return true;
    if (count == workspace.authorized_configured_pins.size())
        return false;
    workspace.authorized_configured_pins[count++] = pin;
    return true;
}

/** @brief Writes a pin value, authorizing it only when resolved from a wildcard. */
bool append_pin(InitializationWorkspace& workspace, Writer& writer, const Hardware& hardware,
    std::uint16_t base, std::uint8_t instance, std::size_t& authorized_count) {
    PinId pin{};
    bool assigned{};
    if (!assigned_pin(hardware, base, instance, pin, assigned))
        return false;
    char text[8]{};
    const int value = assigned ? static_cast<int>(pin.value()) : -1;
    const auto converted = std::to_chars(text, text + sizeof(text), value);
    if (converted.ec != std::errc{} ||
        !writer.append({text, static_cast<std::size_t>(converted.ptr - text)}))
        return false;
    return !assigned || authorize_pin(workspace, pin, authorized_count);
}

/** @brief Rewrites only Tasmota's configured-pin wildcards in a descriptor header. */
bool rewrite_header(InitializationWorkspace& workspace, std::string_view line, Writer& writer,
    const Hardware& hardware, std::size_t& authorized_count) {
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string_view::npos)
        return writer.append(line);
#if USE_UNIVERSAL_TOUCH
    const bool touch_header = line.substr(first, 5U) == ":UTI,";
#else
    constexpr bool touch_header = false;
#endif
    if (!touch_header && line.substr(first, 3U) != ":H,")
        return writer.append(line);
    const auto marker = first;
    const auto prefix = touch_header ? 5U : 3U;

    const auto comment = line.find(';', marker + prefix);
    const auto content_end = comment == std::string_view::npos ? line.size() : comment;
    const auto content = line.substr(marker + prefix, content_end - marker - prefix);
    std::array<std::string_view, kMaxHeaderFields> fields{};
    std::size_t field_count{};
    std::size_t offset{};
    do {
        if (field_count == fields.size())
            return false;
        const auto comma = content.find(',', offset);
        fields[field_count++] = content.substr(
            offset, comma == std::string_view::npos ? content.size() - offset : comma - offset);
        if (comma == std::string_view::npos)
            break;
        offset = comma + 1U;
    } while (offset <= content.size());

    if (field_count < 5U)
        return writer.append(line);
    const auto interface = touch_header ? std::string_view{} : trim(fields[4]);
    int spi_bus = 3;
    if (interface == "SPI" || interface == "EPD") {
        if (field_count > 5U) {
            const auto bus = trim(fields[5]);
            if (bus != "*") {
                const auto parsed = std::from_chars(bus.data(), bus.data() + bus.size(), spi_bus);
                if (parsed.ec != std::errc{} || parsed.ptr != bus.data() + bus.size())
                    spi_bus = 3;
            }
        }
    }
    const std::uint8_t spi_instance = spi_bus == 2 ? 2U : 1U;
    const bool software_spi = spi_bus < 1 || spi_bus > 2;

    if (!writer.append(line.substr(0, marker + prefix)))
        return false;
    for (std::size_t index = 0; index < field_count; ++index) {
        const auto value = trim(fields[index]);
        bool mapped{};
        std::uint16_t base{};
        std::uint8_t instance{1};
        if (value == "*") {
#if USE_UNIVERSAL_TOUCH
            if (touch_header && (index == 3U || index == 4U)) {
                mapped = true;
                base =
                    index == 3U ? generated::gpio::kGpioTsRstBase : generated::gpio::kGpioTsIrqBase;
            } else
#endif
                if ((interface == "SPI" || interface == "EPD") && index == 5U) {
                mapped = true;
                if (!writer.append("3"))
                    return false;
            } else if (interface == "SPI" || interface == "EPD") {
                if (index >= 6U && index <= 12U) {
                    mapped = true;
                    static constexpr std::uint16_t hardware_functions[]{
                        generated::gpio::kGpioSpiCsBase,
                        generated::gpio::kGpioSpiClkBase,
                        generated::gpio::kGpioSpiMosiBase,
                        generated::gpio::kGpioSpiDcBase,
                        generated::gpio::kGpioBacklightBase,
                        generated::gpio::kGpioDisplayResetBase,
                        generated::gpio::kGpioSpiMisoBase,
                    };
                    static constexpr std::uint16_t software_functions[]{
                        generated::gpio::kGpioSspiCsBase,
                        generated::gpio::kGpioSspiSclkBase,
                        generated::gpio::kGpioSspiMosiBase,
                        generated::gpio::kGpioSspiDcBase,
                        generated::gpio::kGpioBacklightBase,
                        generated::gpio::kGpioDisplayResetBase,
                        generated::gpio::kGpioSspiMisoBase,
                    };
                    base = software_spi ? software_functions[index - 6U]
                                        : hardware_functions[index - 6U];
                    instance = software_spi ? 1U : spi_instance;
                }
            } else if (interface == "QSPI" && (index == 12U || index == 13U)) {
                mapped = true;
                base = index == 12U ? generated::gpio::kGpioBacklightBase
                                    : generated::gpio::kGpioDisplayResetBase;
            } else if ((interface == "I2C" || interface == "I2C1" || interface == "I2C2") &&
                       index == 8U) {
                mapped = true;
                base = generated::gpio::kGpioDisplayResetBase;
            } else if ((interface == "PAR" || interface == "I80") && index == 6U) {
                mapped = true;
                base = generated::gpio::kGpioDisplayResetBase;
            } else if ((interface == "PAR" || interface == "I80") && index == 11U) {
                mapped = true;
                base = generated::gpio::kGpioBacklightBase;
            } else if (interface == "RGB" && index == 9U) {
                mapped = true;
                base = generated::gpio::kGpioBacklightBase;
            } else if (interface == "DSI" && index == 7U) {
                mapped = true;
                base = generated::gpio::kGpioBacklightBase;
            } else if (interface == "DSI" && index == 8U) {
                mapped = true;
                base = generated::gpio::kGpioDisplayResetBase;
            }
            if (mapped && index != 5U &&
                !append_pin(workspace, writer, hardware, base, instance, authorized_count))
                return false;
            if (!mapped && !writer.append(fields[index]))
                return false;
        } else if (!writer.append(fields[index])) {
            return false;
        }
        if (index + 1U < field_count && !writer.append(','))
            return false;
    }
    return writer.append(line.substr(content_end));
}

/** @brief Resolves wildcard pins while preserving the complete descriptor text. */
bool resolve_descriptor(InitializationWorkspace& workspace, const Hardware& hardware,
    std::size_t input_size, std::size_t& output_size, std::size_t& authorized_count) {
    const auto& descriptor = workspace.descriptor;
    Writer writer{workspace.resolved_descriptor.data(), workspace.resolved_descriptor.size()};
    authorized_count = 0U;
    std::size_t offset{};
    while (offset < input_size) {
        const auto newline =
            std::find(descriptor.begin() + offset, descriptor.begin() + input_size, '\n');
        const auto end = static_cast<std::size_t>(newline - descriptor.begin());
        if (!rewrite_header(workspace, {descriptor.data() + offset, end - offset}, writer, hardware,
                authorized_count))
            return false;
        if (end < input_size && !writer.append('\n'))
            return false;
        offset = end < input_size ? end + 1U : input_size;
    }
    output_size = writer.size;
    return true;
}

} // namespace

bool initialize_display_from_filesystem(const Hardware& hardware) {
    if (initialization_attempted)
        return platform::display::info() != nullptr;
    initialization_attempted = true;

    // Native parsing copies commands and tables into its own Config; neither describe()
    // nor open() retains a view into this operation's text or diagnostic buffers.
    const auto workspace =
        std::unique_ptr<InitializationWorkspace>{new (std::nothrow) InitializationWorkspace};
    if (!workspace) {
        AddLog(LogLevel::error, "DSP: startup workspace allocation failed");
        return false;
    }
    auto& descriptor = workspace->descriptor;
    auto& resolved_descriptor = workspace->resolved_descriptor;
    auto& error = workspace->error;
    auto& parsed_info = workspace->parsed_info;

    std::size_t descriptor_size{};
    const auto read_result = filesystem::read_file(
        kDescriptorPath, descriptor.data(), descriptor.size(), descriptor_size);
    if (read_result == filesystem::ReadResult::not_found) {
        AddLog(LogLevel::info, "DSP: %s not found; display remains inactive", kDescriptorPath);
        return false;
    }
    if (read_result != filesystem::ReadResult::succeeded || descriptor_size == 0U) {
        AddLog(LogLevel::error, "DSP: cannot read %s (%u)", kDescriptorPath,
            static_cast<unsigned int>(read_result));
        return false;
    }

    std::size_t resolved_size{}, authorized_count{};
    if (!resolve_descriptor(
            *workspace, hardware, descriptor_size, resolved_size, authorized_count)) {
        AddLog(LogLevel::error, "DSP: cannot resolve descriptor GPIO wildcards");
        return false;
    }
    error.fill('\0');
    const auto described = platform::display::describe(
        resolved_descriptor.data(), resolved_size, parsed_info, error.data(), error.size());
    if (described != platform::display::Result::success) {
        AddLog(LogLevel::error, "DSP: descriptor rejected: %s (%s)", result_name(described),
            error[0] == '\0' ? "no parser detail" : error.data());
        return false;
    }
    if (parsed_info.pin_count != 0U &&
        !runtime_pins::claim_display(runtime_pins::kDisplay, parsed_info.pins,
            parsed_info.pin_count, workspace->authorized_configured_pins.data(),
            authorized_count)) {
        AddLog(LogLevel::error, "DSP: descriptor pins conflict with configured hardware");
        return false;
    }

#if USE_UNIVERSAL_TOUCH
    if (parsed_info.touch_i2c_bus >= 0 && !i2c_devices::claim(parsed_info.touch_i2c_bus,
                                              parsed_info.touch_i2c_address, touch_lease)) {
        AddLog(LogLevel::error, "DSP: universal touch I2C slave is unavailable or already owned");
        return false;
    }
#endif
    error.fill('\0');
    const auto opened = platform::display::open(
        resolved_descriptor.data(), resolved_size, parsed_info, error.data(), error.size());
    if (opened != platform::display::Result::success) {
        AddLog(LogLevel::error, "DSP: initialization failed: %s (%s)", result_name(opened),
            error[0] == '\0' ? "no driver detail" : error.data());
        return false;
    }
    AddLog(LogLevel::info, "DSP: %s %ux%u initialized", parsed_info.name,
        static_cast<unsigned int>(parsed_info.width),
        static_cast<unsigned int>(parsed_info.height));
    return true;
}

} // namespace newmota
