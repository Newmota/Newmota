/*
  network_service.cpp - portable Wi-Fi policy for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/network/network_service.h"

#include "core/system/restart.h"
#include "newmota/log.h"
#include "newmota/platform/network.h"
#include "newmota_features.h"
#include "services/settings/settings.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace newmota {
namespace {

constexpr std::uint32_t kConnectTimeoutMilliseconds = 20'000;
constexpr std::uint32_t kWifiTestTimeoutMilliseconds = 9'000;
constexpr std::uint32_t kWifiTestIdleRestartMilliseconds = 180'000;
constexpr std::uint32_t kOnboardingHandoffMilliseconds = 500;
constexpr std::uint32_t kApRetryMilliseconds = 1'000;
constexpr std::uint32_t kWifiScanCacheMilliseconds = 60'000;
constexpr std::uint32_t kWifiScanTimeoutMilliseconds = 30'000;

/** @brief Tests a wrapping millisecond deadline. */
[[nodiscard]] bool deadline_reached(const std::uint32_t now, const std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

/** @brief Treats an empty or unassigned IPv4 address as not network-ready. */
[[nodiscard]] bool has_address(const platform::NetworkInfo& info) {
    const std::string_view address{info.address};
    return !address.empty() && address != "0.0.0.0";
}

template <std::size_t N>
void copy_candidate(std::array<char, N>& destination, const std::string_view source) {
    destination.fill('\0');
    std::copy(source.begin(), source.end(), destination.begin());
}

} // namespace

/** @brief Initializes Wi-Fi and enters AP fallback when STA is unusable. */
void NetworkService::initialize(const std::uint32_t now) {
    const auto result = platform::initialize_wifi();
    if (result == platform::NetworkResult::failed ||
        result == platform::NetworkResult::unavailable) {
        AddLog(LogLevel::error, "WIF: station initialization failed");
        return;
    }
    const auto topic_template = settings_.mqtt_settings().topic();
    device_id_ = platform::device_id();
    if (!network_identity::resolve(topic_template, settings_.hostname(), device_id_, identity_)) {
        AddLog(LogLevel::error, "WIF: device identity resolution failed");
        identity_ = {};
    } else if (platform::set_hostname(identity_.host()) == platform::NetworkResult::failed) {
        AddLog(LogLevel::error, "WIF: native hostname rejected");
    }
    enabled_ = true;
    station_access_point_supported_ = platform::wifi_capabilities().station_access_point;
    slot_ = settings_.preferred_wifi_slot();
    attempted_slots_ = 0U;
    attempted_ = false;
    connected_ = false;
    access_point_ = false;
    access_point_ready_ = false;
    access_point_purpose_ = AccessPointPurpose::none;
    provisioning_state_ = ProvisioningState::idle;
    provisioning_failure_ = ProvisioningFailure::none;
    active_test_owner_ = CredentialTestOwner::none;
    wifi_test_state_ = ProvisioningState::idle;
    wifi_test_failure_ = ProvisioningFailure::none;
    candidate_connect_started_ = false;
    wifi_test_restart_pending_ = false;
    scan_state_ = WifiScanState::idle;
    scan_result_count_ = 0U;
    scan_native_pending_ = false;
    scan_timed_out_ = false;
    release_scan_results();
    scan_console_cursor_ = 0U;
    scan_message_cursor_ = 0U;
    scan_console_empty_pending_ = false;
    scan_message_empty_pending_ = false;
    restart_scheduled_ = false;
    ap_restarting_ = false;
    ap_retry_deadline_ = now;
    ap_ready_deadline_ = now + kConnectTimeoutMilliseconds;
    if (settings_.ssid(slot_).empty()) {
        const auto alternate = static_cast<std::uint8_t>(slot_ == 0U ? 1U : 0U);
        if (!settings_.ssid(alternate).empty()) {
            slot_ = alternate;
        }
    }
    if (settings_.ssid(0).empty() && settings_.ssid(1).empty()) {
        AddLog(LogLevel::info, "WIF: no SSID configured; starting SoftAP");
        start_access_point(AccessPointPurpose::onboarding, now);
        return;
    }
    attempt_deadline_ = now;
    AddLog(LogLevel::info, "WIF: station/AP mode initialized");
}

