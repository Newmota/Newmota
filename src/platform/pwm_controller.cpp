/*
  pwm_controller.cpp - shared retained PWM lifecycle manager

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/gpio_detail.h"
#include "newmota/platform/pwm_detail.h"

#include <algorithm>
#include <limits>
#include <new>

namespace newmota::platform::detail {
namespace {

/** @brief Compares tagged logical output identities. */
[[nodiscard]] bool same_output_id(const PwmOutputId& left, const PwmOutputId& right) {
    return left.owner == right.owner && left.instance == right.instance;
}

/** @brief Maps a retained non-active state to its public operation result. */
[[nodiscard]] PwmResult result_for_state(const PwmOutputState state) {
    switch (state) {
    case PwmOutputState::active:
        return PwmResult::success;
    case PwmOutputState::faulted:
        return PwmResult::faulted;
    case PwmOutputState::quarantined:
        return PwmResult::quarantined;
    }
    return PwmResult::native_failure;
}

/** @brief Adds or strengthens a shared failure-domain state. */
void add_affected_domain(std::uint16_t domain, PwmOutputState state, std::uint16_t* domains,
    PwmOutputState* states, std::size_t& count) {
    if (domain == 0U) {
        return;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        if (domains[index] == domain) {
            if (state == PwmOutputState::quarantined) {
                states[index] = state;
            }
            return;
        }
    }
    if (count < kMaximumPwmOutputs) {
        domains[count] = domain;
        states[count] = state;
        ++count;
    }
}

} // namespace

/** @brief Creates a bounded controller around one platform's native adapter. */
PwmController::PwmController(const PwmBackendOps& backend, void* const context)
    : backend_(backend), context_(context) {}

/** @brief Advances a retained generation while reserving zero as invalid. */
std::uint32_t PwmController::next_generation(const std::uint32_t value) {
    return value == std::numeric_limits<std::uint32_t>::max() ? 1U : value + 1U;
}

/** @brief Finds one active or quarantined retained handle. */
const PwmController::OutputRecord* PwmController::find(const PwmHandle handle) const {
    if (!handle.valid() || handle.slot >= kMaximumPwmOutputs) {
        return nullptr;
    }
    const auto& output = outputs_[handle.slot];
    return output.used && output.generation == handle.generation ? &output : nullptr;
}

/** @brief Finds one mutable active or quarantined retained handle. */
PwmController::OutputRecord* PwmController::find(const PwmHandle handle) {
    return const_cast<OutputRecord*>(static_cast<const PwmController*>(this)->find(handle));
}

/** @brief Copies every retained claim for a native plan or release call. */
std::size_t PwmController::collect_active(PwmBackendActiveOutput* const active) const {
    std::size_t count = 0U;
    for (const auto& record : outputs_) {
        if (record.used) {
            active[count] = record.output;
            ++count;
        }
    }
    return count;
}

/** @brief Marks all retained claimants in one confirmed affected domain. */
void PwmController::apply_domain_state(const std::uint16_t domain, const PwmOutputState state) {
    if (domain == 0U) {
        return;
    }
    for (auto& record : outputs_) {
        if (record.used && record.output.failure_domain == domain &&
            state != PwmOutputState::active) {
            record.output.state = state;
        }
    }
}

/** @brief Allocates the plan and scratch unless a prepare already holds them. */
bool PwmController::begin_planning() {
    if (!planning_) {
        planning_.reset(new (std::nothrow) Planning{});
    }
    return planning_ != nullptr;
}

/** @brief Frees the staged plan and the adapter's native plan. */
void PwmController::drop_planning() {
    planning_.reset();
    if (backend_.discard != nullptr) {
        backend_.discard(context_);
    }
}

/** @brief Plans new outputs without touching active native waveforms. */
PwmResult PwmController::prepare(const PwmRequest* const requests, const std::size_t count,
    PwmPlanToken& plan, PwmRequestResult* const results) {
    drop_planning();
    return stage(requests, nullptr, nullptr, count, false, plan, results);
}

