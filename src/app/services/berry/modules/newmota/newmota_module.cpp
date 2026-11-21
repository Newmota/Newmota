/*
  newmota_module.cpp - Berry command and garbage-collection interface

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/berry/newmota_module.h"

#include "core/command/command.h"
#include "newmota/log.h"
#include "newmota/platform/network.h"
#include "newmota/platform/platform.h"
#include "newmota_features.h"
#include "services/berry/berry_service.h"
#if USE_TIME
#include "services/berry/modules/newmota/cron.h"
#include "services/time/time_format.h"
#include "services/time/time_service.h"
#endif
#if USE_TIMERS
#include "services/timers/timer_service.h"
#endif
#if USE_BERRY_DRIVERS
#include "services/berry/modules/newmota/commands.h"
#include "services/berry/modules/newmota/drivers.h"
#if USE_SENSORS || USE_ENERGY || USE_SWITCHES
#include "core/message/message.h"
#include "services/telemetry/measurement_document.h"
#endif
#endif

extern "C" {
#include "be_gc.h"
#include "be_list.h"
#include "berry.h"
}

#include <array>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#if USE_BERRY_DRIVERS
extern "C" const bclass be_class_Driver;
#endif
#if USE_BERRY_SOLIDIFY
extern "C" void newmota_install_solidified_drivers(bvm* vm);
#endif

namespace {

constexpr std::size_t kCommandResponseCapacity = 4096;
constexpr std::string_view kChainPrefix = "{\"Results\":[";
constexpr std::string_view kChainSuffix = "]}";

newmota::CommandProcessor* command_processor = nullptr;

struct ResponseCapture final {
    char* data{}; ///< kCommandResponseCapacity bytes of a Berry scratch string, one call.
    std::size_t size{};
    bool overflow{};
};

constexpr std::size_t kCallbackLimit = newmota::kNewmotaCallbackCapacity;
constexpr std::size_t kTimerHandlerRootIndex = kCallbackLimit * 2;
#if USE_TIMERS
constexpr std::size_t kCallbackRootCount = kTimerHandlerRootIndex + 1;
#else
constexpr std::size_t kCallbackRootCount = kTimerHandlerRootIndex;
#endif
enum class CallbackKind { timer, deferred, cron };
std::uint64_t registration_sequence{};

struct PendingCallback {
    std::uint32_t deadline{};
    std::uint32_t generation{};
    bool active{};
    CallbackKind kind{};
    std::uint64_t order{};
#if USE_TIME
    newmota::CronPattern cron{};
    bool armed{};
    bool terminal{};
#endif
};

std::array<PendingCallback, kCallbackLimit> pending{};
blist* callback_roots{};
bvm* owner_vm{};
#if USE_TIME
newmota::TimeService* time_owner{};
#endif

// All 32-bit clock/epoch values use signed two's-complement Berry integers,
// including hosts where bint is wider than the firmware integer.
bint SignedBits(const std::uint32_t bits) {
    const std::int64_t value = bits <= INT32_MAX ? static_cast<std::int64_t>(bits)
                                                 : static_cast<std::int64_t>(bits) - 0x100000000LL;
    return static_cast<bint>(value);
}

std::uint32_t ReadBits(bvm* vm, int index) {
    if (!be_isint(vm, index) || be_toint(vm, index) < INT32_MIN || be_toint(vm, index) > INT32_MAX)
        be_raise(vm, "value_error", "expected a signed 32-bit integer bit pattern");
    return static_cast<std::uint32_t>(be_toint(vm, index));
}

std::uint32_t ReadDelay(bvm* vm, int index) {
    if (!be_isint(vm, index) || be_toint(vm, index) < 0 || be_toint(vm, index) > INT32_MAX)
        be_raise(vm, "value_error", "delay must be 0..2147483647 milliseconds");
    return static_cast<std::uint32_t>(be_toint(vm, index));
}

void YieldExecution(bvm* vm) {
    newmota::check_berry_execution_deadline(vm);
    newmota::platform::yield_execution();
    newmota::check_berry_execution_deadline(vm);
}

/** @brief Compiles and runs a filesystem script; a failure is logged and returns false. */
int Load(bvm* vm) {
    if (be_top(vm) != 1 || !be_isstring(vm, 1))
        be_raise(vm, "type_error", "load expects a file name");
    const char* const name = be_tostring(vm, 1);
    const std::string_view text{name};
    const bool has_extension = text.find('.') != std::string_view::npos;
    bool loaded = false;
    if (has_extension && (text.size() < 3 || text.substr(text.size() - 3) != ".be")) {
        newmota::AddLog(newmota::LogLevel::error, "BRY: file extension of '%s' is not '.be'", name);
    } else if (!text.empty()) {
        be_pushfstring(vm, "%s%s%s", name[0] == '/' ? "" : "/", name, has_extension ? "" : ".be");
        const char* const path = be_tostring(vm, -1);
        int result = be_loadfile(vm, path);
        if (result == BE_OK)
            result = be_pcall(vm, 0);
        loaded = result == BE_OK;
        if (result == BE_IO_ERROR) {
            newmota::AddLog(newmota::LogLevel::error, "BRY: cannot open '%s'", path);
        } else if (!loaded) {
            const char* const type = be_isstring(vm, -2) ? be_tostring(vm, -2) : "exception";
            const char* const message = be_isstring(vm, -1) ? be_tostring(vm, -1) : "failed";
            newmota::AddLog(newmota::LogLevel::error, "BRY: failed to load '%s' (%s - %s)", path,
                type, message);
        }
    }
    be_pushbool(vm, loaded);
    be_return(vm);
}