/** @brief Re-resolves a persisted MQTT template for the current device. */
bool NetworkService::refresh_mqtt_topic(const std::string_view topic_template) {
    std::array<char, network_identity::kIdentityCapacity> resolved{};
    std::size_t resolved_size = 0U;
    if (!network_identity::resolve_topic(topic_template, device_id_, resolved, resolved_size)) {
        return false;
    }
    identity_.mqtt_topic = resolved;
    identity_.mqtt_topic_size = resolved_size;
    return true;
}

/** @brief Advances connection, failover, and reconnect policy. */
void NetworkService::poll(const std::uint32_t now) {
#if USE_TASMOTA_STATE
    const bool was_connected = connected_;
    poll_link(now);
    // STATE's Wifi block; Safeboot reports no link details.
    if (connected_ && !was_connected) {
        ++link_count_;
        if (link_lost_)
            downtime_ms_ += now - lost_at_;
        link_lost_ = false;
        link_at_ = now - 1000U; // refresh the link details below
    } else if (!connected_ && was_connected) {
        link_lost_ = true;
        lost_at_ = now;
        link_valid_ = false;
    }
    if (connected_ && now - link_at_ >= 1000U) {
        link_at_ = now;
        link_ = {};
        link_valid_ = platform::wifi_link(link_);
        link_rssi_ = platform::station_rssi();
    }
#else
    poll_link(now);
#endif
}