/** @brief Plans an explicit update for each participating retained handle. */
PwmResult PwmController::prepare_reconfigure(const PwmReconfiguration* const updates,
    const std::size_t count, PwmPlanToken& plan, PwmRequestResult* const results) {
    drop_planning();
    plan = {};
    if (count == 0U || count > kMaximumPwmOutputs || updates == nullptr || results == nullptr) {
        return count > kMaximumPwmOutputs ? PwmResult::too_many_outputs
                                          : PwmResult::invalid_request;
    }
    if (!begin_planning()) {
        return PwmResult::no_memory;
    }

    auto& requests = planning_->workspace.reconfiguration_requests;
    auto& handles = planning_->workspace.reconfiguration_handles;
    auto& pre_errors = planning_->workspace.pre_errors;
    for (std::size_t index = 0U; index < count; ++index) {
        requests[index] = updates[index].request;
        handles[index] = updates[index].handle;
        pre_errors[index] = PwmResult::success;
        const auto* const record = find(handles[index]);
        if (record == nullptr) {
            pre_errors[index] = PwmResult::stale_handle;
            continue;
        }
        if (record->output.state != PwmOutputState::active) {
            pre_errors[index] = result_for_state(record->output.state);
            continue;
        }
        if (!same_output_id(
                requests[index].logical_output, record->output.request.logical_output) ||
            requests[index].pin.value() != record->output.request.pin.value()) {
            pre_errors[index] = PwmResult::ownership_conflict;
        }
    }

    for (std::size_t first = 0U; first < count; ++first) {
        if (pre_errors[first] != PwmResult::success) {
            continue;
        }
        for (std::size_t second = first + 1U; second < count; ++second) {
            if (pre_errors[second] == PwmResult::success &&
                handles[first].slot == handles[second].slot &&
                handles[first].generation == handles[second].generation) {
                pre_errors[first] = PwmResult::duplicate_handle;
                pre_errors[second] = PwmResult::duplicate_handle;
            }
        }
    }
    return stage(requests, handles, pre_errors, count, true, plan, results);
}