int Yield(bvm* vm) {
    if (be_top(vm) != 0)
        be_raise(vm, "type_error", "yield expects no arguments");
    YieldExecution(vm);
    be_return_nil(vm);
}

int Delay(bvm* vm) {
    if (be_top(vm) > 1 || (be_top(vm) == 1 && !be_isint(vm, 1)))
        be_raise(vm, "type_error", "delay expects an optional integer");
    const bint requested = be_top(vm) ? be_toint(vm, 1) : 0;
    if (requested < 0 || requested > 50)
        be_raise(vm, "value_error", "delay must be 0..50 milliseconds");
    if (requested == 0) {
        YieldExecution(vm);
    } else {
        const auto start = newmota::platform::monotonic_milliseconds();
        const auto duration = static_cast<std::uint32_t>(requested);
        auto elapsed =
            static_cast<std::uint32_t>(newmota::platform::monotonic_milliseconds() - start);
        while (elapsed < duration) {
            newmota::check_berry_execution_deadline(vm);
            newmota::platform::sleep_for_milliseconds(duration - elapsed);
            newmota::check_berry_execution_deadline(vm);
            elapsed =
                static_cast<std::uint32_t>(newmota::platform::monotonic_milliseconds() - start);
        }
    }
    be_return_nil(vm);
}

int Millis(bvm* vm) {
    if (be_top(vm) > 1)
        be_raise(vm, "type_error", "millis expects an optional delay");
    const auto delay = be_top(vm) ? ReadDelay(vm, 1) : 0U;
    be_pushint(vm, SignedBits(newmota::platform::monotonic_milliseconds() + delay));
    be_return(vm);
}

int TimeReached(bvm* vm) {
    if (be_top(vm) != 1)
        be_raise(vm, "type_error", "time_reached expects one timer");
    const auto timer = ReadBits(vm, 1);
    be_pushbool(vm, newmota::platform::monotonic_milliseconds() - timer < 0x80000000U);
    be_return(vm);
}

void ClearPending(std::size_t slot) {
    const auto generation = pending[slot].generation;
    pending[slot] = {};
    pending[slot].generation = generation;
    var_setnil(be_list_at(callback_roots, static_cast<int>(slot * 2)));
    var_setnil(be_list_at(callback_roots, static_cast<int>(slot * 2 + 1)));
}