void NetworkService::poll_link(const std::uint32_t now) {
    if (!enabled_) {
        return;
    }
    if (scan_native_pending_) {
        std::size_t result_count = 0U;
        const auto scan_result = platform::wifi_scan_poll(
            scan_records(), platform::kMaximumWifiScanResults, result_count);
        if (scan_result == platform::NetworkResult::success) {
            scan_native_pending_ = false;
            if (scan_timed_out_) {
                scan_result_count_ = 0U;
                scan_state_ = WifiScanState::failed;
                scan_cache_deadline_ = now + kWifiScanCacheMilliseconds;
                release_scan_results();
            } else {
                scan_result_count_ = std::min(result_count, platform::kMaximumWifiScanResults);
                auto* const records = scan_records();
                std::sort(records, records + scan_result_count_,
                    [](const auto& left, const auto& right) { return left.rssi > right.rssi; });
                scan_state_ = WifiScanState::complete;
                scan_cache_deadline_ = now + kWifiScanCacheMilliseconds;
                scan_console_cursor_ = 0U;
                scan_message_cursor_ = 0U;
                scan_console_empty_pending_ = scan_result_count_ == 0U;
                scan_message_empty_pending_ = scan_result_count_ == 0U;
            }
        } else if (scan_result != platform::NetworkResult::busy) {
            scan_native_pending_ = false;
            scan_result_count_ = 0U;
            scan_state_ = WifiScanState::failed;
            scan_cache_deadline_ = now + kWifiScanCacheMilliseconds;
            release_scan_results();
        } else if (deadline_reached(now, scan_deadline_)) {
            if (!scan_timed_out_) {
                scan_timed_out_ = true;
                scan_state_ = WifiScanState::failed;
                scan_result_count_ = 0U;
                scan_cache_deadline_ = now + kWifiScanCacheMilliseconds;
            }
            (void)platform::wifi_scan_cancel();
            // Keep draining or retry cancellation until the adapter releases
            // its native scan. Do not leave every later radio operation busy.
            scan_deadline_ = now + kApRetryMilliseconds;
        }
    } else if ((scan_state_ == WifiScanState::complete || scan_state_ == WifiScanState::failed) &&
               deadline_reached(now, scan_cache_deadline_)) {
        scan_result_count_ = 0U;
        scan_state_ = WifiScanState::idle;
        release_scan_results();
        scan_console_cursor_ = 0U;
        scan_message_cursor_ = 0U;
        scan_console_empty_pending_ = false;
        scan_message_empty_pending_ = false;
    }
    if (wifi_test_restart_pending_ && restart_ != nullptr) {
        if (restart_->pending()) {
            wifi_test_restart_pending_ = false;
        } else if (deadline_reached(now, wifi_test_restart_deadline_)) {
            const auto result = restart_->schedule(RestartTarget::current, now);
            if (result == RestartRequestResult::scheduled) {
                wifi_test_restart_pending_ = false;
                restart_scheduled_ = true;
                AddLog(LogLevel::info, "WIF: restarting after saved credential test inactivity");
            } else if (result != RestartRequestResult::busy) {
                wifi_test_restart_deadline_ = now + kApRetryMilliseconds;
                AddLog(LogLevel::error, "WIF: deferred restart after credential test failed");
            }
        }
    }
    // Let the HTTP 202 response reach the browser before a non-concurrent
    // adapter leaves its setup AP to test the submitted station credentials.
    if (active_test_owner_ == CredentialTestOwner::onboarding && !station_access_point_supported_ &&
        !candidate_connect_started_ && deadline_reached(now, provisioning_deadline_)) {
        const auto ap_state = platform::wifi_snapshot().access_point;
        if (ap_state == platform::AccessPointState::active ||
            ap_state == platform::AccessPointState::initializing) {
            const auto stopped = platform::stop_access_point();
            if (stopped != platform::NetworkResult::success) {
                fail_provisioning(ProvisioningFailure::ap_unavailable);
                return;
            }
        }
        const auto result = join(candidate_ssid_.data(), candidate_password_.data());
        if (result != platform::NetworkResult::success) {
            fail_provisioning(ProvisioningFailure::connect_failed);
            return;
        }
        candidate_connect_started_ = true;
        provisioning_deadline_ =
            now + (candidate_update_web_password_ ? kConnectTimeoutMilliseconds
                                                  : kWifiTestTimeoutMilliseconds);
    }
    const auto snapshot = platform::wifi_snapshot();
    access_point_ = snapshot.access_point == platform::AccessPointState::active;
    access_point_ready_ = access_point_ && has_address(platform::access_point_info());
    const bool observed_ap_ready = access_point_ready_;
    const auto state = snapshot.station;
    const bool station_ready =
        state == platform::WifiState::connected && has_address(platform::network_info());
    if (active_test_owner_ != CredentialTestOwner::none) {
        connected_ = station_ready;
    } else if (wifi_test_state_ == ProvisioningState::succeeded) {
        if (connected_ && !station_ready) {
            AddLog(LogLevel::info, "WIF: tested station connection lost");
        }
        connected_ = station_ready;
    }
    const bool test_may_displace_ap =
        active_test_owner_ != CredentialTestOwner::none && !station_access_point_supported_;
    if (access_point_purpose_ != AccessPointPurpose::none &&
        (!scan_native_pending_ || station_access_point_supported_) && !test_may_displace_ap &&
        !(station_ready && !candidate_connect_started_ &&
            active_test_owner_ == CredentialTestOwner::none &&
            provisioning_state_ == ProvisioningState::idle &&
            wifi_test_state_ != ProvisioningState::succeeded &&
            access_point_purpose_ == AccessPointPurpose::recovery)) {
        if (ethernet_hides_access_point())
            hide_access_point(now);
        else
            maintain_access_point(now);
    }
    if (active_test_owner_ != CredentialTestOwner::none) {
        if (active_test_owner_ == CredentialTestOwner::onboarding &&
            station_access_point_supported_ && !observed_ap_ready) {
            fail_provisioning(ProvisioningFailure::ap_unavailable);
            return;
        }
        if (candidate_connect_started_ && state == platform::WifiState::connected &&
            has_address(platform::network_info())) {
            finish_provisioning(now);
            return;
        }
        if (deadline_reached(now, provisioning_deadline_)) {
            auto failure = ProvisioningFailure::connect_failed;
            if (state == platform::WifiState::connected) {
                failure = ProvisioningFailure::no_ip_address;
            } else if (active_test_owner_ == CredentialTestOwner::onboarding &&
                       station_access_point_supported_) {
                failure = ProvisioningFailure::ap_timeout;
            }
            fail_provisioning(failure);
        }
        return;
    }
    const bool failed_command_test_needs_ap =
        wifi_test_state_ == ProvisioningState::failed && candidate_connect_started_;
    const bool tested_station_lost =
        wifi_test_state_ == ProvisioningState::succeeded && !station_ready;
    if (access_point_purpose_ == AccessPointPurpose::none &&
        (failed_command_test_needs_ap || tested_station_lost) &&
        (!scan_native_pending_ || station_access_point_supported_)) {
        // A command test can run without the setup AP. If it fails, or its
        // temporary connection later disappears, restore a reachable AP
        // without accepting a stale candidate connection as configured Wi-Fi.
        start_access_point(AccessPointPurpose::recovery, now);
    }
    if (wifi_test_state_ == ProvisioningState::succeeded) {
        return;
    }
    if (provisioning_state_ == ProvisioningState::succeeded) {
        return;
    }
    // A native connect attempt may report a late success after its timeout.
    // Never let that stale candidate tear down the recovery AP or become the
    // active station without passing through the provisioning transaction.
    if (candidate_connect_started_) {
        return;
    }
    if (access_point_purpose_ == AccessPointPurpose::onboarding) {
        return;
    }
    if (station_ready) {
        if (!connected_) {
            connected_ = true;
            AddLog(LogLevel::info, "WIF: connected using SSID%u", static_cast<unsigned>(slot_ + 1));
        }
        if (access_point_purpose_ != AccessPointPurpose::none &&
            deadline_reached(now, ap_retry_deadline_)) {
            ap_retry_deadline_ = now + kApRetryMilliseconds;
            const auto stopped = platform::stop_access_point();
            const auto observed = platform::wifi_snapshot().access_point;
            if (stopped == platform::NetworkResult::success &&
                observed != platform::AccessPointState::active &&
                observed != platform::AccessPointState::initializing) {
                access_point_ = false;
                access_point_ready_ = false;
                access_point_purpose_ = AccessPointPurpose::none;
                ap_restarting_ = false;
                AddLog(LogLevel::info, "WIF: SoftAP stopped after station join");
            }
        }
        return;
    }
    if (connected_) {
        connected_ = false;
        attempted_ = false;
        attempted_slots_ = 0U;
        attempt_deadline_ = now;
        AddLog(LogLevel::info, "WIF: connection lost");
    }
    if (access_point_purpose_ != AccessPointPurpose::none ||
        snapshot.access_point == platform::AccessPointState::initializing || access_point_) {
        return;
    }
    if (state == platform::WifiState::initializing || !deadline_reached(now, attempt_deadline_)) {
        return;
    }
    if (attempted_ && !select_next_slot()) {
        start_access_point(AccessPointPurpose::recovery, now);
        return;
    }
    connect(now);
}

