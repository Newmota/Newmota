/*
  console.h - ESP-IDF console interface for Newmota

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

class Console final {
  public:
    /**
     * @brief Selects the boot-time console backend and redirects SDK logging.
     */
    void initialize();

    /**
     * @brief Reads one character without blocking.
     * @return true when @p character was populated.
     */
    [[nodiscard]] bool read_character(char& character);
    /**
     * @brief Writes a complete string through the selected backend.
     * @return true when all bytes were accepted.
     */
    [[nodiscard]] bool write(std::string_view text);
    /**
     * @brief Describes the selected console backend.
     */
    [[nodiscard]] const char* status() const;
    /**
     * @brief Returns the configured UART baud rate, or zero for USB backends.
     */
    [[nodiscard]] std::uint32_t baudrate() const;

  private:
    enum class Backend : std::uint8_t {
        uninitialized,
        uart0,
        usb_cdc,
        usb_serial_jtag,
    };

    /**
     * @brief Installs and configures the UART0 fallback.
     * @return true when UART0 is ready.
     */
    [[nodiscard]] bool initialize_uart();

    Backend backend_{Backend::uninitialized};
    bool usb_fallback_{false};
};

/**
 * @brief Returns the process-wide ESP-IDF console instance.
 */
[[nodiscard]] Console& console();

} // namespace newmota::platform