int Schedule(bvm* vm, bool deferred, bool cron = false) {
    const int argc = be_top(vm);
    if ((deferred && argc != 1) || (!deferred && (argc < 2 || argc > 3)))
        be_raise(vm, "type_error", "invalid timer arguments");
    const auto delay = deferred || cron ? 0U : ReadDelay(vm, 1);
#if USE_TIME
    newmota::CronPattern parsed;
    if (cron) {
        if (!be_isstring(vm, 1))
            be_raise(vm, "type_error", "cron pattern must be a string");
        if (!newmota::parse_cron(
                {be_tostring(vm, 1), static_cast<std::size_t>(be_strlen(vm, 1))}, parsed))
            be_raise(vm, "value_error", "invalid cron pattern");
    }
#endif
    const int callback = deferred ? 1 : 2;
    if (!be_isfunction(vm, callback))
        be_raise(vm, "type_error", "callback must be a function or closure");
    if (!deferred && argc == 3 && !be_isnil(vm, 3) && !be_isstring(vm, 3))
        be_raise(vm, "type_error", "callback ID must be a string or nil");
    std::size_t slot = 0;
    while (slot < pending.size() && pending[slot].active)
        ++slot;
    if (slot == pending.size())
        be_raise(vm, "value_error", "callback queue is full");
    // Build the replacement on the VM stack so every allocation remains traced.
    auto* next = callback_roots ? be_list_copy(vm, callback_roots) : be_list_new(vm);
    be_pushnil(vm);
    var_setlist(be_indexof(vm, -1), next);
    be_list_resize(vm, next, static_cast<int>(kCallbackRootCount));
    var_setval(be_list_at(next, static_cast<int>(slot * 2)), be_indexof(vm, callback));
    auto* id = be_list_at(next, static_cast<int>(slot * 2 + 1));
    if (!deferred && argc == 3)
        var_setval(id, be_indexof(vm, 3));
    else
        var_setnil(id);
    be_gc_fix(vm, reinterpret_cast<bgcobject*>(next));
    if (callback_roots)
        be_gc_unfix(vm, reinterpret_cast<bgcobject*>(callback_roots));
    callback_roots = next;
    auto& entry = pending[slot];
    ++entry.generation;
    entry.deadline = newmota::platform::monotonic_milliseconds() + delay;
    entry.kind = cron       ? CallbackKind::cron
                 : deferred ? CallbackKind::deferred
                            : CallbackKind::timer;
    entry.order = ++registration_sequence;
#if USE_TIME
    entry.armed = false;
    entry.terminal = false;
    if (cron) {
        entry.cron = parsed;
        if (time_owner) {
            const auto current = time_owner->snapshot(newmota::platform::monotonic_milliseconds());
            if (current.valid) {
                entry.armed =
                    newmota::next_cron_after(entry.cron, current.local_seconds, entry.deadline);
                entry.terminal = !entry.armed;
            }
        }
    }
#endif
    entry.active = true;
    be_return_nil(vm);
}

int SetTimer(bvm* vm) {
    return Schedule(vm, false);
}

int Defer(bvm* vm) {
    return Schedule(vm, true);
}

int RemoveCallbacks(bvm* vm, CallbackKind kind) {
    if (be_top(vm) != 1 || !be_isstring(vm, 1))
        be_raise(vm, "type_error", "callback removal expects one string ID");
    if (callback_roots) {
        const auto size = be_strlen(vm, 1);
        const auto* text = be_tostring(vm, 1);
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (!pending[i].active || pending[i].kind != kind)
                continue;
            const auto* id = be_list_at(callback_roots, static_cast<int>(i * 2 + 1));
            be_pushnil(vm);
            var_setval(be_indexof(vm, -1), id);
            const bool match =
                be_isstring(vm, -1) && be_strlen(vm, -1) == size &&
                std::memcmp(be_tostring(vm, -1), text, static_cast<std::size_t>(size)) == 0;
            be_pop(vm, 1);
            if (match)
                ClearPending(i);
        }
    }
    be_return_nil(vm);
}

int RemoveTimer(bvm* vm) {
    return RemoveCallbacks(vm, CallbackKind::timer);
}

#if USE_TIMERS
/** @brief Registers the single Berry callback for native Timer Action 3 events. */
int AddTimerHandler(bvm* vm) {
    if (be_top(vm) != 1 || !be_isfunction(vm, 1))
        be_raise(vm, "type_error", "timer handler expects one function or closure");

    auto* next = callback_roots ? be_list_copy(vm, callback_roots) : be_list_new(vm);
    be_pushnil(vm);
    var_setlist(be_indexof(vm, -1), next);
    be_list_resize(vm, next, static_cast<int>(kCallbackRootCount));
    var_setval(be_list_at(next, static_cast<int>(kTimerHandlerRootIndex)), be_indexof(vm, 1));
    be_gc_fix(vm, reinterpret_cast<bgcobject*>(next));
    if (callback_roots)
        be_gc_unfix(vm, reinterpret_cast<bgcobject*>(callback_roots));
    callback_roots = next;
    be_return_nil(vm);
}

/** @brief Removes the Berry callback for native Timer Action 3 events. */
int RemoveTimerHandler(bvm* vm) {
    if (be_top(vm) != 0)
        be_raise(vm, "type_error", "remove_timer_handler expects no arguments");
    if (callback_roots)
        var_setnil(be_list_at(callback_roots, static_cast<int>(kTimerHandlerRootIndex)));
    be_return_nil(vm);
}
#endif
#if USE_TIME
int AddCron(bvm* vm) {
    return Schedule(vm, false, true);
}