platform::NetworkResult NetworkService::join(
    const std::string_view ssid, const std::string_view password) {
#if USE_TASMOTA_STATE
    station_ssid_.fill('\0');
    std::copy_n(
        ssid.data(), std::min(ssid.size(), station_ssid_.size() - 1U), station_ssid_.data());
#endif
    return platform::connect_wifi(ssid, password);
}

/** @brief Starts the selected configured SSID slot. */
void NetworkService::connect(const std::uint32_t now) {
    if (scan_native_pending_) {
        attempt_deadline_ = now + 250U;
        return;
    }
    const auto result = join(settings_.ssid(slot_), settings_.password(slot_));
    if (result == platform::NetworkResult::busy) {
        attempt_deadline_ = now + 250;
        return;
    }
    attempted_ = true;
    attempted_slots_ =
        static_cast<std::uint8_t>(attempted_slots_ | static_cast<std::uint8_t>(1U << slot_));
    attempt_deadline_ = now + kConnectTimeoutMilliseconds;
    if (result == platform::NetworkResult::success) {
        AddLog(LogLevel::info, "WIF: connecting to SSID%u", static_cast<unsigned>(slot_ + 1));
    } else {
        AddLog(LogLevel::error, "WIF: SSID%u connection start failed",
            static_cast<unsigned>(slot_ + 1));
    }
}

/** @brief Starts a browser candidate test from onboarding or recovery AP. */
platform::NetworkResult NetworkService::start_provisioning_test(const std::string_view ssid,
    const std::string_view password, const std::string_view web_password, const std::uint32_t now) {
    return begin_provisioning_test(ssid, password, web_password, 0U, true, true, true, true,
        CredentialTestOwner::onboarding, now);
}

/** @brief Starts a Tasmota-style indexed credential test independently of AP. */
platform::NetworkResult NetworkService::start_wifi_test(const std::string_view ssid,
    const std::string_view password, const std::uint16_t index, const std::uint32_t now) {
    if (index > 3U)
        return platform::NetworkResult::failed;
    const auto slot = static_cast<std::uint8_t>(index == 2U ? 1U : 0U);
    const bool persist = index != 3U;
    const bool select = index == 0U;
    const bool restart = index == 0U;
    const auto result = begin_provisioning_test(ssid, password, {}, slot, persist, select, restart,
        false, CredentialTestOwner::command, now);
    if (result != platform::NetworkResult::success && result != platform::NetworkResult::busy) {
        wifi_test_state_ = ProvisioningState::failed;
        wifi_test_failure_ = ProvisioningFailure::connect_failed;
    }
    return result;
}

