/*
  berry_service.cpp - Berry runtime service for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file berry_service.cpp
 * @brief Owns the persistent VM, native module registration and bounded script execution.
 */
#include "services/berry/berry_service.h"

#include "newmota_features.h"
#include "support/text.h"
#include "services/berry/berry_port.h"
#include "services/berry/newmota_module.h"
#if USE_BERRY_GPIO
#include "services/berry/modules/gpio/gpio_module.h"
#endif
#if USE_DISPLAY_FONTS
#include "services/berry/modules/font/font_module.h"
#endif
#if USE_BERRY_PIXMAT
#include "services/berry/modules/pixmat/pixmat_module.h"
#endif
#if USE_BERRY_IMAGE
#include "services/berry/modules/img/img_module.h"
#endif
#if USE_MATTER
#include "services/matter/berry/matter_module.h"
#endif
#if USE_DISPLAY
#include "services/berry/modules/display/display_module.h"
#endif
#if defined(USE_LVGL) && USE_LVGL
#include "services/berry/modules/lvgl/lvgl_runtime.h"
#endif
#if USE_TIMERS
#include "services/timers/timer_service.h"
#endif
#if defined(USE_BERRY_PERSIST) && USE_BERRY_PERSIST
#include "services/berry/modules/persist/persist_module.h"
#endif
#if USE_WS2812
#include "services/berry/modules/led_strip/led_strip_module.h"
#endif
#if USE_SERIAL
#include "services/berry/modules/serial/serial_module.h"
#endif
#if USE_BERRY_ENERGY_API
#include "services/berry/modules/energy/energy_module.h"
#endif
#if USE_I2C
#include "services/berry/modules/wire/wire_module.h"
#endif
#if USE_BERRY_AUDIO
#include "services/berry/modules/audio/audio_module.h"
#endif
#if USE_CAMERA
#include "services/berry/modules/cam/cam_module.h"
#endif
#if USE_BLE
#include "services/berry/modules/ble/ble_module.h"
#endif
#if USE_BERRY_DRIVERS
#include "services/berry/modules/newmota/commands.h"
#include "services/berry/modules/newmota/drivers.h"
#endif
#if USE_MQTT
#include "services/mqtt/berry/mqtt_module.h"
#endif
#if USE_PWM || USE_WS2812
#include "services/light/berry/light_module.h"
#endif

#include "newmota/log.h"
#include "newmota/platform/platform.h"
#if USE_WEB_CONSOLE
#include "newmota/console_history.h"
#endif
#if USE_WEB
#include "services/web/berry/widget_module.h"
#endif