int RemoveCron(bvm* vm) {
    return RemoveCallbacks(vm, CallbackKind::cron);
}

void InsertInteger(bvm* vm, int map, const char* key, bint value) {
    be_pushstring(vm, key);
    be_pushint(vm, value);
    (void)be_data_insert(vm, map);
    be_pop(vm, 2);
}

int Rtc(bvm* vm) {
    if (be_top(vm) > 1 || (be_top(vm) == 1 && !be_isstring(vm, 1)))
        be_raise(vm, "type_error", "rtc expects an optional string key");
    if (!time_owner)
        be_raise(vm, "runtime_error", "time service unavailable");
    const auto snapshot = time_owner->snapshot(newmota::platform::monotonic_milliseconds());
    const bool keyed = be_top(vm) == 1;
    const char* key = keyed ? be_tostring(vm, 1) : nullptr;
    if (keyed) {
        if (!std::strcmp(key, "utc"))
            be_pushint(vm, SignedBits(snapshot.utc_seconds));
        else if (!std::strcmp(key, "local"))
            be_pushint(vm, SignedBits(snapshot.local_seconds));
        else if (!std::strcmp(key, "timezone"))
            be_pushint(vm, snapshot.offset_minutes);
        else if (!std::strcmp(key, "valid"))
            be_pushbool(vm, snapshot.valid);
        else if (!std::strcmp(key, "source"))
            be_pushstring(vm, newmota::time_source_name(snapshot.source));
        else
            be_pushnil(vm);
    } else {
        be_newobject(vm, "map");
        const int map = be_absindex(vm, -1);
        InsertInteger(vm, map, "utc", SignedBits(snapshot.utc_seconds));
        InsertInteger(vm, map, "local", SignedBits(snapshot.local_seconds));
        InsertInteger(vm, map, "timezone", snapshot.offset_minutes);
        be_pushstring(vm, "valid");
        be_pushbool(vm, snapshot.valid);
        (void)be_data_insert(vm, map);
        be_pop(vm, 2);
        be_pushstring(vm, "source");
        be_pushstring(vm, newmota::time_source_name(snapshot.source));
        (void)be_data_insert(vm, map);
        be_pop(vm, 3); // key, value, and backing map; return the map instance
    }
    be_return(vm);
}

// Leaves the map instance and its backing map on the stack for optional extra fields.
void PushDateMap(bvm* vm, std::uint32_t epoch) {
    const auto civil = newmota::TimeService::civil_time(epoch);
    be_newobject(vm, "map");
    const int map = be_absindex(vm, -1);
    InsertInteger(vm, map, "year", civil.year);
    InsertInteger(vm, map, "month", civil.month);
    InsertInteger(vm, map, "day", civil.day);
    InsertInteger(vm, map, "hour", civil.hour);
    InsertInteger(vm, map, "min", civil.minute);
    InsertInteger(vm, map, "sec", civil.second);
    InsertInteger(vm, map, "weekday", civil.day_of_week - 1);
    InsertInteger(vm, map, "epoch", SignedBits(epoch));
}

int TimeDump(bvm* vm) {
    if (be_top(vm) != 1)
        be_raise(vm, "type_error", "time_dump expects one epoch");
    PushDateMap(vm, ReadBits(vm, 1));
    be_pop(vm, 1); // return the instance rather than its backing map
    be_return(vm);
}

int Strftime(bvm* vm) {
    if (be_top(vm) != 2 || !be_isstring(vm, 1) || !be_isint(vm, 2))
        be_raise(vm, "type_error", "strftime expects a format and integer epoch");
    char output[256];
    std::size_t size;
    const auto status =
        newmota::format_time({be_tostring(vm, 1), static_cast<std::size_t>(be_strlen(vm, 1))},
            ReadBits(vm, 2), output, sizeof(output), size);
    if (status != newmota::TimeFormatStatus::ok)
        be_raise(vm, "value_error", "invalid or excessive time format");
    be_pushnstring(vm, output, size);
    be_return(vm);
}

