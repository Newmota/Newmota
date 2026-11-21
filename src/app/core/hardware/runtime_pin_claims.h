/*
  runtime_pin_claims.h - Runtime GPIO ownership tokens and claim operations

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file runtime_pin_claims.h
 * @brief Declares retained ownership for runtime GPIO pad claims.
 */

#pragma once
#include "core/hardware/hardware.h"
#include "newmota_features.h"

namespace newmota::runtime_pins {
/** @brief Service category used to scope a runtime pin owner identity. */
enum class OwnerKind : std::uint8_t {
    none,
    gpio,
    serial,
    led_strip,
    display,
    audio,
    edge_capture,
    energy,
    /** @brief General device SPI bus-wire and chip-select generations. */
    device_spi,
    /** @brief Configured ADC input generations. */
    adc
};

/** @brief Identifies a service and its retained instance without numeric namespaces. */
struct Owner {
    OwnerKind kind{OwnerKind::none};
    std::uint32_t instance{};

    /** @brief Reports whether this token contains a well-formed owner identity. */
    constexpr explicit operator bool() const {
        switch (kind) {
        case OwnerKind::gpio:
        case OwnerKind::serial:
        case OwnerKind::led_strip:
        case OwnerKind::display:
        case OwnerKind::audio:
        case OwnerKind::edge_capture:
        case OwnerKind::energy:
        case OwnerKind::device_spi:
        case OwnerKind::adc:
            return instance != 0;
        case OwnerKind::none:
            return false;
        }
        return false;
    }

    /** @brief Compares both the service category and instance token.
     * @param other Owner identity to compare.
     * @return true when both identity components match.
     */
    constexpr bool operator==(const Owner& other) const {
        return kind == other.kind && instance == other.instance;
    }

    /** @brief Reports whether this identity differs from another owner.
     * @param other Owner identity to compare.
     * @return true when either identity component differs.
     */
    constexpr bool operator!=(const Owner& other) const { return !(*this == other); }
};

/** @brief Allocates a never-reused application-task energy GPIO identity.
 * @return An empty owner after uint32 generation exhaustion; never derive these tokens from pins
 * or another service's generation. Quarantined claims remain uniquely identifiable.
 */
Owner allocate_energy_owner();
/** @brief Authorizes only the selected energy provider's resolved pads for native owners.
 * Existing claims remain exclusive; this does not release any foreign owner.
 */
bool authorize_energy(const PinId* pins, std::size_t count);

/** @brief Limits delegated energy-pad acquisition to provider lifecycle calls.
 * A failed provider's configured pads remain reserved against unrelated serial/ADC callers.
 */
class EnergyClaimScope {
  public:
    EnergyClaimScope();
    ~EnergyClaimScope();
    EnergyClaimScope(const EnergyClaimScope&) = delete;
    EnergyClaimScope& operator=(const EnergyClaimScope&) = delete;

  private:
    bool previous_{};
};

/** @brief Stable owner token for the primary GPIO service. */
inline constexpr Owner kGpio{OwnerKind::gpio, 1};
/** @brief Stable owner token for the shared display service. */
inline constexpr Owner kDisplay{OwnerKind::display, 1};

/** @brief Sets the resolved hardware reservation view without clearing retained claims.
 * @param hardware Current resolved bindings, or nullptr when no hardware view is available.
 */
void initialize(const Hardware* hardware);
/** @brief Reserves all configured bindings, including failed or inactive assignments.
 * @param pin Physical pad to check.
 * @return true when the pad belongs to a configured binding or no hardware view is available.
 */
bool configured(PinId pin);

/** @brief Finds the retained runtime owner of a physical pad.
 * @param pin Physical pad to inspect.
 * @return Its owner, or an empty token when the pad has no runtime owner.
 */
Owner owner(PinId pin);

/** @brief Preflights every requested pad before atomically acquiring the complete set.
 * @param owner Owner token that will retain the claims.
 * @param pins Distinct, available, unconfigured pads to claim.
 * @param count Number of entries in pins; must be nonzero.
 * @return true when every pad is acquired or already belongs to this owner.
 */
bool claim(Owner owner, const PinId* pins, std::size_t count);
/** @brief Claims descriptor-selected display pads while allowing authorized bindings.
 * @param owner Display owner token.
 * @param pins Complete distinct pad set required by the descriptor.
 * @param count Number of entries in pins.
 * @param authorized Configured pads whose bindings the descriptor resolved.
 * @param authorized_count Number of entries in authorized.
 * @return true when the complete set is acquired.
 */
bool claim_display(Owner owner, const PinId* pins, std::size_t count, const PinId* authorized,
    std::size_t authorized_count);
/** @brief Claims an unassigned pad or its unique active GPIO_WS2812 binding.
 * @param owner LED strip owner token.
 * @param pin Physical data pad.
 * @return true when the pad is acquired by this owner.
 */
bool claim_led_strip(Owner owner, PinId pin);
/** @brief Atomically claims free pads or their matching active I2S bindings.
 * @param owner Audio owner token.
 * @param pins Physical pads required by the selected I2S route.
 * @param roles I2S role corresponding to each pad.
 * @param count Number of entries in both arrays.
 * @return true when all pads are acquired.
 */
bool claim_i2s(Owner owner, const PinId* pins, const PinRole* roles, std::size_t count);
/** @brief Atomically claims the unique active GPS RX/TX bindings for a serial generation.
 * Both configured roles must match; ordinary unassigned-pad claims stay unchanged.
 */
bool claim_gps_serial(Owner owner, PinId rx, PinId tx, std::uint32_t baud);
#if USE_TUYA_MCU
/** @brief Atomically claims the unique active Tuya Rx/Tx bindings for a serial generation. */
bool claim_tuya_serial(Owner owner, PinId rx, PinId tx);
#endif
/** @brief Releases every pin claim belonging to the supplied owner.
 * @param owner Owner token whose claims are released.
 */
void release(Owner owner);
/** @brief Releases one pin claim belonging to the supplied owner.
 * @param owner Owner token whose claim is released.
 * @param pin Physical pad to release.
 */
void release(Owner owner, PinId pin);
} // namespace newmota::runtime_pins
