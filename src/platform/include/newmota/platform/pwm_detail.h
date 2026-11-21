/*
  pwm_detail.h - shared PWM request and arithmetic policy

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/platform/pwm.h"

#include <memory>

namespace newmota::platform::detail {

/** @brief Retained application claim passed to a native resource planner. */
struct PwmBackendActiveOutput {
    PwmRequest request{};
    PwmHandle handle{};
    PwmEffectiveSettings effective{};
    PwmOutputState state{PwmOutputState::faulted};
    std::uint16_t applied_duty{};
    std::uint16_t failure_domain{};
};

/** @brief Native planner result for one sorted staged request. */
struct PwmBackendPlanResult {
    PwmResult result{PwmResult::invalid_request};
    PwmEffectiveSettings effective{};
    std::uint16_t failure_domain{};
};

/** @brief Native operation outcome with private failure-domain metadata. */
struct PwmBackendOperationResult {
    PwmApplyState applied{PwmApplyState::not_applied};
    PwmResult result{PwmResult::native_failure};
    PwmOutputState state{PwmOutputState::active};
    std::uint16_t failure_domain{};
};

/** @brief Native adapter callbacks; native routes never enter the public API. */
struct PwmBackendOps {
    PwmResult (*prepare)(void* context, PwmPlanToken token, bool reconfiguration,
        const PwmBackendActiveOutput* active, std::size_t active_count, const PwmRequest* requests,
        const PwmHandle* handles, std::size_t count, PwmBackendPlanResult* results){};
    PwmResult (*activate)(void* context, PwmPlanToken token, bool reconfiguration,
        const PwmRequest* requests, const PwmHandle* handles, std::size_t count,
        PwmBackendOperationResult* results){};
    PwmResult (*write_batch)(void* context, const PwmDutyUpdate* updates, std::size_t count,
        PwmBackendOperationResult* results){};
    PwmResult (*release)(
        void* context, const PwmBackendActiveOutput& output, PwmBackendOperationResult& result){};
    /** @brief Frees a native plan that will not be activated; optional. */
    void (*discard)(void* context){};
};

/** @brief Platform supplies one native PWM implementation; plans live on the heap. */
[[nodiscard]] const PwmBackendOps& pwm_native_backend();
[[nodiscard]] void* pwm_native_context();

/** @brief Shared bounded lifecycle manager, instantiated by host fakes/native API. */
class PwmController final {
  public:
    PwmController(const PwmBackendOps& backend, void* context);

    [[nodiscard]] PwmResult prepare(const PwmRequest* requests, std::size_t count,
        PwmPlanToken& plan, PwmRequestResult* results);
    [[nodiscard]] PwmResult prepare_reconfigure(const PwmReconfiguration* updates,
        std::size_t count, PwmPlanToken& plan, PwmRequestResult* results);
    [[nodiscard]] PwmResult activate(
        PwmPlanToken plan, PwmRequestResult* results, std::size_t result_capacity);
    [[nodiscard]] PwmResult write_batch(
        const PwmDutyUpdate* updates, std::size_t count, PwmOperationResult* results);
    [[nodiscard]] PwmResult release(PwmHandle handle);
    [[nodiscard]] PwmResult get_status(PwmHandle handle, PwmOutputStatus& status) const;

  private:
    struct OutputRecord {
        bool used{};
        std::uint32_t generation{};
        PwmBackendActiveOutput output{};
    };

    struct StagedOutput {
        std::size_t input_index{};
        PwmRequest request{};
        PwmHandle handle{};
        PwmEffectiveSettings effective{};
        std::uint16_t failure_domain{};
    };

    struct StagedPlan {
        bool valid{};
        bool reconfiguration{};
        PwmPlanToken token{};
        std::size_t result_count{};
        std::size_t output_count{};
        PwmRequestResult initial_results[kMaximumPwmOutputs]{};
        StagedOutput outputs[kMaximumPwmOutputs]{};
    };