int Strptime(bvm* vm) {
    if (be_top(vm) != 2 || !be_isstring(vm, 1) || !be_isstring(vm, 2))
        be_raise(vm, "type_error", "strptime expects time and format strings");
    const std::string_view input{be_tostring(vm, 1), static_cast<std::size_t>(be_strlen(vm, 1))};
    std::uint32_t epoch;
    std::size_t consumed;
    const auto status = newmota::parse_time(
        input, {be_tostring(vm, 2), static_cast<std::size_t>(be_strlen(vm, 2))}, epoch, consumed);
    if (status == newmota::TimeFormatStatus::invalid_format ||
        status == newmota::TimeFormatStatus::overflow)
        be_raise(vm, "value_error", "invalid or excessive time format/input");
    if (status != newmota::TimeFormatStatus::ok)
        be_return_nil(vm);
    PushDateMap(vm, epoch);
    const int map = be_absindex(vm, -1);
    be_pushstring(vm, "unparsed");
    be_pushnstring(vm, input.data() + consumed, input.size() - consumed);
    (void)be_data_insert(vm, map);
    be_pop(vm, 3); // key, value, and backing map
    be_return(vm);
}

int NextCron(bvm* vm) {
    if (be_top(vm) != 1 || !be_isstring(vm, 1))
        be_raise(vm, "type_error", "next_cron expects one string ID");
    std::size_t found = pending.size();
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto& entry = pending[i];
        if (!entry.active || entry.kind != CallbackKind::cron)
            continue;
        be_pushnil(vm);
        var_setval(be_indexof(vm, -1), be_list_at(callback_roots, static_cast<int>(i * 2 + 1)));
        const bool match = be_isstring(vm, -1) && be_strlen(vm, -1) == be_strlen(vm, 1) &&
                           !std::memcmp(be_tostring(vm, -1), be_tostring(vm, 1),
                               static_cast<std::size_t>(be_strlen(vm, 1)));
        be_pop(vm, 1);
        if (match && (found == pending.size() || entry.order < pending[found].order))
            found = i;
    }
    if (found == pending.size())
        be_return_nil(vm);
    be_pushint(vm, pending[found].armed ? SignedBits(pending[found].deadline) : 0);
    be_return(vm);
}

int TimeString(bvm* vm) {
    if (be_top(vm) != 1)
        be_raise(vm, "type_error", "time_str expects one epoch");
    const auto civil = newmota::TimeService::civil_time(ReadBits(vm, 1));
    // Give snprintf room for the full declared integer ranges. Valid civil
    // fields still produce the fixed 19-character timestamp.
    char text[27];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u",
        static_cast<unsigned>(civil.year), static_cast<unsigned>(civil.month),
        static_cast<unsigned>(civil.day), static_cast<unsigned>(civil.hour),
        static_cast<unsigned>(civil.minute), static_cast<unsigned>(civil.second));
    be_pushstring(vm, text);
    be_return(vm);
}
#endif

/** @brief Collects one bounded JSON response from the shared command path. */
bool CaptureResponse(void* const context, const std::string_view text) {
    auto& capture = *static_cast<ResponseCapture*>(context);
    if (text.size() > kCommandResponseCapacity - capture.size) {
        capture.overflow = true;
        return false;
    }
    std::memcpy(capture.data + capture.size, text.data(), text.size());
    capture.size += text.size();
    return true;
}

/** @brief Wraps the command processor's line-framed chain in one JSON object. */
bool WrapChainResponse(
    char* const command_response, const std::size_t capacity, std::size_t& size) {
    while (
        size >= 2U && command_response[size - 2U] == '\r' && command_response[size - 1U] == '\n') {
        size -= 2U;
    }
    if (kChainPrefix.size() + size + kChainSuffix.size() > capacity) {
        return false;
    }

    std::memmove(command_response + kChainPrefix.size(), command_response, size);
    std::memcpy(command_response, kChainPrefix.data(), kChainPrefix.size());

    const std::size_t end = kChainPrefix.size() + size;
    std::size_t read = kChainPrefix.size();
    std::size_t write = kChainPrefix.size();
    while (read < end) {
        if (read + 1U < end && command_response[read] == '\r' &&
            command_response[read + 1U] == '\n') {
            command_response[write++] = ',';
            read += 2U;
        } else {
            command_response[write++] = command_response[read++];
        }
    }
    std::memcpy(command_response + write, kChainSuffix.data(), kChainSuffix.size());
    size = write + kChainSuffix.size();
    return true;
}

/** @brief Parses one captured JSON response and returns its Berry value. */
int LoadCommandResult(bvm* const vm, const int response) {
    const int original_top = be_top(vm);
    if (!be_getmodule(vm, "json") || !be_getmember(vm, -1, "load")) {
        const int top = be_top(vm);
        if (top > original_top) {
            be_pop(vm, top - original_top);
        }
        be_raise(vm, "runtime_error", "Berry JSON module is unavailable");
    }

    // Keep only json.load on top of the native function's original arguments.
    be_remove(vm, -2);
    be_pushvalue(vm, response);
    const int result = be_pcall(vm, 1);
    if (result == BE_OK) {
        // Berry leaves arguments above the return slot after a native API call.
        be_pop(vm, 1);
    }
    if (result != BE_OK || !be_ismapinstance(vm, -1)) {
        const int top = be_top(vm);
        if (top > original_top) {
            be_pop(vm, top - original_top);
        }
        be_raise(vm, "runtime_error", "Command response is not a JSON object");
    }
    be_return(vm);
}

