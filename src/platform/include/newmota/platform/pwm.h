/*
  pwm.h - portable hardware PWM contract for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/core/hardware/pin.h"

#include <cstddef>
#include <cstdint>

namespace newmota::platform {

inline constexpr std::size_t kMaximumPwmOutputs = 16U;
inline constexpr std::uint16_t kMaximumPwmErrorPpm = 50'000U;

/** @brief Identifies why a complete PWM request or operation was rejected. */
enum class PwmResult : std::uint8_t {
    success,
    invalid_request,
    too_many_outputs,
    duplicate_output,
    duplicate_handle,
    duplicate_pin,
    pin_unavailable,
    unsupported_route,
    resource_unavailable,
    incompatible,
    requirements_unsatisfied,
    ownership_conflict,
    stale_plan,
    stale_handle,
    native_failure,
    faulted,
    quarantined,
    no_memory,
};

/** @brief Logical PWM service namespace, ordered by its stable numeric value. */
enum class PwmOutputOwner : std::uint16_t {
    none = 0U,
    pwm_outputs = 1U,
    display = 2U,
    status_led = 3U,
    berry = 4U,
};

/** @brief Stable logical owner and instance identity supplied by a service. */
struct PwmOutputId {
    PwmOutputOwner owner{PwmOutputOwner::none};
    std::uint16_t instance{};
};

/** @brief Request for one retained logical output. */
struct PwmRequest {
    PwmOutputId logical_output{};
    PinId pin{};
    std::uint32_t frequency_hz{};
    std::uint16_t max_error_ppm{};
    std::uint8_t min_resolution_bits{};
    bool inverted{};
    std::uint16_t initial_duty{};
};

/** @brief Exact carrier frequency represented as a rational number of hertz. */
struct PwmFrequency {
    std::uint64_t numerator_hz{};
    std::uint64_t denominator{};
};

/** @brief Effective settings returned for one admitted output. */
struct PwmEffectiveSettings {
    PwmFrequency frequency{};
    std::uint8_t resolution_bits{};
};

/** @brief Opaque retained handle; the generation changes when a slot is reused. */
struct PwmHandle {
    static constexpr std::uint8_t kInvalidSlot = 0xffU;

    std::uint8_t slot{kInvalidSlot};
    std::uint32_t generation{};

    [[nodiscard]] constexpr bool valid() const { return slot != kInvalidSlot && generation != 0U; }
};

/** @brief Explicitly changes retained outputs that share a timing resource. */
struct PwmReconfiguration {
    PwmHandle handle{};
    PwmRequest request{};
};

/** @brief Opaque token for a staged plan against one live allocation generation. */
struct PwmPlanToken {
    std::uint16_t slot{0xffffU};
    std::uint32_t generation{};

    [[nodiscard]] constexpr bool valid() const { return slot != 0xffffU && generation != 0U; }
};

/** @brief Per-output result from request validation or activation. */
struct PwmRequestResult {
    PwmResult result{PwmResult::invalid_request};
    PwmHandle handle{};
    PwmEffectiveSettings effective{};
};

/** @brief One requested normalized duty update. */
struct PwmDutyUpdate {
    PwmHandle handle{};
    std::uint16_t duty{};
};

/** @brief One output's confirmed result from a batch operation. */
enum class PwmApplyState : std::uint8_t {
    not_applied,
    applied,
    failed,
    unknown,
};

struct PwmOperationResult {
    PwmHandle handle{};
    PwmApplyState state{PwmApplyState::not_applied};
    PwmResult result{PwmResult::invalid_request};
};

/** @brief Retained state for an output, including failures from shared hardware. */
enum class PwmOutputState : std::uint8_t {
    active,
    faulted,
    quarantined,
};

struct PwmOutputStatus {
    PwmOutputState state{PwmOutputState::faulted};
    PwmEffectiveSettings effective{};
    std::uint16_t applied_duty{};
};

/**
 * @brief Stages a bounded set of new outputs without changing hardware.
 *
 * Requests are additions to the retained active set. Existing outputs omitted
 * from this call remain active and unchanged. The result array has @p count
 * entries. New requests are admitted in ascending (owner, instance) order,
 * independent of input order; results remain in input order. A newer prepare,
 * activation, release, or reconfiguration invalidates the previous staged
 * token. Duty writes preserve an additions-only plan; they invalidate a staged
 * reconfiguration because it was checked against the previous applied duties.
 * The caller must obtain each pin from the resolved hardware binding owner;
 * topology availability alone does not authorize taking a relay's pad.
 */
[[nodiscard]] PwmResult pwm_prepare(
    const PwmRequest* requests, std::size_t count, PwmPlanToken& plan, PwmRequestResult* results);
/**
 * @brief Stages a complete update for every output affected by retuning.
 *
 * The request identity and physical pin must match the retained handle. Every
 * active output whose effective timing would change must participate.
 */
[[nodiscard]] PwmResult pwm_prepare_reconfigure(const PwmReconfiguration* updates,
    std::size_t count, PwmPlanToken& plan, PwmRequestResult* results);
/**
 * @brief Activates a current staged plan; results retain input order.
 *
 * @p result_capacity must be at least the request count passed to prepare.
 */
[[nodiscard]] PwmResult pwm_activate(
    PwmPlanToken plan, PwmRequestResult* results, std::size_t result_capacity);
/**
 * @brief Applies a fully validated batch and reports each output separately.
 *
 * The result array has @p count entries. Success means programming was
 * confirmed; a hardware-latched update may become electrically visible at a
 * later documented counter boundary. Unknown results fault every affected
 * shared failure domain, including retained handles outside this batch.
 */
[[nodiscard]] PwmResult pwm_write_batch(
    const PwmDutyUpdate* updates, std::size_t count, PwmOperationResult* results);
/** @brief Safely stops and releases one retained output handle. */
[[nodiscard]] PwmResult pwm_release(PwmHandle handle);
/** @brief Reads retained state, including shared-resource faults. */
[[nodiscard]] PwmResult pwm_get_status(PwmHandle handle, PwmOutputStatus& status);

} // namespace newmota::platform
