/*
  web_service.h - portable Compact browser service for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/platform/web.h"
#include "core/storage/filesystem.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace newmota {

class MqttService;
class MqttClient;
class Settings;
class CommandProcessor;
class MessageRouter;
class BootCounter;
class NetworkService;
class UpdateWebService;
class BerryService;
class LightService;
class TimeService;

class WebService final {
  public:
    /** @brief Binds web behavior to the application-owned settings and command services. */
    WebService(Settings& settings, CommandProcessor& commands, MessageRouter& messages,
        NetworkService* network = nullptr, UpdateWebService* updates = nullptr,
        BerryService* berry = nullptr, BootCounter* boot_counter = nullptr);
    WebService(const WebService&) = delete;
    WebService& operator=(const WebService&) = delete;
    /** @brief Releases unfinished Berry source and filesystem staging handles. */
    ~WebService();
    /**
     * @brief Starts on either local Wi-Fi interface and handles one request.
     * @return true when a request was handled, so an idle wait can end.
     */
    bool poll(std::uint32_t now, bool network_ready);
    /** @brief Handles an owned exchange on the application context, without network I/O. */
    void handle(platform::WebExchange& exchange, std::uint32_t now);

    /** @brief Binds optional MQTT diagnostics without owning a second connection. */
    void set_mqtt_services(MqttService* service, MqttClient* client) {
        mqtt_service_ = service;
        mqtt_client_ = client;
    }

    /** @brief Binds the Light owner shown by the Home light controls; null hides them. */
    void set_light(LightService* light) { light_ = light; }

    /** @brief Binds the clock whose synchronization the timer editor reports. */
    void set_time(const TimeService* time) { time_ = time; }

    /** @brief Prevents broker tests while an outbound transfer reserves the shared stream. */
    void set_mqtt_test_allowed(bool allowed) { mqtt_test_allowed_ = allowed; }

    /** @brief Excludes browser filesystem transfers while an update or download reserves storage.
     */
    void set_filesystem_access_allowed(bool allowed) { filesystem_access_allowed_ = allowed; }

    [[nodiscard]] bool filesystem_upload_active() const {
        return filesystem_upload_.writer.native != nullptr;
    }

  private:
    struct Session final {
        std::array<char, 33> token{}, csrf{};
        std::uint32_t last_used{}, rate_start{};
        unsigned int requests{};
    };

    struct FilesystemUpload final {
        filesystem::FileWriter writer{};
        std::array<char, 33> id{}, owner_session{};
        std::array<char, 192> path{};
        std::size_t total{}, received{};
        std::uint32_t last_activity{};
    };

    void clear_filesystem_upload();
    void expire_filesystem_upload(std::uint32_t now);
    bool filesystem_access_allowed_{true};
    FilesystemUpload filesystem_upload_{};

    struct BerryConsoleUpload final {
        char* source{};
        std::array<char, 33> id{}, owner_session{};
        std::size_t total{}, received{};
        std::uint32_t last_activity{};
    };

    /** @brief Finds an unexpired session cookie; expired slots are released. */
    Session* session(std::string_view cookie, std::uint32_t now);
    /** @brief Clears all sessions whenever the provisioned credential changes. */
    void refresh_credential();
    /** @brief Releases any pending in-memory Berry source upload. */
    void clear_berry_upload();
    /** @brief Releases a pending upload after its idle deadline. */
    void expire_berry_upload(std::uint32_t now);

    MqttService* mqtt_service_{};
    MqttClient* mqtt_client_{};
    bool mqtt_test_allowed_{true};
    Settings& settings_;
    CommandProcessor& commands_;
    MessageRouter& messages_;
    BootCounter* boot_counter_{};
    NetworkService* network_{};
    UpdateWebService* updates_{};
    BerryService* berry_{};
    LightService* light_{};
    const TimeService* time_{};
    std::array<Session, 2> sessions_{};
    BerryConsoleUpload berry_upload_{};
    std::array<char, 64> credential_{};
    std::size_t credential_size_{};
    std::array<char, 33> console_history_generation_{};
    std::uint32_t login_window_{}, last_start_{};
    unsigned int login_attempts_{};
    bool started_{};
    bool attempted_start_{};
};

} // namespace newmota