/** @brief Executes a command through Newmota's shared command processor. */
int NewmotaCommand(bvm* const vm) {
    if (be_top(vm) != 1 || !be_isstring(vm, 1)) {
        be_raise(vm, "type_error", "newmota.cmd expects one string");
    }
    if (command_processor == nullptr) {
        be_raise(vm, "runtime_error", "Newmota command processor is unavailable");
    }

    const int command_length = be_strlen(vm, 1);
    const std::string_view command{be_tostring(vm, 1), static_cast<std::size_t>(command_length)};
    std::size_t command_count = 0;
    const bool valid_line = newmota::validate_command_line(command, &command_count);

    // A Berry-owned scratch string: whatever raises, a command or an allocation, leaves it to
    // the collector.
    ResponseCapture capture{};
    capture.data = static_cast<char*>(be_pushbuffer(vm, kCommandResponseCapacity));
    newmota::CommandOutput output{CaptureResponse, &capture};
    const auto result = command_processor->execute(command, newmota::CommandSource::berry, output);
    const bool fits = !capture.overflow && !result.output_failed && !output.failed() &&
                      (!valid_line || command_count <= 1U ||
                          WrapChainResponse(capture.data, kCommandResponseCapacity, capture.size));
    if (!fits) {
        be_raise(vm, "memory_error", "Command response exceeds Berry buffer");
    }
    be_pushnstring(vm, capture.data, capture.size);
    return LoadCommandResult(vm, be_top(vm));
}

/** @brief Writes one Tasmota-compatible Berry log line to normal log sinks. */
int BerryLog(bvm* const vm) {
    if ((be_top(vm) != 1 && be_top(vm) != 2) || !be_isstring(vm, 1)) {
        be_raise(vm, "type_error", "log expects a string and optional level");
    }
    bint requested = static_cast<bint>(newmota::LogLevel::info);
    if (be_top(vm) == 2 && !be_isnil(vm, 2)) {
        if (!be_isint(vm, 2)) {
            be_raise(vm, "type_error", "log level must be an integer");
        }
        requested = be_toint(vm, 2);
    }
    if (requested < 0 || requested > 4) {
        be_raise(vm, "value_error", "log level must be between 0 and 4");
    }

    const int length = be_strlen(vm, 1);
    const char* const message = be_tostring(vm, 1);
    newmota::AddLog(static_cast<newmota::LogLevel>(requested), "%.*s", length, message);
    be_return_nil(vm);
}

/** @brief Collects Berry objects and reports the remaining allocation size. */
int NewmotaGarbageCollect(bvm* const vm) {
    if (be_top(vm) != 0) {
        be_raise(vm, "type_error", "newmota.gc expects no arguments");
    }
    be_gc_collect(vm);
    const std::size_t allocated = be_gc_memcount(vm);
    if (allocated <= static_cast<std::size_t>(std::numeric_limits<bint>::max())) {
        be_pushint(vm, static_cast<bint>(allocated));
    } else {
        be_pushreal(vm, static_cast<breal>(allocated));
    }
    be_return(vm);
}

/** @brief Returns the connected station IP and Tasmota-style signal quality. */
int WifiStatus(bvm* const vm) {
    if (be_top(vm) != 0) {
        be_raise(vm, "type_error", "newmota.wifi expects no arguments");
    }

    std::uint8_t quality = 0U;
    bool has_ip = false;
    newmota::platform::NetworkInfo info{};
#if USE_WIFI
    const bool connected =
        newmota::platform::wifi_state() == newmota::platform::WifiState::connected;
    const auto rssi = connected ? newmota::platform::station_rssi() : std::nullopt;
    if (rssi.has_value()) {
        const int strength = *rssi;
        quality = static_cast<std::uint8_t>(strength <= -100  ? 0
                                            : strength >= -50 ? 100
                                                              : (strength + 100) * 2);
    }

    info = connected ? newmota::platform::network_info() : newmota::platform::NetworkInfo{};
    has_ip = connected && info.address[0] != '\0' && std::strcmp(info.address, "0.0.0.0") != 0;
#endif

    be_newobject(vm, "map");
    const int map = be_absindex(vm, -1);
    be_pushstring(vm, "quality");
    be_pushint(vm, quality);
    (void)be_data_insert(vm, map);
    be_pop(vm, 2);
    if (has_ip) {
        be_pushstring(vm, "ip");
        be_pushstring(vm, info.address);
        (void)be_data_insert(vm, map);
        be_pop(vm, 2);
    }
    be_pop(vm, 1); // Return the map instance, not its backing map.
    be_return(vm);
}

