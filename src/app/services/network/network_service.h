/*
  network_service.h - portable Wi-Fi policy for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/platform/network.h"
#include "newmota_features.h"
#include "services/network/network_identity.h"
#if USE_ETHERNET
#include "services/network/ethernet_service.h"
#endif

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace newmota {

class Settings;
class RestartController;

/** @brief Why the application-owned SoftAP is currently running. */
enum class AccessPointPurpose : std::uint8_t {
    none,
    onboarding,
    recovery,
};

/** @brief State of the shared temporary station-credential test. */
enum class ProvisioningState : std::uint8_t {
    idle,
    testing,
    succeeded,
    failed,
};

/** @brief Result detail retained for the last temporary credential test. */
enum class ProvisioningFailure : std::uint8_t {
    none,
    connect_failed,
    no_ip_address,
    ap_unavailable,
    ap_timeout,
};

/** @brief State of the shared asynchronous Wi-Fi scan cache. */
enum class WifiScanState : std::uint8_t {
    idle,
    scanning,
    complete,
    failed,
};

/** @brief Independent readers for asynchronous scan-result announcements. */
enum class WifiScanDelivery : std::uint8_t {
    console,
    message,
};

/** @brief Identifies which surface owns the active credential test. */
enum class CredentialTestOwner : std::uint8_t {
    none,
    onboarding,
    command,
};

/** @brief Converts a dBm value to Tasmota's 0..100 signal-quality scale. */
[[nodiscard]] constexpr int wifi_rssi_quality(const std::int8_t rssi) {
    return rssi <= -100 ? 0 : rssi >= -50 ? 100 : 2 * (static_cast<int>(rssi) + 100);
}

class NetworkService final {
  public:
    /** @brief Binds Wi-Fi policy to the persistent settings service. */
    explicit constexpr NetworkService(Settings& settings, RestartController* restart = nullptr)
        : settings_(settings), restart_(restart) {}

    /** @brief Initializes Wi-Fi and enters AP fallback when STA is unusable. */
    void initialize(std::uint32_t now);
    /** @brief Advances station failover and SoftAP lifecycle policy. */
    void poll(std::uint32_t now);

    /** @brief Reports whether station mode currently owns an IP address. */
    [[nodiscard]] bool connected() const { return connected_; }

    /** @brief Reports whether any interface, the station or Ethernet, owns an IP address. */
    [[nodiscard]] bool online() const {
#if USE_ETHERNET
        return connected_ || ethernet::connected();
#else
        return connected_;
#endif
    }

    /** @brief Returns the zero-based SSID slot of the station connection. */
    [[nodiscard]] std::uint8_t station_slot() const { return slot_; }

#if USE_TASMOTA_STATE
    /** @brief Tasmota's LinkCount: station connections since boot. */
    [[nodiscard]] std::uint32_t link_count() const { return link_count_; }

    /** @brief Tasmota's Downtime: seconds disconnected after an established link was lost. */
    [[nodiscard]] std::uint32_t downtime_seconds() const { return downtime_ms_ / 1000U; }

    /** @brief Returns the station link refreshed about once per second, or nullptr. */
    [[nodiscard]] const platform::WifiLink* link() const {
        return connected_ && link_valid_ ? &link_ : nullptr;
    }

    /** @brief Returns the station RSSI in dBm refreshed with link(), if known. */
    [[nodiscard]] std::optional<std::int8_t> link_rssi() const {
        return connected_ ? link_rssi_ : std::nullopt;
    }

    /** @brief Returns the SSID of the station connection, saved slot or credential test. */
    [[nodiscard]] std::string_view station_ssid() const { return station_ssid_.data(); }
#endif

    /** @brief Reports whether the SoftAP currently owns its local interface. */
    [[nodiscard]] bool access_point() const { return access_point_; }

    /** @brief Reports whether any interface can serve local clients. */
    [[nodiscard]] bool available() const { return online() || access_point_ready_; }

    /** @brief Reports whether the AP is the first-boot onboarding surface. */
    [[nodiscard]] bool onboarding() const {
        return access_point_ready_ && !ap_restarting_ &&
               access_point_purpose_ == AccessPointPurpose::onboarding;
    }

    /** @brief Reports whether the AP is a recovery surface after STA failure. */
    [[nodiscard]] bool recovery() const {
        return access_point_ready_ && !ap_restarting_ &&
               access_point_purpose_ == AccessPointPurpose::recovery;
    }

