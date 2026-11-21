/*
  ble.h - Platform BLE contract

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble.h
 * @brief Platform BLE contract: scanning, connections and local configuration.
 *
 * Scanning and connections are independent. A scan hands advertisements to the
 * cooperative loop through one slot; connections run one job at a time (client
 * or server operation) and hand back one completion, or a queued event. Nothing
 * here calls back into application code; the loop polls.
 */

#pragma once
#include <cstddef>
#include <cstdint>

namespace newmota::platform {
/** @brief Script-visible result codes; the numeric values are a stable contract. */
enum class BleError : std::uint8_t {
    none,
    no_connect,
    disconnected,
    no_service,
    no_characteristic,
    cannot_read,
    cannot_notify,
    cannot_write,
    write_failed,
    notify_timeout,
    buffer_too_small,
};

/** @brief Starts the BLE host stack. @return Whether it is running or starting. */
[[nodiscard]] bool ble_start();
/** @brief Stops the stack and releases the controller; a later ble_start() runs it again.
 *  Blocks while open links are closed. @return False when it could not stop.
 */
[[nodiscard]] bool ble_stop();
/** @brief Reports whether the stack has been started. */
[[nodiscard]] bool ble_ready();

// ---- Scanning

/** @brief One advertisement, with a scan response appended when one followed. */
struct BleAdvertisement {
    std::uint8_t address[6]; ///< Display order, most significant byte first.
    std::uint8_t address_type;
    std::int8_t rssi;
    std::uint8_t length;
    std::uint8_t payload[62];
};

/** @brief Parties that can demand a running scan; it runs while any is set. */
enum class BleScanConsumer : std::uint8_t { berry = 1, native = 2 };

/** @brief Scan mode and timing; interval and window count 0.625 ms units. */
struct BleScanParameters {
    bool active; ///< Request scan responses.
    std::uint16_t interval, window;
};

/** @brief Passive, 100 ms interval, 80 ms window. */
inline constexpr BleScanParameters kBleScanDefaults{false, 160, 128};

/** @brief Checks the range every host accepts: 2.5 to 640 ms, window no longer than interval. */
[[nodiscard]] constexpr bool ble_scan_valid(const BleScanParameters& parameters) {
    return parameters.window >= 4 && parameters.window <= parameters.interval &&
           parameters.interval <= 1024;
}

/** @brief Sets or clears one consumer's demand for scanning. */
void ble_scan_set(BleScanConsumer consumer, bool on);
/** @brief Applies scan parameters until reboot, restarting a running scan.
 *  @return False when they are out of range.
 */
[[nodiscard]] bool ble_scan_configure(const BleScanParameters& parameters);
/** @brief Reports the scan parameters in use. */
[[nodiscard]] BleScanParameters ble_scan_parameters();
/** @brief Restricts reports to watched devices once the first one is added.
 *  @param address Display order. @return False when the list is full.
 */
[[nodiscard]] bool ble_scan_watch(const std::uint8_t* address, std::uint8_t address_type);
/** @brief Empties the watch list, so every device is reported again. */
void ble_scan_unwatch();
/** @brief Reads one watched device. @param address Display order. @return False past the end. */
[[nodiscard]] bool ble_scan_watched(
    std::size_t index, std::uint8_t* address, std::uint8_t& address_type);
/** @brief Reports whether ble_scan_take() would return an advertisement. */
[[nodiscard]] bool ble_scan_pending();
/** @brief Takes the waiting advertisement. @return False when there is none. */
[[nodiscard]] bool ble_scan_take(BleAdvertisement& advertisement);
/** @brief Advertisements discarded because the slot was still occupied. */
[[nodiscard]] std::uint32_t ble_scan_dropped();

// ---- Connections

/** @brief One staged job. Pointers must stay valid until the completion is polled. */
struct BleRequest {
    std::uint8_t address[6];   ///< Display order, most significant byte first.
    std::uint8_t address_type; ///< 0 public, 1 random, 2 and 3 identity variants.
    std::uint8_t operation;    ///< 1 read, 2 write, 3 subscribe, 4 unsubscribe, 5 disconnect,
                               ///< 6 list services, 7 list characteristics, 201 advertisement,
                               ///< 202 scan response, 211 server characteristic.
    bool one_shot;             ///< Disconnect after a client operation.
    bool confirm;              ///< Confirmed write, indication, or refresh, per operation.
    bool has_arg1;             ///< A characteristic handle (client) or option (server) follows.
    std::int32_t arg1;
    std::uint32_t pin;        ///< Passkey for pairing during client operations.
    std::uint8_t service[16]; ///< 128-bit UUID, little-endian.
    std::uint8_t characteristic[16];
    const std::uint8_t* data; ///< Payload: write value, advertisement or characteristic value.
    std::uint16_t length;
    std::uint8_t* reply; ///< Receives the result payload; may alias @c data.
    std::uint16_t reply_capacity;
};

/** @brief A finished job, an unsolicited event or a notification. */
struct BleEvent {
    std::uint16_t error;    ///< A BleError, or the host's disconnect reason for operation 5.
    std::uint8_t operation; ///< The job's operation, 103 for a notification, 221 to 230 server.
    std::uint16_t uuid;     ///< Low 16 bits of the characteristic's short UUID.
    std::uint16_t handle;
    std::uint16_t length; ///< Payload bytes now in the reply buffer.
};

/** @brief Creates the connection task and event queue; idempotent. */
[[nodiscard]] bool ble_connection_open();
/** @brief Disconnects and releases the connection task; completes asynchronously. */
void ble_connection_close();
/** @brief Reports whether a job is running or its completion is not yet collected. */
[[nodiscard]] bool ble_connection_busy();
/** @brief Starts a job. @return False when busy or the connection task is not open. */
[[nodiscard]] bool ble_connection_request(const BleRequest& request);
/** @brief Reports whether ble_connection_poll() would return an event. */
[[nodiscard]] bool ble_connection_pending();
/** @brief Takes the next event. Queued payloads are copied to @p payload.
 *  @return False when nothing is ready.
 */
[[nodiscard]] bool ble_connection_poll(
    BleEvent& event, std::uint8_t* payload, std::size_t capacity);
/** @brief Events dropped because the queue was full. */
[[nodiscard]] std::uint32_t ble_connection_dropped();

// ---- Local configuration, synchronous

/** @brief Applies operation 231 (own address), 232 (advertising parameters in
 *  data[0..4]: mode, minimum and maximum interval) or 233 (device name in data).
 *  @return Whether it was applied.
 */
[[nodiscard]] bool ble_configure(const BleRequest& request);
/** @brief Restores a backend-specific bond export from the authenticated event.
 *  ESP uses two consecutive NimBLE security records; other hosts use their own
 *  format. Records are not interchangeable between SDKs or platform identities.
 */
[[nodiscard]] bool ble_store_bond(const std::uint8_t* record, std::size_t size);

// ---- Introspection

struct BleConnectionInfo {
    bool present;
    bool bonded, master, encrypted, authenticated, has_rssi, has_irk;
    std::uint8_t peer_address[6];    ///< Display order.
    std::uint8_t peer_id_address[6]; ///< Display order.
    std::uint8_t irk[16];
    std::int8_t rssi;
    std::uint16_t mtu;
};

struct BleInfo {
    std::uint8_t roles;   ///< Bit 0 scanner, 1 client, 2 server, 3 advertiser.
    std::uint8_t version; ///< 4 or 5.
    std::uint8_t bonds;
    std::int8_t power; ///< dBm.
    std::uint16_t mtu;
    std::uint8_t local_address[6]; ///< Display order.
    bool ready, pending;
    std::uint8_t last_operation;
    std::uint16_t last_error;
    std::uint32_t dropped_events, dropped_advertisements;
    std::uint16_t scan_stack_free, connection_stack_free; ///< Bytes; 0 when the task is absent.
    BleConnectionInfo connection;
};

void ble_info(BleInfo& info);
} // namespace newmota::platform