bool NetworkService::commit_wifi_credentials(
    const std::string_view ssid, const std::string_view password) {
    const auto result =
        settings_.set_wifi_configuration(0U, ssid, password, settings_.web_password(), true);
    if (result != SettingsUpdateResult::saved && result != SettingsUpdateResult::unchanged)
        return false;
    // The tested connection is now the configured one, and nobody restarts to end onboarding here.
    // The next poll treats it as a station that joined, which stops the AP.
    wifi_test_state_ = ProvisioningState::idle;
    slot_ = 0U;
    if (access_point_purpose_ == AccessPointPurpose::onboarding)
        access_point_purpose_ = AccessPointPurpose::recovery;
    return true;
}

/** @brief Resets the delayed restart deadline after a saved WifiTest. */
void NetworkService::note_command_activity(const std::uint32_t now) {
    if (wifi_test_restart_pending_) {
        wifi_test_restart_deadline_ = now + kWifiTestIdleRestartMilliseconds;
    }
}

/** @brief Starts a scan and retains completed results for one minute. */
platform::NetworkResult NetworkService::start_wifi_scan(const std::uint32_t now) {
    if (!enabled_)
        return platform::NetworkResult::unavailable;
    if (scan_native_pending_ || active_test_owner_ != CredentialTestOwner::none ||
        (scan_state_ == WifiScanState::complete &&
            (scan_console_cursor_ < scan_result_count_ ||
                scan_message_cursor_ < scan_result_count_ || scan_console_empty_pending_ ||
                scan_message_empty_pending_))) {
        return platform::NetworkResult::busy;
    }
    if (!allocate_scan_results()) {
        AddLog(LogLevel::error, "WIF: scan failed: Out of memory");
        scan_result_count_ = 0U;
        scan_state_ = WifiScanState::failed;
        scan_cache_deadline_ = now + kWifiScanCacheMilliseconds;
        return platform::NetworkResult::failed;
    }
    const auto result = platform::wifi_scan_start();
    if (result == platform::NetworkResult::success) {
        scan_result_count_ = 0U;
        scan_state_ = WifiScanState::scanning;
        scan_native_pending_ = true;
        scan_timed_out_ = false;
        scan_deadline_ = now + kWifiScanTimeoutMilliseconds;
        scan_console_cursor_ = 0U;
        scan_message_cursor_ = 0U;
        scan_console_empty_pending_ = false;
        scan_message_empty_pending_ = false;
    } else if (result != platform::NetworkResult::busy) {
        scan_result_count_ = 0U;
        scan_state_ = WifiScanState::failed;
        scan_cache_deadline_ = now + kWifiScanCacheMilliseconds;
        release_scan_results();
    } else if (scan_result_count_ == 0U) {
        // Keep the array while it still holds results from the last scan.
        release_scan_results();
    }
    return result;
}

bool NetworkService::allocate_scan_results() {
#if defined(NEWMOTA_IMAGE_SAFEBOOT)
    return true;
#else
    if (!scan_results_)
        scan_results_.reset(new (std::nothrow) ScanResults{});
    return scan_results_ != nullptr;
#endif
}

void NetworkService::release_scan_results() {
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
    scan_results_.reset();
#endif
}

/** @brief Peeks one retained scan result for console or MQTT delivery. */
bool NetworkService::next_wifi_scan_announcement(const WifiScanDelivery delivery,
    std::size_t& network_number, const platform::WifiScanRecord*& result) const {
    network_number = 0U;
    result = nullptr;
    if (scan_state_ != WifiScanState::complete)
        return false;
    const auto cursor =
        delivery == WifiScanDelivery::console ? scan_console_cursor_ : scan_message_cursor_;
    if (scan_result_count_ == 0U) {
        const bool pending = delivery == WifiScanDelivery::console ? scan_console_empty_pending_
                                                                   : scan_message_empty_pending_;
        return pending;
    }
    if (cursor >= scan_result_count_)
        return false;
    network_number = cursor + 1U;
    result = scan_records() + cursor;
    return true;
}

/** @brief Acknowledges one accepted asynchronous scan-result announcement. */
void NetworkService::acknowledge_wifi_scan_announcement(const WifiScanDelivery delivery) {
    if (scan_state_ != WifiScanState::complete)
        return;
    if (scan_result_count_ == 0U) {
        if (delivery == WifiScanDelivery::console) {
            scan_console_empty_pending_ = false;
        } else {
            scan_message_empty_pending_ = false;
        }
        return;
    }
    auto& cursor =
        delivery == WifiScanDelivery::console ? scan_console_cursor_ : scan_message_cursor_;
    if (cursor < scan_result_count_)
        ++cursor;
}