    /** @brief Reports whether this adapter can test STA credentials beside AP. */
    [[nodiscard]] bool provisioning_supported() const {
        return access_point_ && station_access_point_supported_ && (onboarding() || recovery());
    }

    /** @brief Returns the resolved runtime hostname used by the SoftAP. */
    [[nodiscard]] std::string_view access_point_ssid() const { return identity_.host(); }

    /** @brief Returns the resolved station/AP hostname. */
    [[nodiscard]] std::string_view hostname() const { return identity_.host(); }

    /** @brief Returns the resolved MQTT base topic. */
    [[nodiscard]] std::string_view mqtt_topic() const { return identity_.topic(); }

    /** @brief Re-resolves the current MQTT template without rebooting Wi-Fi. */
    [[nodiscard]] bool refresh_mqtt_topic(std::string_view topic_template);

    /** @brief Returns the current temporary credential-test state. */
    [[nodiscard]] ProvisioningState provisioning_state() const { return provisioning_state_; }

    /** @brief Reports whether a successful test has queued the restart handoff. */
    [[nodiscard]] bool restart_scheduled() const { return restart_scheduled_; }

    /** @brief Returns the current scan state. */
    [[nodiscard]] WifiScanState wifi_scan_state() const { return scan_state_; }

    /** @brief Reports whether a native scan still owns the radio. */
    [[nodiscard]] bool wifi_scan_pending() const { return scan_native_pending_; }

    /** @brief Returns the current Tasmota WifiTest command status. */
    [[nodiscard]] ProvisioningState wifi_test_state() const { return wifi_test_state_; }

    /** @brief Returns the last Tasmota WifiTest error detail. */
    [[nodiscard]] ProvisioningFailure wifi_test_failure() const { return wifi_test_failure_; }

    /** @brief Returns the sorted results from the most recent completed scan. */
    [[nodiscard]] const platform::WifiScanRecord* wifi_scan_results(std::size_t& count) const {
        count = scan_result_count_;
        return scan_records();
    }

    /** @brief Peeks the next result for one async-announcement reader. */
    [[nodiscard]] bool next_wifi_scan_announcement(WifiScanDelivery delivery,
        std::size_t& network_number, const platform::WifiScanRecord*& result) const;
    /** @brief Advances one reader after its announcement was accepted. */
    void acknowledge_wifi_scan_announcement(WifiScanDelivery delivery);
    /** @brief Starts a scan or reports that another radio operation is active. */
    [[nodiscard]] platform::NetworkResult start_wifi_scan(std::uint32_t now);
    /** @brief Starts an AP-independent Tasmota test with index 0, 1, 2, or 3. */
    [[nodiscard]] platform::NetworkResult start_wifi_test(
        std::string_view ssid, std::string_view password, std::uint16_t index, std::uint32_t now);

    /** @brief Stores credentials already proven by a temporary test in the first slot. */
    [[nodiscard]] bool commit_wifi_credentials(std::string_view ssid, std::string_view password);

    /** @brief Returns the retained terminal credential-test error detail. */
    [[nodiscard]] ProvisioningFailure provisioning_failure() const { return provisioning_failure_; }

    /** @brief Resets Tasmota's saved-test inactivity restart deadline. */
    void note_command_activity(std::uint32_t now);
    /** @brief Starts a temporary STA test without changing persistent settings. */
    [[nodiscard]] platform::NetworkResult start_provisioning_test(std::string_view ssid,
        std::string_view password, std::string_view web_password, std::uint32_t now);

  private:
    /** @brief Advances failover and AP policy; poll() wraps it to observe link changes. */
    void poll_link(std::uint32_t now);
    /** @brief Starts the next configured SSID slot. */
    void connect(std::uint32_t now);
    /** @brief Selects the next non-empty SSID slot. */
    [[nodiscard]] bool select_next_slot();
    /** @brief Starts the generated local AP used for unconfigured/failing Wi-Fi. */
    void start_access_point(AccessPointPurpose purpose, std::uint32_t now);
    /** @brief Retries AP startup and recovers interfaces that never become ready. */
    void maintain_access_point(std::uint32_t now);
    /** @brief Whether a wired address keeps the AP down, as in Tasmota. */
    [[nodiscard]] bool ethernet_hides_access_point() const;
    /** @brief Stops the AP while ethernet_hides_access_point() holds. */
    void hide_access_point(std::uint32_t now);
    /** @brief Marks candidate failure and resumes AP recovery. */
    void fail_provisioning(ProvisioningFailure failure = ProvisioningFailure::connect_failed);
    /** @brief Commits a validated candidate and schedules the delayed restart. */
    void finish_provisioning(std::uint32_t now);
    /** @brief Starts one candidate exchange and snapshots its persistence policy. */
    [[nodiscard]] platform::NetworkResult begin_provisioning_test(std::string_view ssid,
        std::string_view password, std::string_view web_password, std::uint8_t slot, bool persist,
        bool select, bool restart, bool update_web_password, CredentialTestOwner owner,
        std::uint32_t now);

