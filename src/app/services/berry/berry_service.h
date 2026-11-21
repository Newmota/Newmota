/*
  berry_service.h - Berry runtime service for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/command/command.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

struct bvm;

namespace newmota {

/** @brief Raises when the active protected Berry execution deadline expires. */
void check_berry_execution_deadline(bvm* vm);

/** @brief Reports an expired deadline without raising, for native loops that must clean up. */
[[nodiscard]] bool berry_execution_deadline_reached();

class LightService;
class TimeService;
class TimerService;
class Hardware;
class BerryMqttBridge;
struct MqttPublishPacket;

inline constexpr std::size_t kBerryConsoleMaxSourceBytes = 64U * 1024U;
inline constexpr std::size_t kBerryConsoleBufferOverhead = 16U;

enum class BerryScriptResult : std::uint8_t {
    executed,
    not_found,
    unavailable,
    failed,
};

enum class BerryReplResult : std::uint8_t {
    executed,
    invalid,
    unavailable,
    failed,
    no_memory,
};

class BerryService final {
  public:
    /** @brief Releases the Berry VM when the service lifetime ends. */
    ~BerryService();

    BerryService(const BerryService&) = delete;
    BerryService& operator=(const BerryService&) = delete;

    /** @brief Constructs an inactive Berry service without allocating a VM. */
    constexpr BerryService() = default;

    /**
     * @brief Creates the persistent VM and enables strict named-global compilation.
     * @return true when Berry is ready to execute commands.
     */
    [[nodiscard]] bool initialize(CommandProcessor& commands, LightService* light = nullptr,
        BerryMqttBridge* mqtt = nullptr, TimeService* time = nullptr,
        const Hardware* hardware = nullptr, TimerService* timers = nullptr);

    /**
     * @brief Executes one Tasmota-compatible Br command in the persistent VM.
     * @return The command status after writing its JSON response.
     */
    [[nodiscard]] CommandStatus command(const CommandRequest& request, CommandOutput& output);

    /** @brief Evaluates raw Berry source with Berry REPL output semantics. */
    [[nodiscard]] BerryReplResult repl(std::string_view source);

    /** @brief Evaluates and takes ownership of a possibly chunk-uploaded buffer. */
    [[nodiscard]] BerryReplResult repl_owned(
        char* source, std::size_t source_size, std::size_t buffer_capacity);

    /**
     * @brief Streams, compiles, and executes one LittleFS Berry script.
     * @return The script execution outcome with missing storage distinguished.
     */
    [[nodiscard]] BerryScriptResult execute_file(const char* path);

    /** @brief Runs MQTT callbacks under the VM deadline; errors consume delivery. */
    [[nodiscard]] bool mqtt_publish(const MqttPublishPacket& packet);

#if USE_MATTER
    /**
     * @brief Handles MtrInfo, MtrJoin and MtrReset through the running Matter stack.
     * @return unknown for another command, otherwise the command status.
     */
    [[nodiscard]] CommandStatus matter_command(
        const CommandRequest& request, CommandOutput& output);
#endif

#if USE_CAMERA
    /**
     * @brief Handles Tasmota's Webcam and Wc* commands through the registered sensor driver.
     * @return unknown for another command, otherwise the command status.
     */
    [[nodiscard]] CommandStatus camera_command(
        const CommandRequest& request, CommandOutput& output);
#endif
#if USE_BERRY_DRIVERS
    /**
     * @brief Runs a command registered with `newmota.add_cmd()`, after every native command.
     * @return unknown when no registered name matches, otherwise the command status.
     */
    [[nodiscard]] CommandStatus script_command(
        const CommandRequest& request, CommandOutput& output);
#endif
    /** @brief Dispatches deferred callbacks and timers due on a scheduler tick. */
    void poll(std::uint32_t now, bool timer_tick);

    /** @brief Saves imported persist state before a controlled restart, logging failures. */
    void save_persist();

    /** @brief Reports whether the persistent Berry VM is available. */
    [[nodiscard]] bool ready() const { return vm_ != nullptr; }

    /**
     * @brief Reports Tasmota's STATE Berry fields from the last collection; unlike Tasmota it
     * does not collect first, because STATE is rebuilt every 100 ms.
     * @return false without a VM.
     */
    [[nodiscard]] bool memory(std::uint32_t& heap_used_kb, std::uint32_t& objects) const;

  private:
    bvm* vm_{nullptr};
    TimerService* timers_{};
};

} // namespace newmota
