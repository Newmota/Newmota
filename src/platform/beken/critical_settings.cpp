/*
  critical_settings.cpp - Beken NET-parameter critical-record transport

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file critical_settings.cpp
 * @brief Beken NET-parameter critical-record transport.
 */

#include "newmota/platform/critical_settings.h"
#include "newmota/log.h"

#include "flash_access.h"
#include "net_param_tlv.h"

#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "BkDriverFlash.h"
}

#ifndef NEWMOTA_NET_PARAM_OFFSET
#error "NEWMOTA_NET_PARAM_OFFSET is required"
#endif

#ifndef NEWMOTA_NET_PARAM_SIZE
#error "NEWMOTA_NET_PARAM_SIZE is required"
#endif

namespace newmota::platform {
namespace {

constexpr std::uint32_t kNetParamOffset = NEWMOTA_NET_PARAM_OFFSET;
constexpr std::uint32_t kNetParamSize = NEWMOTA_NET_PARAM_SIZE;
std::array<std::uint8_t, beken::kFlashSectorSize> net_param_scratch;

static_assert(kNetParamSize == beken::kFlashSectorSize);
static_assert(kNetParamOffset % beken::kFlashSectorSize == 0U);

[[nodiscard]] bool valid_native_partition() {
    const auto* const partition = bk_flash_get_info(BK_PARTITION_NET_PARAM);
    if (partition == nullptr || partition->partition_start_addr != kNetParamOffset ||
        partition->partition_length != kNetParamSize) {
        AddLog(LogLevel::error, "CFG: Beken NET partition does not match resolved layout");
        return false;
    }
    return true;
}

struct ReplaceContext final {
    const void* data;
    std::size_t size;
    beken::NetParamResult result{beken::NetParamResult::invalid};
};

beken::SectorMutationResult replace_record(void* const opaque, const std::uint8_t* const source,
    std::uint8_t* const destination, const std::size_t size) {
    auto& context = *static_cast<ReplaceContext*>(opaque);
    context.result = beken::replace_net_param_record(
        source, destination, size, beken::kNewmotaCriticalRecordType, context.data, context.size);
    switch (context.result) {
    case beken::NetParamResult::succeeded:
        return beken::SectorMutationResult::changed;
    case beken::NetParamResult::unchanged:
        return beken::SectorMutationResult::unchanged;
    case beken::NetParamResult::not_found:
    case beken::NetParamResult::invalid:
    case beken::NetParamResult::no_space:
        return beken::SectorMutationResult::failed;
    }
    return beken::SectorMutationResult::failed;
}

struct RemoveContext final {
    beken::NetParamResult result{beken::NetParamResult::invalid};
};

beken::SectorMutationResult remove_record(void* const opaque, const std::uint8_t* const source,
    std::uint8_t* const destination, const std::size_t size) {
    auto& context = *static_cast<RemoveContext*>(opaque);
    context.result = beken::remove_net_param_record(
        source, destination, size, beken::kNewmotaCriticalRecordType);
    switch (context.result) {
    case beken::NetParamResult::succeeded:
        return beken::SectorMutationResult::changed;
    case beken::NetParamResult::not_found:
    case beken::NetParamResult::unchanged:
        return beken::SectorMutationResult::unchanged;
    case beken::NetParamResult::invalid:
    case beken::NetParamResult::no_space:
        return beken::SectorMutationResult::failed;
    }
    return beken::SectorMutationResult::failed;
}

} // namespace

CriticalSettingsResult read_critical_settings(
    void* const data, const std::size_t capacity, std::size_t& size) {
    size = 0U;
    if (!valid_native_partition() || data == nullptr || capacity > kCriticalSettingsCapacity) {
        return CriticalSettingsResult::failed;
    }
    if (!beken::read_flash(kNetParamOffset, net_param_scratch.data(), net_param_scratch.size())) {
        AddLog(LogLevel::error, "CFG: Beken NET sector read failed");
        return CriticalSettingsResult::failed;
    }
    switch (beken::read_net_param_record(net_param_scratch.data(), net_param_scratch.size(),
        beken::kNewmotaCriticalRecordType, data, capacity, size)) {
    case beken::NetParamResult::succeeded:
        return CriticalSettingsResult::succeeded;
    case beken::NetParamResult::not_found:
        return CriticalSettingsResult::not_found;
    case beken::NetParamResult::unchanged:
    case beken::NetParamResult::invalid:
        AddLog(LogLevel::error, "CFG: Beken NET table is invalid");
        return CriticalSettingsResult::failed;
    case beken::NetParamResult::no_space:
        AddLog(LogLevel::error, "CFG: Beken critical record exceeds read capacity");
        return CriticalSettingsResult::failed;
    }
    return CriticalSettingsResult::failed;
}

CriticalSettingsResult replace_critical_settings(const void* const data, const std::size_t size) {
    if (!valid_native_partition() || data == nullptr || size == 0U ||
        size > kCriticalSettingsCapacity) {
        return CriticalSettingsResult::failed;
    }
    ReplaceContext context{data, size};
    if (!beken::mutate_flash_sector(kNetParamOffset, replace_record, &context)) {
        return CriticalSettingsResult::failed;
    }
    return context.result == beken::NetParamResult::succeeded ||
                   context.result == beken::NetParamResult::unchanged
               ? CriticalSettingsResult::succeeded
               : CriticalSettingsResult::failed;
}

CriticalSettingsResult remove_critical_settings() {
    if (!valid_native_partition()) {
        return CriticalSettingsResult::failed;
    }
    RemoveContext context{};
    if (!beken::mutate_flash_sector(kNetParamOffset, remove_record, &context)) {
        return CriticalSettingsResult::failed;
    }
    return context.result == beken::NetParamResult::not_found ? CriticalSettingsResult::not_found
                                                              : CriticalSettingsResult::succeeded;
}

} // namespace newmota::platform