/** @brief Validates, sorts, and asks the adapter to stage a whole request set. */
PwmResult PwmController::stage(const PwmRequest* const requests, const PwmHandle* const handles,
    const PwmResult* const pre_errors, const std::size_t count, const bool reconfiguration,
    PwmPlanToken& plan, PwmRequestResult* const results) {
    plan = {};
    if (count == 0U || requests == nullptr || results == nullptr) {
        drop_planning();
        return PwmResult::invalid_request;
    }
    if (count > kMaximumPwmOutputs) {
        drop_planning();
        return PwmResult::too_many_outputs;
    }
    if (!begin_planning()) {
        return PwmResult::no_memory;
    }
    auto& workspace = planning_->workspace;

    auto& eligible = workspace.eligible;
    auto& compact = workspace.compact;
    auto& compact_to_input = workspace.compact_to_input;
    auto& conflicts = workspace.conflicts;
    auto& active = workspace.active;
    auto& reserved = workspace.reserved;
    auto& selected = workspace.selected;
    auto& order = workspace.order;
    auto& accepted_requests = workspace.accepted_requests;
    auto& accepted_handles = workspace.accepted_handles;
    auto& accepted_order = workspace.accepted_order;
    auto& native_results = workspace.native_results;
    for (std::size_t index = 0U; index < count; ++index) {
        eligible[index] = false;
        reserved[index] = false;
        selected[index] = false;
        native_results[index] = {};
    }
    std::size_t compact_count = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        results[index] = {};
        if (handles != nullptr) {
            results[index].handle = handles[index];
        }
        if (pre_errors != nullptr && pre_errors[index] != PwmResult::success) {
            results[index].result = pre_errors[index];
            continue;
        }
        results[index].result = validate_pwm_request(requests[index]);
        if (results[index].result == PwmResult::success) {
            eligible[index] = true;
            compact[compact_count] = requests[index];
            compact_to_input[compact_count] = index;
            ++compact_count;
        }
    }

    if (compact_count > 0U) {
        (void)validate_pwm_request_set(compact, compact_count, conflicts);
        for (std::size_t index = 0U; index < compact_count; ++index) {
            const auto input = compact_to_input[index];
            if (conflicts[index].result != PwmResult::success) {
                results[input].result = conflicts[index].result;
                eligible[input] = false;
            }
        }
    }

    const auto active_count = collect_active(active);
    for (const auto& record : outputs_) {
        if (record.used) {
            reserved[record.output.handle.slot] = true;
        }
    }
    std::size_t order_count = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        if (!eligible[index]) {
            continue;
        }
        const auto pin_result =
            validate_gpio(requests[index].pin, capability_mask(PinCapability::output));
        if (pin_result == GpioResult::unavailable) {
            results[index].result = PwmResult::pin_unavailable;
            eligible[index] = false;
            continue;
        }
        if (pin_result != GpioResult::success) {
            results[index].result = PwmResult::unsupported_route;
            eligible[index] = false;
            continue;
        }

        bool collides_with_active = false;
        for (std::size_t active_index = 0U; active_index < active_count; ++active_index) {
            const auto& existing = active[active_index];
            const bool is_reconfiguration_target =
                reconfiguration && handles != nullptr &&
                handles[index].slot == existing.handle.slot &&
                handles[index].generation == existing.handle.generation;
            if (!is_reconfiguration_target &&
                (same_output_id(requests[index].logical_output, existing.request.logical_output) ||
                    requests[index].pin.value() == existing.request.pin.value())) {
                collides_with_active = true;
                break;
            }
        }
        if (collides_with_active) {
            results[index].result = PwmResult::ownership_conflict;
            eligible[index] = false;
            continue;
        }

        // Insert by stable owner and instance identity; ties were rejected above.
        std::size_t position = order_count;
        while (position > 0U) {
            const auto prior = order[position - 1U];
            const auto& prior_id = requests[prior].logical_output;
            const auto& current_id = requests[index].logical_output;
            if (prior_id.owner < current_id.owner ||
                (prior_id.owner == current_id.owner && prior_id.instance < current_id.instance)) {
                break;
            }
            order[position] = prior;
            --position;
        }
        order[position] = index;
        ++order_count;
    }

    std::size_t accepted_count = 0U;
    plan_generation_ = next_generation(plan_generation_);
    plan = {0U, plan_generation_};
    PwmResult native_result = PwmResult::success;

    if (reconfiguration) {
        for (std::size_t position = 0U; position < order_count; ++position) {
            const auto input = order[position];
            accepted_requests[position] = requests[input];
            accepted_handles[position] = handles[input];
            accepted_order[position] = input;
        }
        accepted_count = order_count;
    } else {
        bool retry = true;
        while (retry) {
            retry = false;
            std::fill_n(reserved, kMaximumPwmOutputs, false);
            std::fill_n(selected, kMaximumPwmOutputs, false);
            for (const auto& record : outputs_) {
                if (record.used) {
                    reserved[record.output.handle.slot] = true;
                }
            }
            accepted_count = 0U;
            for (std::size_t position = 0U; position < order_count; ++position) {
                const auto input = order[position];
                if (!eligible[input]) {
                    continue;
                }
                std::size_t slot = 0U;
                while (slot < kMaximumPwmOutputs && reserved[slot]) {
                    ++slot;
                }
                if (slot == kMaximumPwmOutputs) {
                    break;
                }
                reserved[slot] = true;
                selected[input] = true;
                accepted_requests[accepted_count] = requests[input];
                accepted_handles[accepted_count] = {
                    static_cast<std::uint8_t>(slot), next_generation(outputs_[slot].generation)};
                accepted_order[accepted_count] = input;
                native_results[accepted_count] = {};
                ++accepted_count;
            }
            if (accepted_count == 0U || backend_.prepare == nullptr) {
                if (accepted_count > 0U) {
                    native_result = PwmResult::native_failure;
                    for (std::size_t position = 0U; position < accepted_count; ++position) {
                        results[accepted_order[position]].result = native_result;
                        eligible[accepted_order[position]] = false;
                    }
                    retry = true;
                }
                continue;
            }
            native_result = backend_.prepare(context_, plan, false, active, active_count,
                accepted_requests, accepted_handles, accepted_count, native_results);
            for (std::size_t position = 0U; position < accepted_count; ++position) {
                auto planned_result = native_results[position].result;
                if (native_result != PwmResult::success &&
                    planned_result == PwmResult::invalid_request) {
                    planned_result = native_result;
                }
                if (planned_result == PwmResult::success &&
                    (!pwm_frequency_meets_request(native_results[position].effective.frequency,
                         accepted_requests[position]) ||
                        native_results[position].effective.resolution_bits <
                            accepted_requests[position].min_resolution_bits)) {
                    planned_result = PwmResult::requirements_unsatisfied;
                }
                if (planned_result != PwmResult::success) {
                    results[accepted_order[position]].result = planned_result;
                    eligible[accepted_order[position]] = false;
                    retry = true;
                }
            }
        }
        for (std::size_t position = 0U; position < order_count; ++position) {
            const auto input = order[position];
            if (eligible[input] && !selected[input]) {
                results[input].result = PwmResult::resource_unavailable;
                eligible[input] = false;
            }
        }
    }

    if (reconfiguration && accepted_count > 0U) {
        if (backend_.prepare == nullptr) {
            native_result = PwmResult::native_failure;
        } else {
            native_result = backend_.prepare(context_, plan, true, active, active_count,
                accepted_requests, accepted_handles, accepted_count, native_results);
        }
    }

    // prepare() and prepare_reconfigure() allocated a fresh plan.
    auto& staged = planning_->staged;
    staged.reconfiguration = reconfiguration;
    staged.token = plan;
    staged.result_count = count;
    for (std::size_t index = 0U; index < count; ++index) {
        staged.initial_results[index] = results[index];
    }
    for (std::size_t position = 0U; position < accepted_count; ++position) {
        const auto input = accepted_order[position];
        auto planned_result = native_results[position].result;
        if (native_result != PwmResult::success && planned_result == PwmResult::invalid_request) {
            planned_result = native_result;
        }
        if (planned_result == PwmResult::success &&
            (!pwm_frequency_meets_request(
                 native_results[position].effective.frequency, accepted_requests[position]) ||
                native_results[position].effective.resolution_bits <
                    accepted_requests[position].min_resolution_bits)) {
            planned_result = PwmResult::requirements_unsatisfied;
        }
        results[input].result = planned_result;
        if (planned_result != PwmResult::success) {
            continue;
        }
        results[input].effective = native_results[position].effective;
        if (reconfiguration) {
            results[input].handle = accepted_handles[position];
        }
        auto& staged_output = staged.outputs[staged.output_count];
        staged_output.input_index = input;
        staged_output.request = accepted_requests[position];
        staged_output.handle = accepted_handles[position];
        staged_output.effective = native_results[position].effective;
        staged_output.failure_domain = native_results[position].failure_domain;
        ++staged.output_count;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        staged.initial_results[index] = results[index];
    }
    staged.valid = true;
    return PwmResult::success;
}