extern "C" {
#include "be_vm.h"
#include "berry.h"
}

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace newmota {
namespace {

constexpr std::uint32_t kExecutionTimeoutMilliseconds = 4000;
constexpr std::uint32_t kHalfTimerRange = 1U << 31;
constexpr std::size_t kExpressionCapacity = 256;
constexpr std::size_t kBerryConsoleSourceCapacity = NEWMOTA_WEB_REQUEST_BODY_SIZE;
constexpr std::string_view kBerryExpressionPrefix{"return ("};
static_assert(kBerryConsoleBufferOverhead >= kBerryExpressionPrefix.size() + 1U);
std::uint32_t execution_deadline = 0;
bool execution_active = false;

/** @brief Tests a wrapping millisecond deadline. */
[[nodiscard]] bool DeadlineReached(const std::uint32_t now, const std::uint32_t deadline) {
    return now - deadline < kHalfTimerRange;
}

/** @brief Interrupts Berry bytecode after the bounded command deadline. */
void ObserveBerry(bvm* vm, const int event, ...) {
    if (event == BE_OBS_VM_HEARTBEAT)
        check_berry_execution_deadline(vm);
}

/** @brief Removes every value left on the Berry API stack. */
void ClearStack(bvm* vm) {
    const int top = be_top(vm);
    if (top > 0) {
        be_pop(vm, top);
    }
}

/** @brief Executes a compiled closure and converts its result inside one pcall.
 *
 * Conversion may invoke user-defined tostring bytecode or raise an allocation
 * error. Keep it under the same exception handler and deadline as execution.
 */
int ExecuteAndStringify(bvm* vm) {
    be_call(vm, 0);
    (void)be_tostring(vm, 1);
    be_pushvalue(vm, 1);
    be_return(vm);
}

/** @brief Executes REPL source and records its non-nil result only in Berry. */
int ExecuteAndCaptureReplResult(bvm* vm) {
    be_call(vm, 0);
    if (!be_isnil(vm, 1)) {
        const char* const value = be_tostring(vm, 1);
        const int length = be_strlen(vm, 1);
#if USE_WEB_CONSOLE
        console_history::record_berry_output({value, static_cast<std::size_t>(length)});
#else
        (void)value;
        (void)length;
#endif
    }
    be_return_nil(vm);
}

/** @brief Adds a short Berry console message to its output history. */
void BerryConsoleMessage(const LogLevel level, const char* const message) {
    const char* const text = message == nullptr ? "Berry execution failed" : message;
#if USE_WEB_CONSOLE
    console_history::record_berry_output(text);
#endif
    AddLog(level, "BRY: %s", text);
}

/** @brief Compiles expression-first source and releases upload bytes before execution. */
BerryReplResult RunReplSource(bvm* const vm, const std::string_view expression,
    const std::string_view source, char** const owned_source) {
    if (execution_active) {
        BerryConsoleMessage(LogLevel::error, "Berry VM is already executing");
        return BerryReplResult::failed;
    }
    if (vm == nullptr) {
        return BerryReplResult::unavailable;
    }

    ClearStack(vm);
    be_pushntvfunction(vm, ExecuteAndCaptureReplResult);
    int result = be_loadbuffer(vm, "console", expression.data(), expression.size());
    if (be_getexcept(vm, result) == BE_SYNTAX_ERROR) {
        ClearStack(vm);
        be_pushntvfunction(vm, ExecuteAndCaptureReplResult);
        result = be_loadbuffer(vm, "console", source.data(), source.size());
    }
    if (result == BE_OK) {
        // Compiled Berry bytecode no longer refers to the source allocation.
        if (owned_source != nullptr && *owned_source != nullptr) {
            delete[] *owned_source;
            *owned_source = nullptr;
        }
        execution_deadline = platform::monotonic_milliseconds() + kExecutionTimeoutMilliseconds;
        execution_active = true;
        result = be_pcall(vm, 1);
    }

    if (result == BE_OK) {
        execution_active = false;
        ClearStack(vm);
        return BerryReplResult::executed;
    }
    if (result != BE_MALLOC_FAIL) {
        begin_berry_exception_report();
        be_dumpexcept(vm);
        end_berry_exception_report();
    } else {
        BerryConsoleMessage(LogLevel::error, "Berry memory allocation failed");
    }
    execution_active = false;
    ClearStack(vm);
    return BerryReplResult::failed;
}

/** @brief Writes a successful or failed Br value as escaped JSON. */
[[nodiscard]] bool WriteResponse(CommandOutput& output, const char* const value) {
    return output.write("{\"Br\":") && output.json_string(value == nullptr ? "" : value) &&
           output.write("}");
}

/** @brief Logs a Berry compile or runtime exception from the VM stack. */
void LogScriptError(bvm* const vm, const char* const path) {
    const int top = be_top(vm);
    const char* const exception =
        top >= 2 && be_isstring(vm, -2) ? be_tostring(vm, -2) : "exception";
    const char* const message =
        top >= 1 && be_isstring(vm, -1) ? be_tostring(vm, -1) : "Berry execution failed";
    AddLog(LogLevel::error, "BRY: %s failed [%s] %s", path, exception, message);
}

} // namespace

bool berry_execution_deadline_reached() {
    return execution_active &&
           DeadlineReached(platform::monotonic_milliseconds(), execution_deadline);
}

void check_berry_execution_deadline(bvm* const vm) {
    if (berry_execution_deadline_reached())
        be_raise(vm, "timeout_error", "Berry code running for too long");
}

/** @brief Releases the persistent Berry VM. */
BerryService::~BerryService() {
    if (vm_ != nullptr) {
#if defined(USE_LVGL) && USE_LVGL
        remove_lvgl_runtime(vm_);
#endif
#if USE_SERIAL
        remove_serial_module();
#endif
#if USE_WS2812
        remove_led_strip_module();
#endif
#if USE_BERRY_GPIO
        remove_gpio_module();
#endif
#if USE_I2C
        remove_wire_module();
#endif
#if USE_BERRY_AUDIO
        remove_audio_module();
#endif
#if USE_CAMERA
        remove_cam_module();
#endif
#if USE_PWM || USE_WS2812
        remove_light_module();
#endif
#if USE_BLE
        remove_ble_module();
#endif
#if USE_MQTT
        remove_mqtt_module();
#endif
#if USE_WEB
        remove_widget_module(); // widget content belongs to the scripts of this VM
#endif
        remove_newmota_module();
        be_vm_delete(vm_);
    }
}

/** @brief Creates and configures the persistent Berry VM. */
bool BerryService::initialize(CommandProcessor& commands, LightService* const light,
    BerryMqttBridge* const mqtt, TimeService* const time, const Hardware* const hardware,
    TimerService* const timers) {
    if (vm_ != nullptr) {
        return true;
    }
    vm_ = be_vm_new();
    if (vm_ == nullptr) {
        AddLog(LogLevel::error, "BRY: VM allocation failed");
        return false;
    }
    comp_set_named_gbl(vm_);
    comp_set_strict(vm_);
    be_set_obs_hook(vm_, ObserveBerry);
    bool modules_ready = install_newmota_module(vm_, commands, time);
#if USE_BERRY_PIXMAT
    if (modules_ready)
        modules_ready = install_pixmat_module(vm_);
#endif
#if USE_BERRY_IMAGE
    if (modules_ready)
        modules_ready = install_img_module(vm_);
#endif
#if USE_BERRY_ENERGY_API
    if (modules_ready)
        modules_ready = install_energy_module(vm_);
#endif
#if USE_DISPLAY_FONTS
    if (modules_ready)
        modules_ready = install_font_module(vm_);
#endif
#if USE_DISPLAY
    if (modules_ready)
        modules_ready = install_display_module(vm_);
#endif
#if USE_BERRY_GPIO
    install_gpio_module(hardware);
#endif
#if USE_SERIAL
    if (modules_ready)
        modules_ready = install_serial_module(vm_);
#endif
#if USE_WS2812
    if (modules_ready)
        modules_ready = install_led_strip_module(vm_);
#endif
#if USE_I2C
    if (modules_ready)
        modules_ready = install_wire_module(vm_);
#endif
#if USE_BERRY_AUDIO
    if (modules_ready)
        modules_ready = install_audio_module(vm_);
#endif
#if USE_BLE
    install_ble_module(vm_);
#endif
#if USE_PWM || USE_WS2812
    if (modules_ready && light != nullptr)
        modules_ready = install_light_module(vm_, *light);
#else
    (void)light;
#endif
#if USE_MQTT
    if (modules_ready && mqtt)
        install_mqtt_module(vm_, *mqtt);
#else
    (void)mqtt;
#endif
#if defined(USE_LVGL) && USE_LVGL
    if (modules_ready)
        modules_ready = install_lvgl_runtime(vm_);
#endif
    if (!modules_ready) {
#if USE_SERIAL
        remove_serial_module();
#endif
#if USE_WS2812
        remove_led_strip_module();
#endif
#if USE_BERRY_GPIO
        remove_gpio_module();
#endif
        AddLog(LogLevel::error, "BRY: Newmota module registration failed");
#if USE_I2C
        remove_wire_module();
#endif
#if USE_BERRY_AUDIO
        remove_audio_module();
#endif
#if USE_CAMERA
        remove_cam_module();
#endif
#if USE_PWM || USE_WS2812
        remove_light_module();
#endif
#if USE_BLE
        remove_ble_module();
#endif
#if USE_MQTT
        remove_mqtt_module();
#endif
        remove_newmota_module();
        be_vm_delete(vm_);
        vm_ = nullptr;
        return false;
    }
    timers_ = timers;
    AddLog(LogLevel::info, "BRY: Berry %s initialized", BERRY_VERSION);
    return true;
}

bool BerryService::memory(std::uint32_t& heap_used_kb, std::uint32_t& objects) const {
    if (vm_ == nullptr)
        return false;
    heap_used_kb = static_cast<std::uint32_t>(vm_->gc.usage / 1024U);
    objects = vm_->counter_gc_kept;
    return true;
}

void BerryService::poll(const std::uint32_t now, const bool timer_tick) {
    if (!vm_ || execution_active)
        return;
    std::array<NewmotaPendingCallback, kNewmotaCallbackCapacity> due{};
    const auto count = snapshot_newmota_callbacks(now, timer_tick, due.data());
#if USE_BLE
    const bool ble_pending = ble_module_pending();
#else
    constexpr bool ble_pending = false;
#endif
#if USE_WEB
    const bool widget_pending = widget_module_pending();
#else
    constexpr bool widget_pending = false;
#endif
#if USE_MATTER
    const bool matter_pending = matter_module_pending(timer_tick);
#else
    constexpr bool matter_pending = false;
#endif
#if USE_BERRY_DRIVERS
    const bool drivers_pending = berry_drivers_pending();
#else
    constexpr bool drivers_pending = false;
#endif
#if (!defined(USE_LVGL) || !USE_LVGL) && USE_TIMERS
    if (count == 0U && timers_ == nullptr && !ble_pending && !widget_pending && !matter_pending &&
        !drivers_pending)
        return;
#elif !defined(USE_LVGL) || !USE_LVGL
    if (!count && !ble_pending && !widget_pending && !matter_pending && !drivers_pending)
        return;
#endif
    ClearStack(vm_);
    execution_deadline = platform::monotonic_milliseconds() + kExecutionTimeoutMilliseconds;
    execution_active = true;
#if defined(USE_LVGL) && USE_LVGL
    be_pushntvfunction(vm_, poll_lvgl_runtime);
    if (be_pcall(vm_, 0) != BE_OK)
        LogScriptError(vm_, "LVGL callback");
    ClearStack(vm_);
#endif
#if USE_BLE
    if (ble_pending) {
        be_pushntvfunction(vm_, dispatch_ble_events);
        if (be_pcall(vm_, 0) != BE_OK)
            LogScriptError(vm_, "BLE callback");
        ClearStack(vm_);
    }
#endif
#if USE_MATTER
    if (matter_pending) {
        be_pushntvfunction(vm_, dispatch_matter_events);
        if (be_pcall(vm_, 0) != BE_OK)
            LogScriptError(vm_, "Matter");
        ClearStack(vm_);
    }
#endif
#if USE_WEB
    // One action per protected call: a failing callback is logged and the rest still run.
    while (widget_pending && widget_module_pending() &&
           !DeadlineReached(platform::monotonic_milliseconds(), execution_deadline)) {
        be_pushntvfunction(vm_, dispatch_widget_action);
        if (be_pcall(vm_, 0) != BE_OK)
            LogScriptError(vm_, "widget callback");
        ClearStack(vm_);
    }
#endif
#if USE_BERRY_DRIVERS
    // One protected call per driver and hook, so a failing driver does not stop the others.
    // Events a hook causes, such as a Power change, are delivered in a later poll.
    begin_berry_driver_batch();
    BerryDriverEvent driver_event{};
    while (drivers_pending &&
           !DeadlineReached(platform::monotonic_milliseconds(), execution_deadline) &&
           next_berry_driver_event(driver_event)) {
        // The count is read each pass, as Tasmota's `while i < size(_drivers)` does.
        for (std::size_t slot = 0; slot < berry_driver_count(); ++slot) {
            if (DeadlineReached(platform::monotonic_milliseconds(), execution_deadline))
                break; // the remaining drivers miss this event; later events wait
            be_pushntvfunction(vm_, dispatch_berry_driver);
            be_pushcomptr(vm_, &driver_event);
            be_pushint(vm_, static_cast<bint>(slot));
            const bool failed = be_pcall(vm_, 2) != BE_OK;
            if (failed)
                LogScriptError(vm_, "driver");
            ClearStack(vm_);
            if (!failed && berry_driver_done())
                break; // as in Tasmota, true ends this event's dispatch
        }
    }
    // After the hooks ran, so a tick method a hook assigned is found at once.
    if (berry_driver_refresh_pending() &&
        !DeadlineReached(platform::monotonic_milliseconds(), execution_deadline)) {
        be_pushntvfunction(vm_, refresh_berry_driver_hooks);
        if (be_pcall(vm_, 0) != BE_OK)
            LogScriptError(vm_, "driver hooks");
        ClearStack(vm_);
    }
#endif
    for (std::size_t i = 0; i < count; ++i) {
        if (DeadlineReached(platform::monotonic_milliseconds(), execution_deadline))
            break;
        be_pushntvfunction(vm_, dispatch_newmota_callback);
        be_pushcomptr(vm_, &due[i]);
        const int result = be_pcall(vm_, 1);
        if (result != BE_OK)
            LogScriptError(vm_, "timer callback");
        ClearStack(vm_);
    }
#if USE_TIMERS
    if (timers_ != nullptr) {
        for (;;) {
            if (DeadlineReached(platform::monotonic_milliseconds(), execution_deadline)) {
                break;
            }
            TimerEvent event{};
            if (!timers_->pop_event(event)) {
                break;
            }
            be_pushntvfunction(vm_, dispatch_newmota_timer_event);
            be_pushcomptr(vm_, &event);
            const int result = be_pcall(vm_, 1);
            if (result != BE_OK)
                LogScriptError(vm_, "timer event");
            ClearStack(vm_);
        }
    }
#endif
    execution_active = false;
}

#if USE_MQTT
bool BerryService::mqtt_publish(const MqttPublishPacket& packet) {
    if (!vm_)
        return false;
    if (execution_active)
        return true;
    ClearStack(vm_);
    be_pushntvfunction(vm_, dispatch_mqtt_callbacks);
    be_pushcomptr(vm_, const_cast<MqttPublishPacket*>(&packet));
    execution_deadline = platform::monotonic_milliseconds() + kExecutionTimeoutMilliseconds;
    execution_active = true;
    const int result = be_pcall(vm_, 1);
    execution_active = false;
    const bool handled = result != BE_OK || be_tobool(vm_, 1);
    if (result != BE_OK)
        LogScriptError(vm_, "MQTT callback");
    ClearStack(vm_);
    return handled;
}
#endif

#if USE_MATTER
CommandStatus BerryService::matter_command(const CommandRequest& request, CommandOutput& output) {
    const char* name = nullptr;
    for (const char* const candidate : {"MtrInfo", "MtrJoin", "MtrReset"}) {
        if (support::ascii_iequals(request.name, candidate))
            name = candidate;
    }
    if (name == nullptr)
        return CommandStatus::unknown;
    if (!vm_)
        return CommandStatus::failed;
    const int top = be_top(vm_);
    const auto restore = [this, top]() { be_pop(vm_, be_top(vm_) - top); };
    if (!be_getglobal(vm_, "_matter") || be_isnil(vm_, -1)) {
        restore();
        return output.format("{\"%s\":\"Off\"}", name) ? CommandStatus::handled
                                                       : CommandStatus::failed;
    }
    const int device = be_absindex(vm_, -1);
    be_getmember(vm_, device, "command");
    be_pushvalue(vm_, device);
    be_pushstring(vm_, name);
    be_pushnstring(vm_, request.payload.data(), request.payload.size());
    // A command may arrive from a running script, whose deadline then still applies.
    const bool outer_active = execution_active;
    if (!outer_active) {
        execution_deadline = platform::monotonic_milliseconds() + kExecutionTimeoutMilliseconds;
        execution_active = true;
    }
    const int result = be_pcall(vm_, 3);
    execution_active = outer_active;
    if (result != BE_OK) {
        LogScriptError(vm_, "Matter command");
        restore();
        return CommandStatus::failed;
    }
    const bool answered = be_isstring(vm_, -4);
    const bool written = answered && output.write(be_tostring(vm_, -4));
    restore();
    if (!answered)
        return CommandStatus::invalid;
    return written ? CommandStatus::handled : CommandStatus::failed;
}
#endif

#if USE_CAMERA
CommandStatus BerryService::camera_command(const CommandRequest& request, CommandOutput& output) {
    if (!camera_command_name(request.name))
        return CommandStatus::unknown;
    if (!vm_)
        return CommandStatus::failed;
    const int top = be_top(vm_);
    CameraCommand command{&request, &output};
    be_pushntvfunction(vm_, dispatch_camera_command);
    be_pushcomptr(vm_, &command);
    // A command may arrive from a running script, whose deadline then still applies.
    const bool outer_active = execution_active;
    if (!outer_active) {
        execution_deadline = platform::monotonic_milliseconds() + kExecutionTimeoutMilliseconds;
        execution_active = true;
    }
    const int result = be_pcall(vm_, 1);
    execution_active = outer_active;
    if (result != BE_OK) {
        LogScriptError(vm_, "camera command");
        command.status = CommandStatus::failed;
    }
    be_pop(vm_, be_top(vm_) - top);
    return command.status;
}
#endif

#if USE_BERRY_DRIVERS
CommandStatus BerryService::script_command(const CommandRequest& request, CommandOutput& output) {
    if (!vm_)
        return CommandStatus::unknown;
    BerryCommandCall call{&request, find_berry_command(request.name)};
    if (call.command < 0)
        return CommandStatus::unknown;
    std::unique_ptr<char[]> response{new (std::nothrow) char[kMaximumCommandResponseLength]};
    if (!response)
        return CommandStatus::failed;
    call.response = response.get();
    const int top = be_top(vm_);
    // Commands nest: a command a Berry command issues answers into its own response.
    auto* const previous = swap_berry_command_call(&call);
    // A command may arrive from a running script, whose deadline then still applies.
    const bool outer_active = execution_active;
    if (!outer_active) {
        execution_deadline = platform::monotonic_milliseconds() + kExecutionTimeoutMilliseconds;
        execution_active = true;
    }
    be_pushntvfunction(vm_, dispatch_berry_command);
    be_pushcomptr(vm_, &call);
    // As in Tasmota, a command that raises is still handled, with the response set so far.
    if (be_pcall(vm_, 1) != BE_OK)
        LogScriptError(vm_, "command");
    execution_active = outer_active;
    be_pop(vm_, be_top(vm_) - top);
    (void)swap_berry_command_call(previous);
    if (call.response_overflow) {
        AddLog(LogLevel::warning, "BRY: %s response too long", call.name.data());
        return CommandStatus::failed;
    }
    if (call.response_size != 0U && !output.write({call.response, call.response_size}))
        return CommandStatus::failed;
    return CommandStatus::handled;
}
#endif

void BerryService::save_persist() {
#if defined(USE_BERRY_PERSIST) && USE_BERRY_PERSIST
    if (!vm_)
        return;
    const int top = be_top(vm_);
    const bool outer_active = execution_active;
    const auto outer_deadline = execution_deadline;
    if (!outer_active) {
        execution_deadline = platform::monotonic_milliseconds() + kExecutionTimeoutMilliseconds;
        execution_active = true;
    }
    be_pushntvfunction(vm_, save_imported_persist);
    const int result = be_pcall(vm_, 0);
    if (result != BE_OK)
        LogScriptError(vm_, "persist restart save");
    be_pop(vm_, be_top(vm_) - top);
    execution_deadline = outer_deadline;
    execution_active = outer_active;
#endif
}

/** @brief Compiles and executes one expression-first Br command. */
CommandStatus BerryService::command(const CommandRequest& request, CommandOutput& output) {
    if (request.has_index || request.is_query()) {
        return CommandStatus::invalid;
    }
    if (execution_active) {
        (void)WriteResponse(output, "Berry VM is already executing");
        return CommandStatus::failed;
    }
    if (vm_ == nullptr) {
        (void)WriteResponse(output, "Berry not started");
        return CommandStatus::failed;
    }

    ClearStack(vm_);
    std::array<char, kExpressionCapacity> expression{};
    const int length = std::snprintf(expression.data(), expression.size(), "return (%.*s)",
        static_cast<int>(request.payload.size()), request.payload.data());
    if (length < 0 || static_cast<std::size_t>(length) >= expression.size()) {
        return CommandStatus::invalid;
    }

    be_pushntvfunction(vm_, ExecuteAndStringify);
    int result = be_loadbuffer(vm_, "input", expression.data(), static_cast<std::size_t>(length));
    if (be_getexcept(vm_, result) == BE_SYNTAX_ERROR) {
        ClearStack(vm_);
        be_pushntvfunction(vm_, ExecuteAndStringify);
        result = be_loadbuffer(vm_, "input", request.payload.data(), request.payload.size());
    }

    if (result == BE_OK) {
        execution_deadline = platform::monotonic_milliseconds() + kExecutionTimeoutMilliseconds;
        execution_active = true;
        result = be_pcall(vm_, 1);
        execution_active = false;
    }

    bool written = false;
    if (result == BE_OK) {
        // ExecuteAndStringify already produced a string; this only reads it.
        written = WriteResponse(output, be_tostring(vm_, 1));
    } else {
        const int top = be_top(vm_);
        const char* exception =
            top >= 2 && be_isstring(vm_, -2) ? be_tostring(vm_, -2) : "exception";
        const char* message =
            top >= 1 && be_isstring(vm_, -1) ? be_tostring(vm_, -1) : "Berry execution failed";
        std::array<char, 192> combined{};
        const int error_length =
            std::snprintf(combined.data(), combined.size(), "[%s] %s", exception, message);
        written = error_length >= 0 && static_cast<std::size_t>(error_length) < combined.size() &&
                  WriteResponse(output, combined.data());
    }
    ClearStack(vm_);
    return written ? CommandStatus::handled : CommandStatus::failed;
}

/** @brief Evaluates one raw Berry source string like the interactive REPL. */
BerryReplResult BerryService::repl(const std::string_view source) {
    if (source.empty() || source.size() > kBerryConsoleSourceCapacity ||
        source.find('\0') != std::string_view::npos) {
        return BerryReplResult::invalid;
    }
    // The expression lives on the heap only while it runs.
    const auto capacity = source.size() + kBerryConsoleBufferOverhead;
    auto* const buffer = new (std::nothrow) char[capacity];
    if (buffer == nullptr)
        return BerryReplResult::no_memory;
    std::memcpy(buffer, source.data(), source.size());
    return repl_owned(buffer, source.size(), capacity);
}

/** @brief Evaluates a chunk-uploaded source and owns its temporary heap buffer. */
BerryReplResult BerryService::repl_owned(
    char* const source, const std::size_t source_size, const std::size_t buffer_capacity) {
    if (source == nullptr)
        return BerryReplResult::invalid;
    if (source_size == 0U || source_size > kBerryConsoleMaxSourceBytes ||
        source_size > buffer_capacity ||
        kBerryConsoleBufferOverhead > buffer_capacity - source_size) {
        delete[] source;
        return BerryReplResult::invalid;
    }
    std::string_view source_view{source, source_size};
    if (source_view.find('\0') != std::string_view::npos) {
        delete[] source;
        return BerryReplResult::invalid;
    }

    std::memmove(source + kBerryExpressionPrefix.size(), source, source_size);
    std::memcpy(source, kBerryExpressionPrefix.data(), kBerryExpressionPrefix.size());
    source[kBerryExpressionPrefix.size() + source_size] = ')';
    const auto expression_size = kBerryExpressionPrefix.size() + source_size + 1U;
    source_view = {source + kBerryExpressionPrefix.size(), source_size};
    char* owned_source = source;
    const auto result = RunReplSource(vm_, {source, expression_size}, source_view, &owned_source);
    delete[] owned_source;
    return result;
}

/** @brief Streams and executes one Berry file inside the persistent VM. */
BerryScriptResult BerryService::execute_file(const char* const path) {
    if (vm_ == nullptr || path == nullptr || path[0] == '\0') {
        return BerryScriptResult::failed;
    }

    ClearStack(vm_);
    execution_deadline = platform::monotonic_milliseconds() + kExecutionTimeoutMilliseconds;
    execution_active = true;
    int result = be_loadfile(vm_, path);
    if (result == BE_OK) {
        result = be_pcall(vm_, 0);
    }
    execution_active = false;

    if (result == BE_OK) {
        ClearStack(vm_);
        AddLog(LogLevel::info, "BRY: Successfully loaded '%s'", path);
        return BerryScriptResult::executed;
    }
    if (result == BE_IO_ERROR) {
        const auto open_result = berry_file_open_result();
        ClearStack(vm_);
        if (open_result == BerryFileOpenResult::not_found) {
            return BerryScriptResult::not_found;
        }
        if (open_result == BerryFileOpenResult::unavailable) {
            return BerryScriptResult::unavailable;
        }
        AddLog(LogLevel::error, "BRY: cannot open '%s'", path);
        return BerryScriptResult::failed;
    }

    LogScriptError(vm_, path);
    ClearStack(vm_);
    return BerryScriptResult::failed;
}

} // namespace newmota