    // Preparation uses heap scratch instead of placing several request/result
    // matrices on the platform's 8 KiB application stack.
    struct Workspace {
        bool eligible[kMaximumPwmOutputs]{};
        PwmRequest compact[kMaximumPwmOutputs]{};
        std::size_t compact_to_input[kMaximumPwmOutputs]{};
        PwmRequestResult conflicts[kMaximumPwmOutputs]{};
        PwmBackendActiveOutput active[kMaximumPwmOutputs]{};
        bool reserved[kMaximumPwmOutputs]{};
        bool selected[kMaximumPwmOutputs]{};
        std::size_t order[kMaximumPwmOutputs]{};
        PwmRequest accepted_requests[kMaximumPwmOutputs]{};
        PwmHandle accepted_handles[kMaximumPwmOutputs]{};
        std::size_t accepted_order[kMaximumPwmOutputs]{};
        PwmBackendPlanResult native_results[kMaximumPwmOutputs]{};
        PwmRequest reconfiguration_requests[kMaximumPwmOutputs]{};
        PwmHandle reconfiguration_handles[kMaximumPwmOutputs]{};
        PwmResult pre_errors[kMaximumPwmOutputs]{};
    };

    /** @brief Exists from a prepare until its activation or invalidation. */
    struct Planning {
        StagedPlan staged{};
        Workspace workspace{};
    };

    [[nodiscard]] PwmResult stage(const PwmRequest* requests, const PwmHandle* handles,
        const PwmResult* pre_errors, std::size_t count, bool reconfiguration, PwmPlanToken& plan,
        PwmRequestResult* results);
    [[nodiscard]] const OutputRecord* find(PwmHandle handle) const;
    [[nodiscard]] OutputRecord* find(PwmHandle handle);
    [[nodiscard]] std::size_t collect_active(PwmBackendActiveOutput* active) const;
    void apply_domain_state(std::uint16_t domain, PwmOutputState state);
    [[nodiscard]] bool begin_planning();
    void drop_planning();
    [[nodiscard]] static std::uint32_t next_generation(std::uint32_t value);

    const PwmBackendOps& backend_;
    void* context_{};
    OutputRecord outputs_[kMaximumPwmOutputs]{};
    std::unique_ptr<Planning> planning_;
    std::uint32_t plan_generation_{};
};

/** @brief Checks all hard request bounds without consulting native routing. */
[[nodiscard]] PwmResult validate_pwm_request(const PwmRequest& request);

/** @brief Validates every request and marks duplicate identities and pads. */
[[nodiscard]] PwmResult validate_pwm_request_set(
    const PwmRequest* requests, std::size_t count, PwmRequestResult* results);

/**
 * @brief Compares two nonnegative fractions without cross multiplication.
 * @param left_numerator Left numerator.
 * @param left_denominator Left denominator, nonzero.
 * @param right_numerator Right numerator.
 * @param right_denominator Right denominator, nonzero.
 * @return Negative, zero, or positive when left is less than, equal to, or greater than right.
 */
[[nodiscard]] int compare_fractions(std::uint64_t left_numerator, std::uint64_t left_denominator,
    std::uint64_t right_numerator, std::uint64_t right_denominator);

/** @brief Compares an exact rational carrier against the request's error limit. */
[[nodiscard]] bool pwm_frequency_meets_request(
    const PwmFrequency& effective, const PwmRequest& request);

/**
 * @brief Returns resolution for uniformly spaced levels including both endpoints.
 *
 * Native adapters may use this result only when their representable normalized
 * duty levels are uniform and the endpoint strategy preserves those endpoints.
 */
[[nodiscard]] std::uint8_t pwm_resolution_bits(std::uint32_t intervals);

/** @brief Returns the interval floor required by a requested resolution. */
[[nodiscard]] std::uint32_t pwm_minimum_intervals(std::uint8_t resolution_bits);

/** @brief Projects normalized duty onto the nearest representable interval. */
[[nodiscard]] std::uint32_t pwm_duty_interval(std::uint16_t duty, std::uint32_t intervals);

} // namespace newmota::platform::detail
