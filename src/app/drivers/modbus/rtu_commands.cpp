/*
  rtu_commands.cpp - Persistent Modbus physical bus configuration commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file rtu_commands.cpp
 * @brief Persistent Modbus physical bus configuration commands.
 */

#include "drivers/modbus/rtu_service.h"
#include "core/command/command.h"
#include "support/text.h"
#include "support/number.h"
#include <array>
#include <algorithm>

namespace newmota::modbus {
CommandStatus dispatch(const CommandRequest& request, CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "ModbusBus"))
        return CommandStatus::unknown;
    const std::size_t index = request.has_index ? request.index : 1;
    if (!index || index > kMaximumBuses)
        return CommandStatus::invalid;
    if (request.is_query()) {
        (void)write_bus(output, index - 1);
        return CommandStatus::handled;
    }
    Result result{};
    if (request.payload == "0")
        result = close_bus(index - 1);
    else {
        auto text = request.payload;
        std::array<std::int32_t, 8> values{};
        values[6] = -1;
        const auto fields = std::count(text.begin(), text.end(), ',') + 1;
        if (fields != 6 && fields != 8)
            return CommandStatus::invalid;
        for (std::size_t i = 0; i < std::size_t(fields); ++i) {
            const auto comma = text.find(',');
            if ((i + 1 < std::size_t(fields)) != (comma != std::string_view::npos))
                return CommandStatus::invalid;
            if (!support::parse_signed_decimal(text.substr(0, comma), -1, 115200, values[i]))
                return CommandStatus::invalid;
            if (comma != std::string_view::npos)
                text.remove_prefix(comma + 1);
        }
        if (values[0] < 0 || values[0] >= PinId::kInvalid || values[1] < 0 ||
            values[1] >= PinId::kInvalid || values[2] >= PinId::kInvalid || values[3] < 300 ||
            values[4] < 0 || values[4] > 2 || values[5] < 1 || values[5] > 2 ||
            values[6] >= PinId::kInvalid || values[7] < 0 || values[7] > 1)
            return CommandStatus::invalid;
        BusConfiguration config{};
        config.uart.rx = PinId{static_cast<std::uint16_t>(values[0])};
        config.uart.tx = PinId{static_cast<std::uint16_t>(values[1])};
        config.transmit_enable = PinId{static_cast<std::uint16_t>(values[2])};
        config.receive_enable = PinId{static_cast<std::uint16_t>(values[6])};
        config.receive_active_level = values[7];
        config.uart.baud = values[3];
        config.uart.parity = static_cast<platform::serial::Parity>(values[4]);
        config.uart.stop_bits = values[5];
        result = configure_bus(index - 1, config);
    }
    (void)output.format("{\"ModbusBus\":%u,\"Result\":%u}", unsigned(index), unsigned(result));
    return result == Result::success ? CommandStatus::handled : CommandStatus::failed;
}
} // namespace newmota::modbus