/** @brief Applies one current staged plan and retains confirmed outputs. */
PwmResult PwmController::activate(
    const PwmPlanToken plan, PwmRequestResult* const results, const std::size_t result_capacity) {
    if (!planning_ || !planning_->staged.valid || !plan.valid() ||
        planning_->staged.token.slot != plan.slot ||
        planning_->staged.token.generation != plan.generation) {
        return PwmResult::stale_plan;
    }
    const auto& staged = planning_->staged;
    if (results == nullptr || result_capacity < staged.result_count) {
        return PwmResult::invalid_request;
    }
    for (std::size_t index = 0U; index < staged.result_count; ++index) {
        results[index] = staged.initial_results[index];
    }

    PwmRequest requests[kMaximumPwmOutputs]{};
    PwmHandle handles[kMaximumPwmOutputs]{};
    PwmBackendOperationResult native_results[kMaximumPwmOutputs]{};
    for (std::size_t index = 0U; index < staged.output_count; ++index) {
        requests[index] = staged.outputs[index].request;
        handles[index] = staged.outputs[index].handle;
    }
    PwmResult native_result = PwmResult::success;
    if (staged.output_count > 0U) {
        native_result = backend_.activate == nullptr
                            ? PwmResult::native_failure
                            : backend_.activate(context_, plan, staged.reconfiguration, requests,
                                  handles, staged.output_count, native_results);
    }

    std::uint16_t affected_domains[kMaximumPwmOutputs]{};
    PwmOutputState affected_states[kMaximumPwmOutputs]{};
    std::size_t affected_count = 0U;
    for (std::size_t index = 0U; index < staged.output_count; ++index) {
        const auto& staged_output = staged.outputs[index];
        auto operation = native_results[index];
        if (native_result != PwmResult::success && operation.result == PwmResult::native_failure &&
            operation.applied == PwmApplyState::not_applied) {
            operation.result = native_result;
        }
        const bool inconsistent =
            operation.applied == PwmApplyState::applied && operation.result != PwmResult::success;
        const bool confirmed = operation.applied == PwmApplyState::applied &&
                               operation.result == PwmResult::success &&
                               operation.state == PwmOutputState::active;
        const bool uncertain = operation.applied == PwmApplyState::unknown ||
                               operation.state != PwmOutputState::active || inconsistent;
        OutputRecord* record = nullptr;
        if (staged.reconfiguration) {
            record = find(staged_output.handle);
            if (record != nullptr && confirmed) {
                record->output.request = staged_output.request;
                record->output.effective = staged_output.effective;
                record->output.failure_domain = operation.failure_domain;
                record->output.state = PwmOutputState::active;
            }
        } else if (confirmed || uncertain) {
            auto& candidate = outputs_[staged_output.handle.slot];
            candidate.used = true;
            candidate.generation = staged_output.handle.generation;
            candidate.output = {staged_output.request, staged_output.handle,
                staged_output.effective, operation.state, staged_output.request.initial_duty,
                operation.failure_domain};
            if (uncertain && operation.state == PwmOutputState::active) {
                candidate.output.state = PwmOutputState::faulted;
            }
            record = &candidate;
        }

        if (uncertain && record != nullptr) {
            record->output.failure_domain = operation.failure_domain;
            record->output.state = operation.state == PwmOutputState::active
                                       ? PwmOutputState::faulted
                                       : operation.state;
            add_affected_domain(operation.failure_domain, record->output.state, affected_domains,
                affected_states, affected_count);
        }

        auto& output_result = results[staged_output.input_index];
        output_result.handle = record != nullptr ? record->output.handle : PwmHandle{};
        if (confirmed) {
            output_result.result = PwmResult::success;
            output_result.effective = staged_output.effective;
        } else if (uncertain && record != nullptr) {
            output_result.result = result_for_state(record->output.state);
            output_result.effective = staged_output.effective;
        } else {
            output_result.result = operation.result == PwmResult::success
                                       ? PwmResult::native_failure
                                       : operation.result;
        }
    }

    for (std::size_t index = 0U; index < affected_count; ++index) {
        apply_domain_state(affected_domains[index], affected_states[index]);
    }
    // A successful sibling in a domain with an uncertain peer is not confirmed.
    for (std::size_t index = 0U; index < staged.output_count; ++index) {
        const auto& staged_output = staged.outputs[index];
        auto& output_result = results[staged_output.input_index];
        const auto* const record = find(staged_output.handle);
        if (record != nullptr && record->output.state != PwmOutputState::active) {
            output_result.handle = record->output.handle;
            output_result.result = result_for_state(record->output.state);
        }
    }
    drop_planning();
    return PwmResult::success;
}

