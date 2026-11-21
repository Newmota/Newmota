/*
  camera.cpp - ESP-IDF DVP camera capture for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera.cpp
 * @brief JPEG capture through esp_cam_ctlr_dvp into a three-slot PSRAM frame ring.
 *
 * The controller ISR picks the next buffer and publishes completed frames. Only the newest
 * completed frame stays ready; readers may hold it while the ISR fills the other slots.
 * When no slot is free the driver's backup buffer absorbs the frame and it is dropped.
 */
#include "newmota/platform/camera.h"

#include "esp_attr.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_private/esp_cam_dvp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <array>

namespace newmota::platform::camera {
namespace {
constexpr int kController = 0;
constexpr std::size_t kSlots = 3;
constexpr std::uint32_t kWaitSliceMs = 50;
constexpr std::uint32_t kDmaBurstBytes = 128;

enum class SlotState : std::uint8_t { free, filling, ready };

struct Slot final {
    std::uint8_t* data{};
    std::size_t size{};
    std::uint32_t sequence{};
    std::uint32_t timestamp_ms{};
    SlotState state{SlotState::free};
    std::uint8_t readers{};
};

struct State final {
    esp_cam_ctlr_handle_t handle{};
    std::array<Slot, kSlots> slots{};
    esp_cam_ctlr_dvp_pin_config_t pins{};
    std::size_t buffer_size{};
    std::uint32_t xclk_hz{};
    std::uint32_t sequence{}, dropped{}, invalid{};
    bool clock{}, capturing{};
    // Stop found a held slot; the last give() frees the ring.
    bool release_pending{};
};

State state{};
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t frame_signal{};

gpio_num_t pad(PinId pin) {
    return pin.valid() ? static_cast<gpio_num_t>(pin.value()) : GPIO_NUM_NC;
}

/** @brief Frees every slot buffer; callers own the lock-free teardown window. */
void free_slots() {
    for (auto& slot : state.slots) {
        heap_caps_free(slot.data);
        slot = {};
    }
    state.release_pending = false;
}

/** @brief Gives the controller a free slot, reclaiming an unread ready frame if needed. */
bool IRAM_ATTR on_get_new_trans(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* trans, void*) {
    portENTER_CRITICAL_ISR(&lock);
    Slot* chosen{};
    for (auto& slot : state.slots)
        if (slot.data && slot.state == SlotState::free && slot.readers == 0U) {
            chosen = &slot;
            break;
        }
    if (!chosen)
        for (auto& slot : state.slots)
            if (slot.data && slot.state == SlotState::ready && slot.readers == 0U &&
                (!chosen || slot.sequence < chosen->sequence))
                chosen = &slot;
    if (chosen) {
        if (chosen->state == SlotState::ready)
            ++state.dropped;
        chosen->state = SlotState::filling;
        trans->buffer = chosen->data;
        trans->buflen = state.buffer_size;
    } else {
        ++state.dropped; // the driver's backup buffer takes this frame
    }
    portEXIT_CRITICAL_ISR(&lock);
    return false;
}

/** @brief Publishes a completed JPEG frame as the only ready slot. */
bool IRAM_ATTR on_trans_finished(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* trans, void*) {
    bool published{};
    portENTER_CRITICAL_ISR(&lock);
    Slot* done{};
    for (auto& slot : state.slots)
        if (slot.data && slot.data == trans->buffer)
            done = &slot;
    if (done) {
        if (trans->received_size == 0U || trans->received_size > state.buffer_size) {
            ++state.invalid;
            done->state = SlotState::free;
        } else {
            for (auto& slot : state.slots)
                if (&slot != done && slot.state == SlotState::ready && slot.readers == 0U)
                    slot.state = SlotState::free;
            done->size = trans->received_size;
            done->sequence = ++state.sequence;
            done->timestamp_ms = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;
            done->state = SlotState::ready;
            published = true;
        }
    }
    portEXIT_CRITICAL_ISR(&lock);
    BaseType_t woken = pdFALSE;
    if (published && frame_signal)
        xSemaphoreGiveFromISR(frame_signal, &woken);
    return woken == pdTRUE;
}

/** @brief Releases the controller handle after capture has stopped. */
void delete_controller() {
    if (!state.handle)
        return;
    (void)esp_cam_ctlr_stop(state.handle);
    (void)esp_cam_ctlr_disable(state.handle);
    (void)esp_cam_ctlr_del(state.handle);
    state.handle = nullptr;
}
} // namespace

Result start_clock(const Pins& pins, const std::uint32_t xclk_hz) {
    if (state.clock || state.handle)
        return Result::busy;
    if (!pins.xclk.valid() || !pins.pclk.valid() || !pins.vsync.valid() || !pins.href.valid() ||
        xclk_hz == 0U)
        return Result::invalid_argument;
    for (const auto pin : pins.data)
        if (!pin.valid())
            return Result::invalid_argument;
    if (!frame_signal) {
        frame_signal = xSemaphoreCreateBinary();
        if (!frame_signal)
            return Result::no_memory;
    }
    state.pins = {};
    state.pins.data_width = CAM_CTLR_DATA_WIDTH_8;
    for (auto& io : state.pins.data_io) // the signal count macro differs between IDF 5 and 6
        io = GPIO_NUM_NC;
    for (std::size_t i = 0; i < kDataPins; ++i)
        state.pins.data_io[i] = pad(pins.data[i]);
    state.pins.vsync_io = pad(pins.vsync);
    state.pins.de_io = pad(pins.href);
    state.pins.pclk_io = pad(pins.pclk);
    state.pins.xclk_io = pad(pins.xclk);
    if (esp_cam_ctlr_dvp_init(kController, CAM_CLK_SRC_DEFAULT, &state.pins) != ESP_OK)
        return Result::failed;
    if (esp_cam_ctlr_dvp_output_clock(kController, CAM_CLK_SRC_DEFAULT, xclk_hz) != ESP_OK) {
        (void)esp_cam_ctlr_dvp_deinit(kController);
        return Result::invalid_argument;
    }
    state.xclk_hz = xclk_hz;
    state.clock = true;
    return Result::success;
}

Result start(const std::size_t buffer_size) {
    if (!state.clock)
        return Result::unavailable;
    if (state.handle || state.release_pending)
        return Result::busy;
    if (buffer_size == 0U)
        return Result::invalid_argument;
    // JPEG mode sizes the DMA buffer as h_res * v_res and ignores line geometry.
    esp_cam_ctlr_dvp_config_t config{};
    config.ctlr_id = kController;
    config.clk_src = CAM_CLK_SRC_DEFAULT;
    config.h_res = static_cast<std::uint32_t>(buffer_size);
    config.v_res = 1U;
    config.input_data_color_type = CAM_CTLR_COLOR_RGB565;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    // Equal formats keep IDF 6's RGB/YUV converter off; JPEG bytes are not pixels.
    config.output_data_color_type = config.input_data_color_type;
#endif
    config.cam_data_width = 8U;
    config.pic_format_jpeg = 1U;
    config.pin_dont_init = 1U;
    config.dma_burst_size = kDmaBurstBytes;
    config.xclk_freq = state.xclk_hz;
    config.pin = &state.pins;
    if (esp_cam_new_dvp_ctlr(&config, &state.handle) != ESP_OK) {
        state.handle = nullptr;
        return Result::failed;
    }
    std::size_t length{};
    if (esp_cam_ctlr_get_frame_buffer_len(state.handle, &length) != ESP_OK || length == 0U) {
        delete_controller();
        return Result::failed;
    }
    for (auto& slot : state.slots) {
        slot = {};
        slot.data = static_cast<std::uint8_t*>(
            esp_cam_ctlr_alloc_buffer(state.handle, length, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
        if (!slot.data) {
            free_slots();
            delete_controller();
            return Result::no_memory;
        }
    }
    state.buffer_size = length;
    state.sequence = state.dropped = state.invalid = 0U;
    esp_cam_ctlr_evt_cbs_t callbacks{}; // IDF 6 adds members after these two
    callbacks.on_get_new_trans = on_get_new_trans;
    callbacks.on_trans_finished = on_trans_finished;
    if (esp_cam_ctlr_register_event_callbacks(state.handle, &callbacks, nullptr) != ESP_OK ||
        esp_cam_ctlr_enable(state.handle) != ESP_OK) {
        delete_controller();
        free_slots();
        return Result::failed;
    }
    state.capturing = true;
    if (esp_cam_ctlr_start(state.handle) != ESP_OK) {
        state.capturing = false;
        delete_controller();
        free_slots();
        return Result::failed;
    }
    return Result::success;
}

void stop() {
    portENTER_CRITICAL(&lock);
    state.capturing = false;
    portEXIT_CRITICAL(&lock);
    delete_controller();
    if (state.clock) {
        (void)esp_cam_ctlr_dvp_deinit(kController);
        state.clock = false;
    }
    bool held{};
    portENTER_CRITICAL(&lock);
    for (auto& slot : state.slots) {
        held = held || slot.readers != 0U;
        slot.state = SlotState::free;
    }
    state.release_pending = held;
    portEXIT_CRITICAL(&lock);
    if (!held)
        free_slots();
    if (frame_signal)
        xSemaphoreGive(frame_signal); // wake a waiting reader so it sees the stop
}

bool clock_running() {
    return state.clock;
}

bool take(Frame& frame, const std::uint32_t after_sequence, const std::uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        bool capturing{}, found{};
        portENTER_CRITICAL(&lock);
        capturing = state.capturing;
        Slot* newest{};
        for (auto& slot : state.slots)
            if (slot.state == SlotState::ready && slot.sequence > after_sequence &&
                (!newest || slot.sequence > newest->sequence))
                newest = &slot;
        if (capturing && newest) {
            ++newest->readers;
            frame.data = newest->data;
            frame.size = newest->size;
            frame.sequence = newest->sequence;
            frame.timestamp_ms = newest->timestamp_ms;
            frame.slot = static_cast<std::uint8_t>(newest - state.slots.data());
            found = true;
        }
        portEXIT_CRITICAL(&lock);
        if (!capturing)
            return false;
        if (found)
            return true;
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= limit)
            return false;
        // Several readers share one binary semaphore, so wait in short slices.
        const TickType_t slice = std::min<TickType_t>(limit - elapsed, pdMS_TO_TICKS(kWaitSliceMs));
        (void)xSemaphoreTake(frame_signal, slice > 0 ? slice : 1);
    }
}

void give(Frame& frame) {
    if (frame.slot >= kSlots)
        return;
    bool release{};
    portENTER_CRITICAL(&lock);
    auto& slot = state.slots[frame.slot];
    if (slot.readers != 0U)
        --slot.readers;
    if (state.release_pending) {
        release = true;
        for (const auto& item : state.slots)
            release = release && item.readers == 0U;
    }
    portEXIT_CRITICAL(&lock);
    if (release)
        free_slots();
    frame = {};
}

Statistics statistics() {
    Statistics result{};
    portENTER_CRITICAL(&lock);
    result.frames = state.sequence;
    result.dropped = state.dropped;
    result.invalid = state.invalid;
    result.buffer_size = state.buffer_size;
    result.capturing = state.capturing;
    portEXIT_CRITICAL(&lock);
    return result;
}

} // namespace newmota::platform::camera