/** @brief Begins one candidate connection while preserving unrelated settings. */
platform::NetworkResult NetworkService::begin_provisioning_test(const std::string_view ssid,
    const std::string_view password, const std::string_view web_password, const std::uint8_t slot,
    const bool persist, const bool select, const bool restart, const bool update_web_password,
    const CredentialTestOwner owner, const std::uint32_t now) {
    if (active_test_owner_ != CredentialTestOwner::none ||
        (owner == CredentialTestOwner::onboarding &&
            provisioning_state_ == ProvisioningState::succeeded) ||
        scan_native_pending_ || (restart_ != nullptr && restart_->pending())) {
        return platform::NetworkResult::busy;
    }
    // The browser must submit from a live setup AP. Adapters without
    // concurrent STA/AP defer the station handoff until after the HTTP reply.
    if (owner == CredentialTestOwner::onboarding &&
        (!access_point_ready_ || (!onboarding() && !recovery()))) {
        provisioning_state_ = ProvisioningState::failed;
        provisioning_failure_ = ProvisioningFailure::ap_unavailable;
        return platform::NetworkResult::unavailable;
    }
    if (!enabled_) {
        if (owner == CredentialTestOwner::command) {
            wifi_test_state_ = ProvisioningState::failed;
            wifi_test_failure_ = ProvisioningFailure::connect_failed;
        } else {
            provisioning_state_ = ProvisioningState::failed;
            provisioning_failure_ = ProvisioningFailure::connect_failed;
        }
        return platform::NetworkResult::unavailable;
    }
    if (slot >= Settings::kWifiSlotCount || ssid.empty() ||
        ssid.size() > candidate_ssid_.size() - 1U ||
        password.size() > candidate_password_.size() - 1U ||
        (update_web_password && (web_password.size() > candidate_web_password_.size() - 1U ||
                                    (!web_password.empty() && web_password.size() < 8U))) ||
        ssid.find('\0') != std::string_view::npos ||
        password.find('\0') != std::string_view::npos ||
        (update_web_password && web_password.find('\0') != std::string_view::npos)) {
        if (owner == CredentialTestOwner::command) {
            wifi_test_state_ = ProvisioningState::failed;
            wifi_test_failure_ = ProvisioningFailure::connect_failed;
        } else {
            provisioning_state_ = ProvisioningState::failed;
            provisioning_failure_ = ProvisioningFailure::connect_failed;
        }
        return platform::NetworkResult::failed;
    }

    copy_candidate(candidate_ssid_, ssid);
    copy_candidate(candidate_password_, password);
    if (update_web_password) {
        copy_candidate(candidate_web_password_, web_password);
    } else {
        candidate_web_password_.fill('\0');
    }
    candidate_slot_ = slot;
    candidate_persist_ = persist;
    candidate_select_ = select;
    candidate_restart_ = restart;
    candidate_update_web_password_ = update_web_password;
    const bool defer_connect =
        owner == CredentialTestOwner::onboarding && !station_access_point_supported_;
    const auto result = defer_connect ? platform::NetworkResult::success
                                      : join(candidate_ssid_.data(), candidate_password_.data());
    if (result == platform::NetworkResult::success) {
        active_test_owner_ = owner;
        if (owner == CredentialTestOwner::command) {
            wifi_test_state_ = ProvisioningState::testing;
            wifi_test_failure_ = ProvisioningFailure::none;
        } else {
            provisioning_state_ = ProvisioningState::testing;
            provisioning_failure_ = ProvisioningFailure::none;
        }
        candidate_connect_started_ = !defer_connect;
        provisioning_deadline_ = defer_connect
                                     ? now + kOnboardingHandoffMilliseconds
                                     : now + (update_web_password ? kConnectTimeoutMilliseconds
                                                                  : kWifiTestTimeoutMilliseconds);
        restart_scheduled_ = false;
    } else {
        candidate_ssid_.fill('\0');
        candidate_password_.fill('\0');
        candidate_web_password_.fill('\0');
        candidate_persist_ = false;
        candidate_select_ = false;
        candidate_restart_ = false;
        candidate_update_web_password_ = false;
        if (result != platform::NetworkResult::busy) {
            if (owner == CredentialTestOwner::command) {
                wifi_test_state_ = ProvisioningState::failed;
                wifi_test_failure_ = ProvisioningFailure::connect_failed;
            } else {
                provisioning_state_ = ProvisioningState::failed;
                provisioning_failure_ = ProvisioningFailure::connect_failed;
            }
        }
    }
    return result;
}