/** @brief Validates a complete duty batch before delegating changed values. */
PwmResult PwmController::write_batch(const PwmDutyUpdate* const updates, const std::size_t count,
    PwmOperationResult* const results) {
    if (planning_ && planning_->staged.reconfiguration) {
        drop_planning();
    }
    if (count == 0U || updates == nullptr || results == nullptr) {
        return PwmResult::invalid_request;
    }
    if (count > kMaximumPwmOutputs) {
        return PwmResult::too_many_outputs;
    }

    bool valid = true;
    for (std::size_t index = 0U; index < count; ++index) {
        results[index] = {updates[index].handle, PwmApplyState::not_applied, PwmResult::success};
        const auto* const record = find(updates[index].handle);
        if (record == nullptr) {
            results[index].result = PwmResult::stale_handle;
            valid = false;
        } else if (record->output.state != PwmOutputState::active) {
            results[index].result = result_for_state(record->output.state);
            valid = false;
        }
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (updates[prior].handle.slot == updates[index].handle.slot &&
                updates[prior].handle.generation == updates[index].handle.generation) {
                results[prior].result = PwmResult::duplicate_handle;
                results[index].result = PwmResult::duplicate_handle;
                valid = false;
            }
        }
    }
    if (!valid) {
        return PwmResult::invalid_request;
    }

    PwmDutyUpdate changed[kMaximumPwmOutputs]{};
    std::size_t changed_to_input[kMaximumPwmOutputs]{};
    std::size_t changed_count = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        const auto* const record = find(updates[index].handle);
        if (record->output.applied_duty == updates[index].duty) {
            results[index].state = PwmApplyState::applied;
            continue;
        }
        changed[changed_count] = updates[index];
        changed_to_input[changed_count] = index;
        ++changed_count;
    }
    if (changed_count == 0U) {
        return PwmResult::success;
    }

    PwmBackendOperationResult native_results[kMaximumPwmOutputs]{};
    const auto native_result =
        backend_.write_batch == nullptr
            ? PwmResult::native_failure
            : backend_.write_batch(context_, changed, changed_count, native_results);
    std::uint16_t affected_domains[kMaximumPwmOutputs]{};
    PwmOutputState affected_states[kMaximumPwmOutputs]{};
    std::size_t affected_count = 0U;
    for (std::size_t index = 0U; index < changed_count; ++index) {
        const auto input = changed_to_input[index];
        auto operation = native_results[index];
        if (native_result != PwmResult::success && operation.result == PwmResult::native_failure &&
            operation.applied == PwmApplyState::not_applied) {
            operation.result = native_result;
        }
        auto* const record = find(changed[index].handle);
        const bool inconsistent =
            operation.applied == PwmApplyState::applied && operation.result != PwmResult::success;
        const bool confirmed = operation.applied == PwmApplyState::applied &&
                               operation.result == PwmResult::success &&
                               operation.state == PwmOutputState::active;
        const bool uncertain = operation.applied == PwmApplyState::unknown ||
                               operation.state != PwmOutputState::active || inconsistent;
        results[input].state = operation.applied;
        results[input].result = operation.result;
        if (confirmed) {
            record->output.applied_duty = changed[index].duty;
        }
        if (uncertain) {
            const auto state = operation.state == PwmOutputState::active ? PwmOutputState::faulted
                                                                         : operation.state;
            record->output.state = state;
            if (operation.failure_domain != 0U) {
                record->output.failure_domain = operation.failure_domain;
            }
            add_affected_domain(record->output.failure_domain, state, affected_domains,
                affected_states, affected_count);
            results[input].state = PwmApplyState::unknown;
            results[input].result = result_for_state(state);
        } else if (!confirmed) {
            results[input].state = operation.applied == PwmApplyState::not_applied
                                       ? PwmApplyState::failed
                                       : operation.applied;
            if (results[input].result == PwmResult::success) {
                results[input].result = PwmResult::native_failure;
            }
        }
    }

    for (std::size_t index = 0U; index < affected_count; ++index) {
        apply_domain_state(affected_domains[index], affected_states[index]);
    }
    for (std::size_t index = 0U; index < count; ++index) {
        const auto* const record = find(updates[index].handle);
        if (record != nullptr && record->output.state != PwmOutputState::active) {
            results[index].state = PwmApplyState::unknown;
            results[index].result = result_for_state(record->output.state);
        }
    }
    return native_result;
}