#if USE_BERRY_DRIVERS
/** @brief Tasmota's read_sensors(): the MQTT SENSOR document as a string, nil when empty.
 * The optional display argument is accepted and ignored; nil also when it exceeds the
 * 1024-byte MQTT payload, which is then not published either.
 */
int ReadSensors(bvm* const vm) {
    if (be_top(vm) > 1) {
        be_raise(vm, "type_error", "newmota.read_sensors expects an optional display flag");
    }
#if USE_SENSORS || USE_ENERGY || USE_SWITCHES
    // A Berry-owned scratch string: an allocation that raises leaves it to the collector.
    constexpr std::size_t capacity = newmota::CanonicalMessage::kPayloadCapacity + 1U;
    auto* const document = static_cast<char*>(be_pushbuffer(vm, capacity));
    std::size_t size = 0;
    if (newmota::telemetry::serialize_measurements(document, capacity, size) &&
        size > 2U) { // "{}" means no sensor wrote anything
        be_pushnstring(vm, document, size);
        be_return(vm);
    }
#endif
    be_return_nil(vm);
}
#endif

} // namespace

extern "C" {

be_native_module_attr_table(newmota){
    be_native_module_function("cmd", NewmotaCommand),
    be_native_module_function("gc", NewmotaGarbageCollect),
    be_native_module_function("load", Load),
    be_native_module_function("millis", Millis),
    be_native_module_function("yield", Yield),
    be_native_module_function("delay", Delay),
    be_native_module_function("time_reached", TimeReached),
    be_native_module_function("set_timer", SetTimer),
    be_native_module_function("remove_timer", RemoveTimer),
    be_native_module_function("defer", Defer),
#if USE_TIME
    be_native_module_function("add_cron", AddCron),
    be_native_module_function("remove_cron", RemoveCron),
    be_native_module_function("next_cron", NextCron),
    be_native_module_function("strftime", Strftime),
    be_native_module_function("strptime", Strptime),
    be_native_module_function("rtc", Rtc),
    be_native_module_function("time_dump", TimeDump),
    be_native_module_function("time_str", TimeString),
#endif
    be_native_module_function("wifi", WifiStatus),
#if USE_TIMERS
    be_native_module_function("add_timer_handler", AddTimerHandler),
    be_native_module_function("remove_timer_handler", RemoveTimerHandler),
#endif
#if USE_BERRY_DRIVERS
    be_native_module_function("add_driver", newmota::add_berry_driver),
    be_native_module_function("remove_driver", newmota::remove_berry_driver),
    be_native_module_function("add_cmd", newmota::add_berry_command),
    be_native_module_function("remove_cmd", newmota::remove_berry_command),
    be_native_module_function("resp_cmnd", newmota::berry_resp_cmnd),
    be_native_module_function("resp_cmnd_str", newmota::berry_resp_cmnd_str),
    be_native_module_function("resp_cmnd_done", newmota::berry_resp_cmnd_done),
    be_native_module_function("resp_cmnd_error", newmota::berry_resp_cmnd_error),
    be_native_module_function("resp_cmnd_failed", newmota::berry_resp_cmnd_failed),
    be_native_module_function("read_sensors", ReadSensors),
#endif
};

extern const bntvmodule_t be_native_module_newmota = {
    "newmota", newmota_attrs, sizeof(newmota_attrs) / sizeof(newmota_attrs[0]), nullptr};

} // extern "C"

