/*
  power_service.h - relay power and button behavior for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  CommandSource/SetDevicePower reference: Tasmota tasmota.h and support_tasmota.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file power_service.h
 * @brief Routes relay, core Light and configured external device power endpoints.
 */

#pragma once
#include "newmota_features.h"

#include "drivers/io/basic_io.h"
#if USE_STATUS_LEDS
#include "services/power/led_service.h"
#endif
#include "services/power/power_settings.h"
#ifndef NEWMOTA_IMAGE_SAFEBOOT
#include "services/power/button_event.h"
#endif

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota {
#if USE_PWM || USE_WS2812
class LightService;
#endif

/** @brief Pinned Tasmota source identities for implemented portable Power callers.
 * These values describe operation origin, never a driver or endpoint identity.
 */
enum class PowerOperationSource : std::uint8_t {
    unspecified = 0,
    mqtt = 1,
    restart = 2,
    button = 3,
    switch_input = 4,
    serial = 6,
    web_gui = 7,
    web_command = 8,
    web_console = 9,
    pulse_timer = 10,
    timer = 11,
    max_power = 13,
    max_energy = 14,
    overtemperature = 15,
    light = 16,
#if USE_BACKLOG
    backlog = 5,
#endif
    berry = 27
};

enum class PowerAction : std::uint8_t {
    off,
    on,
    toggle,
    blink,
    blink_stop,
};

enum class PowerChangeResult : std::uint8_t {
    changed,
    unchanged,
    unavailable,
    failed,
};

#ifndef NEWMOTA_IMAGE_SAFEBOOT
struct ButtonPolicy final {
    bool immediate{};
    bool swap_single_double{};
    bool decoupled{};
    std::uint8_t hold_tenths{40U};
};
#endif
#if USE_SWITCHES
/** @brief Settings read by the switch handler on each pass. */
struct SwitchPolicy final {
    const std::uint8_t* modes{};   /**< SwitchMode1..32. */
    bool detached{};               /**< SetOption114: publish Actions instead of switching Power. */
    std::uint8_t hold_tenths{40U}; /**< SetOption32, Tasmota's P_HOLD_TIME. */
};
#endif

class PowerService final {
  public:
    /** @brief Initializes safe pins and then applies the selected boot policy. */
    [[nodiscard]] BasicIoInitializationResult initialize(const Hardware& hardware,
        std::uint32_t now, const PowerSettings& settings = kDefaultPowerSettings,
        bool save_state = false, bool power_on_reset = true);
    /** @brief Applies one logical action to a configured one-based relay.
     * @param instance One-based configured endpoint.
     * @param action Requested logical operation.
     * @param source Explicit caller origin, recorded only with USE_POWER_PROVENANCE.
     * @return Endpoint operation result; provenance is not physical switching feedback.
     */
    [[nodiscard]] PowerChangeResult set(std::uint8_t instance, PowerAction action,
        PowerOperationSource source = PowerOperationSource::unspecified);
    /** @brief Applies one logical action to every configured relay.
     * @param action Requested logical operation.
     * @param source Explicit caller origin shared by this operation's endpoints.
     * @return Aggregated endpoint result; unavailable when no endpoints exist.
     */
    [[nodiscard]] PowerChangeResult set_all(
        PowerAction action, PowerOperationSource source = PowerOperationSource::unspecified);
#if USE_POWER_FORCE_OFF
    /** @brief Applies source FORCE_OFF to every output, bypassing ordinary native relay policy.
     * @param source Explicit caller cause; captured/logged only with USE_POWER_PROVENANCE.
     * @return failed if any endpoint fails; all other outputs are still attempted.
     * Source PowerOnState4 reasserts ON even for FORCE_OFF; native pulse/blink timers remain.
     * This is not a latch. External outputs report accepted intent, not isolation feedback.
     */
    [[nodiscard]] PowerChangeResult force_off(
        PowerOperationSource source = PowerOperationSource::unspecified);
#endif
#if USE_POWER_TARGETS
    /** @brief Groups synchronous owner updates so intermediate relay masks are never remembered.
     * Owners use one scope per accepted logical operation; nested scopes commit only at outer exit.
     */
    class TargetUpdate final {
      public:
        /** @brief Opens one synchronous owner-operation boundary.
         * @param owner Non-owning Power owner, or null for an uninitialized external adapter.
         * @param enabled False for operations that do not apply logical power.
         */
        explicit TargetUpdate(PowerService* owner, bool enabled = true);
        ~TargetUpdate();
        TargetUpdate(const TargetUpdate&) = delete;
        TargetUpdate& operator=(const TargetUpdate&) = delete;

      private:
        PowerService* owner_{};
    };

    /** @brief Records an external owner's accepted state when no grouped update is active. */
    void observe_targets();
    /** @brief Reads the source-style shared last nonzero intent, restricted to explicit targets.
     * @param selection Fully configured nonempty endpoint mask.
     * @param on Receives remembered ON bits within selection; unchanged when invalid.
     * @return true for valid targets. Zero never erases the remembered nonzero mask.
     */
    [[nodiscard]] bool last_nonzero_targets(std::uint32_t selection, std::uint32_t& on) const;
    /** @brief Restores the shared last-nonzero mask within explicitly owned targets.
     * @param selection Configured endpoint scope; no trip-local capture is made.
     * @param source Operation origin.
     * @return Scoped restore result; native pulse/blink schedules remain untouched.
     */
    [[nodiscard]] PowerChangeResult restore_last_nonzero(
        std::uint32_t selection, PowerOperationSource source = PowerOperationSource::unspecified);
#endif
#if USE_POWER_TARGETS
    /** @brief Reads an explicit nonempty mask of configured logical Power endpoints.
     * @param selection One bit per one-based Power index; every bit must exist.
     * @param on Receives accepted logical ON intent limited to selection, not fading PWM feedback.
     * Unchanged on invalid selection.
     * @return true for a fully configured selection. This neither reserves endpoints nor reads
     * GPIO.
     */
    [[nodiscard]] bool target_state(std::uint32_t selection, std::uint32_t& on) const;
    /** @brief Applies selected FORCE_OFF, cancelling selected blink and rearming native PulseTime.
     * @param selection Explicit configured endpoint mask; no meter/phase mapping is inferred.
     * @param source Operation origin.
     * @return Aggregate result; invalid masks are rejected before any mutation.
     * Source always-on/interlock policies still apply globally; this is not an isolation guarantee.
     */
    [[nodiscard]] PowerChangeResult force_off_selected(
        std::uint32_t selection, PowerOperationSource source = PowerOperationSource::unspecified);
    /** @brief Applies explicit group FORCE_OFF without selected pulse/blink manipulation.
     * @param selection Fully configured group mask, never inferred from meter/channel indexes.
     * @param source Operation origin.
     * @return Aggregate direct-application result; existing schedules and global policies remain.
     */
    [[nodiscard]] PowerChangeResult force_off_group(
        std::uint32_t selection, PowerOperationSource source = PowerOperationSource::unspecified);
    /** @brief Reapplies explicit caller-owned state within a configured endpoint selection.
     * @param selection Scope of caller intent; no implicit trip snapshot or last-nonzero capture.
     * @param on Requested ON subset of selection; outside bits are invalid.
     * @param source Operation origin.
     * @return Aggregate result. Equal selected intent is a no-op, preserving source RestorePower
     * behavior. Bypasses ordinary native pulse/blink manipulation, retaining current unselected
     * logical intent.
     */
    [[nodiscard]] PowerChangeResult restore_selected(std::uint32_t selection, std::uint32_t on,
        PowerOperationSource source = PowerOperationSource::unspecified);
    /** @brief Applies ordinary selected operations, including per-channel ON retry policy.
     * @param selection Explicit configured endpoint mask, validated before any operation.
     * @param action Ordinary logical action; native timers/interlocks use their existing owner.
     * @param source Operation origin.
     * @return Aggregate result, attempting every selected endpoint after a write failure.
     */
    [[nodiscard]] PowerChangeResult set_selected(std::uint32_t selection, PowerAction action,
        PowerOperationSource source = PowerOperationSource::unspecified);
#endif
#if USE_POWER_PROVENANCE
    /** @brief Returns the latest explicit configured Power-operation origin, including unchanged
     * intent. Queries/unavailable endpoints do not alter it; unspecified callers remain source
     * zero. This is volatile diagnostic state, separate from the last forced-operation cause.
     */
    [[nodiscard]] PowerOperationSource last_source() const { return last_source_; }

    /** @brief Returns the last explicit forced-operation cause, including a failed attempt.
     * This remains independent of subsequent ordinary transport/button/timer operations.
     */
    [[nodiscard]] PowerOperationSource last_force_source() const { return last_force_source_; }
#endif
    /** @brief Reads one configured relay's logical state. */
    [[nodiscard]] bool state(std::uint8_t instance, bool& on) const;
    /**
     * @brief Serializes the bounded Tasmota-compatible STATE payload.
     *
     * The result contains only fields owned by the power service.  It is
     * suitable for publication through the canonical message router and is
     * unchanged by WebUI or MQTT adapters.
     *
     * @return false when the caller-provided buffer cannot hold the complete
     *         JSON value; @p size is zero on failure.
     */
    [[nodiscard]] bool serialize_state(
        std::uint32_t uptime_seconds, char* output, std::size_t capacity, std::size_t& size) const;
#if USE_ENERGY_POWER_DELTA || USE_POWER_TARGETS
    /** @brief Returns the revision of configured Power operations, including unchanged intent.
     * Used by reporting consumers to suppress transient measurements after an operation.
     * This is not a physical switching timestamp or a meter-to-relay association.
     */
    [[nodiscard]] std::uint32_t operation_revision() const {
        return operation_revision_ + state_revision();
    }
#endif
    /** @brief Returns the compact logical state of all relay instances. */
#if USE_PWM || USE_WS2812
    [[nodiscard]] std::uint32_t state_mask() const;
#else
    [[nodiscard]] constexpr std::uint32_t state_mask() const { return state_mask_; }
#endif
    /**
     * @brief Returns the semantic revision of the logical relay state.
     *
     * Snapshot refreshes such as uptime updates do not advance this value;
     * consumers can therefore distinguish state changes from fresh readings.
     */
#if USE_PWM || USE_WS2812
    [[nodiscard]] std::uint32_t state_revision() const;
#else
    [[nodiscard]] constexpr std::uint32_t state_revision() const { return state_revision_; }
#endif
    [[nodiscard]] std::uint8_t endpoint_count() const {
        std::uint8_t count = 0U;
        for (std::uint8_t i = 1U; i <= 32U; ++i) {
            bool on = false;
            if (state(i, on))
                ++count;
        }
        return count;
    }
#if USE_PWM || USE_WS2812
    void attach_light(LightService& light) { light_ = &light; }

    [[nodiscard]] LightService* light() const { return light_; }
#endif
    /** @brief Reports whether a one-based relay instance is configured. */
    [[nodiscard]] bool has_relay(std::uint8_t instance) const;
    /** @brief Enables or releases Tasmota's immutable always-on relay mode. */
    [[nodiscard]] PowerChangeResult set_always_on(bool enabled);
    /** @brief Restarts one configured pulse timer after a setting change. */
    void update_pulse_time(std::uint8_t index, std::uint32_t now);
    /** @brief Restarts an active blink cadence after BlinkTime changes. */
    void update_blink_time(std::uint32_t now);
    /** @brief Applies a changed BlinkCount to an active blink sequence. */
    void update_blink_count();
    /** @brief Enforces the currently referenced interlock configuration. */
    [[nodiscard]] PowerChangeResult apply_interlock();
    /** @brief Returns one Tasmota-encoded remaining pulse duration. */
    [[nodiscard]] std::uint16_t remaining_pulse_time(std::uint8_t index, std::uint32_t now) const;
    /** @brief Reports whether a one-based relay is currently blinking. */
    [[nodiscard]] bool blinking(std::uint8_t instance) const;

    /** @brief Returns the number of configured relay instances. */
#if USE_TUYA_MCU
    [[nodiscard]] std::uint8_t relay_count() const { return io_.relay_count(); }

    /** @brief Adds relays without a pin before initialize(); see BasicIoDriver. */
    void attach_external_relays(
        std::uint32_t mask, BasicIoDriver::ExternalRelayWriter writer, void* context) {
        io_.attach_external_relays(mask, writer, context);
    }

    /** @brief Returns the relays without a pin, one bit per one-based instance. */
    [[nodiscard]] constexpr std::uint32_t external_relays() const { return io_.external_relays(); }
#if USE_SWITCHES
    /** @brief Adds switches without a pin before initialize(); see BasicIoDriver. */
    void attach_external_switches(std::uint32_t mask) { io_.attach_external_switches(mask); }

    /** @brief Sets a switch without a pin; the switch handler reacts on its next pass. */
    void set_external_switch(std::uint8_t instance, bool high) {
        io_.set_external_switch(instance, high);
    }
#endif
#else
    [[nodiscard]] constexpr std::uint8_t relay_count() const { return io_.relay_count(); }
#endif

#if USE_STATUS_LEDS
    /** @brief Returns the status and link LED policy driver. */
    [[nodiscard]] LedService& leds() { return leds_; }
#endif

#ifndef NEWMOTA_IMAGE_SAFEBOOT
    /** @brief Binds a synchronous observer/router; true consumes the remote single-toggle route.
     * The callback must not block or retain the event reference. SO73 suppresses local Power
     * regardless of callback success; no callback means ordinary local fallback.
     */
    void set_button_event_handler(bool (*handler)(void*, const ButtonEvent&), void* context) {
        button_event_handler_ = handler;
        button_event_context_ = context;
    }

#if USE_BERRY_DRIVERS
    /** @brief Observes each debounced press and release after the startup guard.
     * The handler receives the instance, the new level and the multi-press count before this
     * edge. It runs synchronously, must not block and cannot cancel the native action.
     */
    void set_button_level_handler(
        void (*handler)(void*, std::uint8_t, bool, std::uint8_t), void* context) {
        button_level_handler_ = handler;
        button_level_context_ = context;
    }

    /** @brief Observes each relay or Light Power operation, changed or not, as Tasmota's
     * SetDevicePower: every command, interlock step and group write. Runs synchronously after
     * the logical state is recorded and must not block.
     */
    void set_operation_handler(void (*handler)(void*), void* context) {
        operation_handler_ = handler;
        operation_context_ = context;
    }
#endif

    /** @brief Returns the number of configured button instances. */
    [[nodiscard]] constexpr std::uint8_t button_count() const { return io_.button_count(); }

    /** @brief Probes GPIO every 10 ms and applies policy at the ButtonDebounce cadence.
     * @param debounce Validated ButtonDebounce milliseconds, excluding AC mode.
     * Call from each main pass and idle-wait slice; missed samples are never replayed.
     */
    void poll_buttons(std::uint32_t now, ButtonPolicy policy, std::uint16_t debounce);
#endif
#if USE_SWITCHES
    /** @brief Binds the synchronous SendKey route; true consumes the key, as Tasmota's SendKey.
     * The callback must not block or retain the event reference.
     */
    void set_switch_event_handler(bool (*handler)(void*, const SwitchEvent&), void* context) {
        switch_event_handler_ = handler;
        switch_event_context_ = context;
    }

    /** @brief Returns the number of configured switch instances. */
    [[nodiscard]] std::uint8_t switch_count() const { return io_.switch_count(); }

    /** @brief Returns the configured switches, including switches without a pin not yet seen. */
    [[nodiscard]] std::uint32_t configured_switches() const { return io_.configured_switches(); }

    /** @brief Reports a switch's state as Tasmota's SwitchState: the level, flipped by _INV modes.
     * @return false for an unconfigured switch.
     */
    [[nodiscard]] bool switch_state(
        std::uint8_t instance, const SwitchPolicy& policy, bool& on) const;

    /** @brief Probes switches every 10 ms and runs Tasmota's SwitchHandler every SwitchDebounce.
     * @param debounce Validated SwitchDebounce milliseconds, excluding AC mode.
     */
    void poll_switches(std::uint32_t now, const SwitchPolicy& policy, std::uint16_t debounce);
#endif
    /** @brief Advances pulse and blink deadlines at the 100 ms cadence. */
    void poll_timers(std::uint32_t now);

  private:
    /** @brief Logs accepted button, timer and pulse results without repeating command replies. */
    void log_runtime_result(
        std::uint8_t instance, PowerChangeResult result, PowerOperationSource source) const;
#if USE_PWM || USE_WS2812
    friend class LightService; /**< Light reports explicit logical power origins without recursive
                                  routing. */
#endif
#if USE_POWER_FORCE_OFF
    /** @brief Applies a direct scoped mask through the shared forced-output policy.
     * Selected and global FORCE_OFF deliberately differ only in native timer cancellation.
     * Always-on/interlock policies can alter endpoints outside the requested selection.
     */
    [[nodiscard]] PowerChangeResult write_direct(std::uint32_t selection, std::uint32_t on,
        bool cancel_selected, bool forced, PowerOperationSource source);
#endif
#if USE_POWER_TARGETS
    std::uint32_t last_nonzero_targets_{}, target_update_depth_{};
    /** @brief Combines native state and external accepted intent, not fading PWM feedback. */
    [[nodiscard]] std::uint32_t target_intent_mask() const;
    /** @brief Keeps source last_power only for nonzero complete masks. */
    void remember_targets(std::uint32_t on);
#endif
    /** @brief Captures a source before an accepted endpoint operation; no storage when excluded. */
    void record_source(PowerOperationSource source);
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    static constexpr std::uint32_t kStartupGuardMilliseconds = 4000U;
    static constexpr std::uint32_t kMultiPressWindowMilliseconds = 500U;

    /** @brief Receives one driver button edge through the allocation-free callback. */
    static void button_edge(void* context, const ButtonEdge& edge);
    /** @brief Records one button edge against the current polling policy. */
    void handle_button_edge(const ButtonEdge& edge);
    /** @brief Executes one completed button press sequence. */
    void execute_button_sequence(std::uint8_t instance, std::uint8_t press_count);
    /** @brief Delivers one typed gesture without allocating or retaining a payload buffer. */
    [[nodiscard]] bool emit_button_event(const ButtonEvent& event);
#endif
#if USE_SWITCHES
    /** @brief Runs one SwitchHandler pass for one switch. */
    void handle_switch(
        std::size_t index, bool high, const SwitchPolicy& policy, std::uint32_t loops_per_second);
    /** @brief Delivers a SendKey or Action event; true when a route consumed the key. */
    bool emit_switch_event(const SwitchEvent& event);
#endif
    static constexpr std::uint32_t kInterlockDelayMilliseconds = 50U;
    /** @brief Applies an ordinary logical state and its timer policies. */
    [[nodiscard]] PowerChangeResult set_logical(std::uint8_t instance, PowerAction action);
    /** @brief Writes one logical state while bypassing high-level policies. */
    [[nodiscard]] PowerChangeResult write_logical(std::uint8_t instance, bool on);
    /** @brief Stops one blink and restores its retained logical state. */
    [[nodiscard]] PowerChangeResult stop_blink(std::uint8_t instance);
    /** @brief Starts or refreshes one relay blink sequence. */
    [[nodiscard]] PowerChangeResult start_blink(std::uint8_t instance);
    /** @brief Arms a pulse deadline from the current logical relay state. */
    void arm_pulse(std::uint8_t index, std::uint32_t now);
    /** @brief Returns the effective mask for one configured interlock group. */
    [[nodiscard]] std::uint32_t interlock_group(std::size_t index) const;
    /** @brief Returns the mask of all configured relays. */
    [[nodiscard]] std::uint32_t relay_mask() const;
    /** @brief Encodes milliseconds using Tasmota's PulseTime units. */
    [[nodiscard]] static std::uint16_t encode_pulse_remaining(std::uint32_t milliseconds);
    /** @brief Converts one Tasmota PulseTime value into milliseconds. */
    [[nodiscard]] static std::uint32_t pulse_milliseconds(std::uint16_t value);
    /** @brief Reports whether one rollover-safe millisecond deadline is due. */
    [[nodiscard]] static bool deadline_reached(std::uint32_t now, std::uint32_t deadline);

#if USE_PWM || USE_WS2812
    LightService* light_{};
#endif
    BasicIoDriver io_{};
#if USE_STATUS_LEDS
    LedService leds_{io_};
#endif
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    std::array<std::uint32_t, BasicIoDriver::kMaximumButtons> button_deadlines_{};
    std::array<std::uint8_t, BasicIoDriver::kMaximumButtons> button_press_counts_{};
    std::array<std::uint32_t, BasicIoDriver::kMaximumButtons> button_press_started_{};
    std::uint32_t button_hold_armed_{};
    std::uint32_t button_hold_sent_{};
    bool (*button_event_handler_)(void*, const ButtonEvent&){};
    void* button_event_context_{};
#if USE_BERRY_DRIVERS
    void (*button_level_handler_)(void*, std::uint8_t, bool, std::uint8_t){};
    void* button_level_context_{};
    void (*operation_handler_)(void*){};
    void* operation_context_{};
#endif
    std::uint32_t button_enable_at_{};
    std::uint32_t button_poll_at_{};
    std::uint16_t button_debounce_{};
    ButtonPolicy button_policy_{};
#endif
#if USE_SWITCHES
    std::array<std::uint16_t, BasicIoDriver::kMaximumSwitches> switch_hold_timers_{};
    std::array<std::uint8_t, BasicIoDriver::kMaximumSwitches> switch_press_flags_{};
    bool (*switch_event_handler_)(void*, const SwitchEvent&){};
    void* switch_event_context_{};
    std::uint32_t switch_last_levels_{}; /**< Tasmota last_state, one bit per switch, set HIGH. */
    std::uint32_t switch_poll_at_{};
    std::uint16_t switch_debounce_{};
#endif
    std::array<std::uint32_t, kMaximumPowerOutputs> pulse_deadlines_{};
    std::uint32_t state_mask_{};
    std::uint32_t state_revision_{};
#if USE_ENERGY_POWER_DELTA || USE_POWER_TARGETS
    std::uint32_t operation_revision_{};
#endif
    std::uint32_t pulse_active_mask_{};
    std::uint32_t blink_mask_{};
    std::uint32_t blink_deadline_{};
    std::uint32_t now_{};
    std::uint16_t blink_transitions_{};
    const PowerSettings* settings_{&kDefaultPowerSettings};
    bool blink_output_on_{};
    bool save_state_{};
#if USE_POWER_PROVENANCE
    PowerOperationSource last_source_{PowerOperationSource::unspecified};
    PowerOperationSource last_force_source_{PowerOperationSource::unspecified};
#endif
    bool always_on_{};
};

} // namespace newmota