/** @brief Selects the alternate configured SSID slot. */
bool NetworkService::select_next_slot() {
    for (std::uint8_t offset = 1U; offset <= Settings::kWifiSlotCount; ++offset) {
        const auto candidate =
            static_cast<std::uint8_t>((slot_ + offset) % Settings::kWifiSlotCount);
        if (settings_.ssid(candidate).empty() ||
            (attempted_slots_ & static_cast<std::uint8_t>(1U << candidate)) != 0U) {
            continue;
        }
        slot_ = candidate;
        return true;
    }
    return false;
}

/** @brief Starts the mandatory local AP used for first setup and recovery. */
void NetworkService::start_access_point(const AccessPointPurpose purpose, const std::uint32_t now) {
    access_point_purpose_ = purpose;
    ap_restarting_ = false;
    ap_retry_deadline_ = now + kApRetryMilliseconds;
    ap_ready_deadline_ = now + kConnectTimeoutMilliseconds;
    if (ethernet_hides_access_point()) {
        // The AP comes up through maintain_access_point() once the wired link drops.
        access_point_ = false;
        access_point_ready_ = false;
        return;
    }
    const auto result = platform::start_access_point(access_point_ssid(), {});
    if (result != platform::NetworkResult::success) {
        if (result != platform::NetworkResult::busy) {
            AddLog(LogLevel::error, "WIF: SoftAP start failed; retrying");
        }
        access_point_ = false;
        access_point_ready_ = false;
        return;
    }
    if (platform::set_hostname(access_point_ssid()) == platform::NetworkResult::failed) {
        AddLog(LogLevel::error, "WIF: native hostname rejected after AP start");
    }
    access_point_purpose_ = purpose;
    access_point_ = platform::wifi_snapshot().access_point == platform::AccessPointState::active;
    const auto info = platform::access_point_info();
    access_point_ready_ = access_point_ && has_address(info);
    AddLog(LogLevel::info, "WIF: SoftAP %s at %s", access_point_ssid().data(),
        has_address(info) ? info.address : "unknown address");
}

void NetworkService::maintain_access_point(const std::uint32_t now) {
    if (access_point_ready_ && !ap_restarting_)
        return;
    const auto state = platform::wifi_snapshot().access_point;
    if (state == platform::AccessPointState::active && !ap_restarting_) {
        // Some SDKs report the AP and its address only after an asynchronous start event.
        const auto info = platform::access_point_info();
        if (has_address(info)) {
            access_point_ = true;
            access_point_ready_ = true;
            AddLog(
                LogLevel::info, "WIF: SoftAP %s at %s", access_point_ssid().data(), info.address);
            return;
        }
    }
    if (state == platform::AccessPointState::active ||
        state == platform::AccessPointState::initializing) {
        if (!ap_restarting_ && !deadline_reached(now, ap_ready_deadline_))
            return;
        ap_restarting_ = true;
        if (!deadline_reached(now, ap_retry_deadline_))
            return;
        ap_retry_deadline_ = now + kApRetryMilliseconds;
        (void)platform::stop_access_point();
        access_point_ =
            platform::wifi_snapshot().access_point == platform::AccessPointState::active;
        access_point_ready_ = access_point_ && has_address(platform::access_point_info());
        return; // Confirm shutdown on the next poll before restarting.
    }
    if (!deadline_reached(now, ap_retry_deadline_))
        return;
    ap_restarting_ = false;
    start_access_point(access_point_purpose_, now);
}

bool NetworkService::ethernet_hides_access_point() const {
#if USE_ETHERNET
    // Tasmota opens no AP while Ethernet has an address. A browser test runs over the AP, so
    // it keeps the AP until it ends.
    return ethernet::connected() && active_test_owner_ != CredentialTestOwner::onboarding;
#else
    return false;
#endif
}