/** @brief Safely releases one retained output or keeps its failed claim. */
PwmResult PwmController::release(const PwmHandle handle) {
    drop_planning();
    auto* const record = find(handle);
    if (record == nullptr) {
        return PwmResult::stale_handle;
    }
    if (backend_.release == nullptr) {
        return PwmResult::native_failure;
    }

    PwmBackendOperationResult operation{};
    const auto native_result = backend_.release(context_, record->output, operation);
    if (operation.applied == PwmApplyState::applied && operation.result == PwmResult::success &&
        operation.state == PwmOutputState::active) {
        record->used = false;
        record->output = {};
        return PwmResult::success;
    }

    if (operation.applied == PwmApplyState::unknown || operation.state != PwmOutputState::active) {
        const auto state =
            operation.state == PwmOutputState::active ? PwmOutputState::faulted : operation.state;
        record->output.state = state;
        if (operation.failure_domain != 0U) {
            record->output.failure_domain = operation.failure_domain;
        }
        apply_domain_state(record->output.failure_domain, state);
        return result_for_state(state);
    }
    if (native_result != PwmResult::success) {
        return native_result;
    }
    return operation.result == PwmResult::success ? PwmResult::native_failure : operation.result;
}

/** @brief Reports retained effective state, including domain-wide faults. */
PwmResult PwmController::get_status(const PwmHandle handle, PwmOutputStatus& status) const {
    const auto* const record = find(handle);
    if (record == nullptr) {
        return PwmResult::stale_handle;
    }
    status = {record->output.state, record->output.effective, record->output.applied_duty};
    return result_for_state(record->output.state);
}

} // namespace newmota::platform::detail