namespace newmota {

/** @brief Registers Newmota's native module as a Berry global. */
bool install_newmota_module(bvm* const vm, CommandProcessor& commands, TimeService* time) {
    owner_vm = vm;
#if USE_TIME
    time_owner = time;
#else
    (void)time;
#endif
    command_processor = &commands;
    if (!be_getmodule(vm, "newmota")) {
        remove_newmota_module();
        return false;
    }
    be_setglobal(vm, "newmota");
    be_pop(vm, 1);
    be_pushntvfunction(vm, BerryLog);
    be_setglobal(vm, "log");
    be_pop(vm, 1);
    be_pushntvfunction(vm, Load);
    be_setglobal(vm, "load");
    be_pop(vm, 1);
#if USE_BERRY_DRIVERS
    be_pushntvclass(vm, &be_class_Driver);
    be_setglobal(vm, "Driver");
    be_pop(vm, 1);
#endif
#if USE_BERRY_SOLIDIFY
    newmota_install_solidified_drivers(vm);
#endif
    return true;
}

std::size_t snapshot_newmota_callbacks(
    std::uint32_t now, bool timer_tick, NewmotaPendingCallback* due) {
    std::size_t count = 0;
#if USE_TIME
    const auto clock = time_owner ? time_owner->snapshot(now) : TimeSnapshot{};
#endif
    for (std::size_t i = 0; i < pending.size(); ++i) {
        auto& entry = pending[i];
        if (!entry.active)
            continue;
        bool ready =
            entry.kind == CallbackKind::deferred ||
            (entry.kind == CallbackKind::timer && timer_tick && now - entry.deadline < 0x80000000U);
#if USE_TIME
        if (entry.kind == CallbackKind::cron && timer_tick && clock.valid) {
            if (!entry.armed && !entry.terminal) {
                entry.armed = next_cron_after(entry.cron, clock.local_seconds, entry.deadline);
                entry.terminal = !entry.armed;
            }
            ready = entry.armed && clock.local_seconds >= entry.deadline;
        }
#endif
        if (ready)
            due[count++] = {i, entry.generation};
    }
    return count;
}

int dispatch_newmota_callback(bvm* vm) {
    const auto* due = static_cast<const NewmotaPendingCallback*>(be_tocomptr(vm, 1));
    const auto slot = due->slot;
    if (pending[slot].active && pending[slot].generation == due->generation) {
        be_pushnil(vm);
        var_setval(be_indexof(vm, -1), be_list_at(callback_roots, static_cast<int>(slot * 2)));
#if USE_TIME
        if (pending[slot].kind == CallbackKind::cron) {
            const auto clock = time_owner ? time_owner->snapshot(platform::monotonic_milliseconds())
                                          : TimeSnapshot{};
            auto& entry = pending[slot];
            if (!clock.valid || !entry.armed || clock.local_seconds < entry.deadline) {
                be_pop(vm, 1);
                be_return_nil(vm);
            }
            entry.armed = next_cron_after(entry.cron, clock.local_seconds, entry.deadline);
            entry.terminal = !entry.armed;
            be_pushint(vm, SignedBits(clock.local_seconds));
            be_pushint(vm, entry.armed ? SignedBits(entry.deadline) : 0);
            be_call(vm, 2);
        } else
#endif
        {
            ClearPending(slot); // the callback stays rooted on the VM stack during execution
            be_call(vm, 0);
        }
    }
    be_return_nil(vm);
}

#if USE_TIMERS
/** @brief Delivers one Timer Action 3 event to the optional Berry handler. */
int dispatch_newmota_timer_event(bvm* vm) {
    const auto* event = static_cast<const newmota::TimerEvent*>(be_tocomptr(vm, 1));
    if (event == nullptr || callback_roots == nullptr)
        be_return_nil(vm);

    be_pushnil(vm);
    var_setval(
        be_indexof(vm, -1), be_list_at(callback_roots, static_cast<int>(kTimerHandlerRootIndex)));
    if (!be_isfunction(vm, -1)) {
        be_pop(vm, 1);
        be_return_nil(vm);
    }

    be_pushint(vm, event->timer);
    be_pushint(vm, event->output);
    be_pushint(vm, event->action);
    be_pushint(vm, SignedBits(event->scheduled));
    be_call(vm, 4);
    be_return_nil(vm);
}
#else
int dispatch_newmota_timer_event(bvm* vm) {
    (void)vm;
    return 0;
}
#endif

/** @brief Clears the non-owning command processor binding. */
void remove_newmota_module() {
#if USE_BERRY_DRIVERS
    if (owner_vm) {
        remove_berry_drivers(owner_vm);
        remove_berry_commands(owner_vm);
    }
#endif
    command_processor = nullptr;
    if (callback_roots)
        be_gc_unfix(owner_vm, reinterpret_cast<bgcobject*>(callback_roots));
    callback_roots = nullptr;
    owner_vm = nullptr;
    pending = {};
    registration_sequence = 0;
#if USE_TIME
    time_owner = nullptr;
#endif
}

} // namespace newmota