/** @brief Stops the AP but keeps its purpose, so it returns when Ethernet drops. */
void NetworkService::hide_access_point(const std::uint32_t now) {
    const auto state = platform::wifi_snapshot().access_point;
    ap_restarting_ = false;
    if (state != platform::AccessPointState::active &&
        state != platform::AccessPointState::initializing) {
        access_point_ = false;
        access_point_ready_ = false;
        return;
    }
    if (!deadline_reached(now, ap_retry_deadline_))
        return;
    ap_retry_deadline_ = now + kApRetryMilliseconds;
    if (platform::stop_access_point() == platform::NetworkResult::success)
        AddLog(LogLevel::info, "WIF: SoftAP stopped while Ethernet is up");
    access_point_ = platform::wifi_snapshot().access_point == platform::AccessPointState::active;
    access_point_ready_ = access_point_ && has_address(platform::access_point_info());
}

/** @brief Records a failed candidate and lets AP recovery resume. */
void NetworkService::fail_provisioning(const ProvisioningFailure failure) {
    const auto owner = active_test_owner_;
    active_test_owner_ = CredentialTestOwner::none;
    if (owner == CredentialTestOwner::command) {
        wifi_test_state_ = ProvisioningState::failed;
        wifi_test_failure_ = failure;
    } else {
        provisioning_state_ = ProvisioningState::failed;
        provisioning_failure_ = failure;
    }
    candidate_ssid_.fill('\0');
    candidate_password_.fill('\0');
    candidate_web_password_.fill('\0');
    candidate_persist_ = false;
    candidate_select_ = false;
    candidate_restart_ = false;
    candidate_update_web_password_ = false;
    AddLog(LogLevel::error, "WIF: temporary station credentials failed");
}

/** @brief Commits a DHCP-validated candidate according to its command policy. */
void NetworkService::finish_provisioning(const std::uint32_t now) {
    const auto owner = active_test_owner_;
    const bool restart_requested = candidate_restart_;
    const bool idle_restart_requested = candidate_persist_ && !candidate_restart_;
    if (candidate_persist_) {
        const auto web_password = candidate_update_web_password_
                                      ? std::string_view{candidate_web_password_.data()}
                                      : settings_.web_password();
        const auto result = settings_.set_wifi_configuration(candidate_slot_,
            candidate_ssid_.data(), candidate_password_.data(), web_password, candidate_select_);
        if (result != SettingsUpdateResult::saved && result != SettingsUpdateResult::unchanged) {
            fail_provisioning(ProvisioningFailure::connect_failed);
            return;
        }
    }
    active_test_owner_ = CredentialTestOwner::none;
    if (owner == CredentialTestOwner::command) {
        wifi_test_state_ = ProvisioningState::succeeded;
        wifi_test_failure_ = ProvisioningFailure::none;
    } else {
        provisioning_state_ = ProvisioningState::succeeded;
        provisioning_failure_ = ProvisioningFailure::none;
    }
    connected_ = true;
    candidate_connect_started_ = false;
    slot_ = candidate_slot_;
    if (!station_access_point_supported_ && access_point_purpose_ != AccessPointPurpose::none) {
        const auto ap_state = platform::wifi_snapshot().access_point;
        if (ap_state == platform::AccessPointState::active ||
            ap_state == platform::AccessPointState::initializing) {
            (void)platform::stop_access_point();
        }
        access_point_purpose_ = AccessPointPurpose::none;
        ap_restarting_ = false;
        const auto snapshot = platform::wifi_snapshot();
        access_point_ = snapshot.access_point == platform::AccessPointState::active;
        access_point_ready_ = access_point_ && has_address(platform::access_point_info());
    }
    if (candidate_restart_ && restart_ != nullptr) {
        const auto restart_result = restart_->schedule(RestartTarget::current, now);
        restart_scheduled_ = restart_result == RestartRequestResult::scheduled;
        if (!restart_scheduled_) {
            AddLog(LogLevel::error, "WIF: restart after provisioning failed");
        }
    }
    if (idle_restart_requested) {
        wifi_test_restart_pending_ = true;
        wifi_test_restart_deadline_ = now + kWifiTestIdleRestartMilliseconds;
    } else if (restart_requested) {
        wifi_test_restart_pending_ = false;
    }
    candidate_ssid_.fill('\0');
    candidate_password_.fill('\0');
    candidate_web_password_.fill('\0');
    candidate_persist_ = false;
    candidate_select_ = false;
    candidate_restart_ = false;
    candidate_update_web_password_ = false;
    AddLog(LogLevel::info, restart_requested ? "WIF: station credentials accepted; restarting"
                                             : "WIF: station credentials accepted");
}

} // namespace newmota