    Settings& settings_;
    RestartController* restart_{};
    network_identity::Resolved identity_{};
    std::uint32_t device_id_{};
    std::uint32_t attempt_deadline_{};
    std::uint32_t provisioning_deadline_{};
    std::uint32_t ap_retry_deadline_{};
    std::uint32_t ap_ready_deadline_{};
    bool ap_restarting_{};
    std::uint8_t slot_{};
    std::uint8_t attempted_slots_{};
    bool enabled_{};
    bool attempted_{};
    bool connected_{};
#if USE_TASMOTA_STATE
    // Radio queries run at most once per second, not with every STATE rebuild.
    platform::WifiLink link_{};
    std::optional<std::int8_t> link_rssi_{};
    bool link_valid_{};
    std::uint32_t link_at_{}, lost_at_{}, downtime_ms_{}, link_count_{};
    bool link_lost_{};
    std::array<char, 33> station_ssid_{};
#endif
    /** @brief Starts a station join and remembers its SSID for STATE. */
    [[nodiscard]] platform::NetworkResult join(std::string_view ssid, std::string_view password);
    bool access_point_{};
    bool access_point_ready_{};
    bool station_access_point_supported_{};
    AccessPointPurpose access_point_purpose_{AccessPointPurpose::none};
    ProvisioningState provisioning_state_{ProvisioningState::idle};
    ProvisioningFailure provisioning_failure_{ProvisioningFailure::none};
    std::array<char, 33> candidate_ssid_{};
    std::array<char, 65> candidate_password_{};
    std::array<char, 65> candidate_web_password_{};
    using ScanResults = std::array<platform::WifiScanRecord, platform::kMaximumWifiScanResults>;
    /** @brief Allocates the result array when a scan starts. */
    [[nodiscard]] bool allocate_scan_results();
    /** @brief Frees the result array once no scan or retained result needs it. */
    void release_scan_results();
#if defined(NEWMOTA_IMAGE_SAFEBOOT)
    // Safeboot keeps it static: its binary size matters more than its free heap.
    ScanResults scan_results_{};

    [[nodiscard]] platform::WifiScanRecord* scan_records() { return scan_results_.data(); }

    [[nodiscard]] const platform::WifiScanRecord* scan_records() const {
        return scan_results_.data();
    }
#else
    std::unique_ptr<ScanResults> scan_results_{};

    [[nodiscard]] platform::WifiScanRecord* scan_records() {
        return scan_results_ ? scan_results_->data() : nullptr;
    }

    [[nodiscard]] const platform::WifiScanRecord* scan_records() const {
        return scan_results_ ? scan_results_->data() : nullptr;
    }
#endif
    std::size_t scan_result_count_{};
    std::uint32_t scan_cache_deadline_{};
    std::uint32_t scan_deadline_{};
    WifiScanState scan_state_{WifiScanState::idle};
    bool scan_native_pending_{};
    bool scan_timed_out_{};
    std::size_t scan_console_cursor_{};
    std::size_t scan_message_cursor_{};
    bool scan_console_empty_pending_{};
    bool scan_message_empty_pending_{};
    std::uint8_t candidate_slot_{};
    bool candidate_persist_{};
    bool candidate_select_{};
    bool candidate_restart_{};
    bool candidate_update_web_password_{};
    bool candidate_connect_started_{};
    CredentialTestOwner active_test_owner_{CredentialTestOwner::none};
    ProvisioningState wifi_test_state_{ProvisioningState::idle};
    ProvisioningFailure wifi_test_failure_{ProvisioningFailure::none};
    std::uint32_t wifi_test_restart_deadline_{};
    bool wifi_test_restart_pending_{};
    bool restart_scheduled_{};
};

} // namespace newmota
