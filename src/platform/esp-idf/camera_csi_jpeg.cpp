/*
  camera_csi_jpeg.cpp - ESP32-P4 hardware JPEG frames of the CSI camera for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera_csi_jpeg.cpp
 * @brief Ported from Tasmota's xdrv_81_1_esp32_webcam_CSI_mjpeg.ino.
 *
 * The processing task encodes a frame only while a reader asked within the last two
 * seconds, into a three-slot ring in DMA-capable memory. Only the newest frame stays ready;
 * readers of `camera.h` take/give (the port-81 server, Berry `cam.get_image()`) may hold it while
 * the next one is encoded. The still session uses one slot and captures only while a
 * reader waits.
 */
#include "camera_csi_internal.h"

#include "newmota/log.h"

#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <array>
#include <atomic>

namespace newmota::platform::camera_csi::detail {
namespace {
constexpr std::size_t kSlots = 3;
constexpr std::uint32_t kDemandMs = 2000;
constexpr std::uint32_t kWaitSliceMs = 50;

enum class SlotState : std::uint8_t { free, filling, ready };

struct Slot final {
    std::uint8_t* data{};
    std::size_t capacity{}, size{};
    std::uint32_t sequence{}, timestamp_ms{};
    SlotState state{SlotState::free};
    std::uint8_t readers{};
};

struct Jpeg final {
    jpeg_encoder_handle_t engine{};
    jpeg_encode_cfg_t cfg{};
    std::array<Slot, kSlots> slots{};
    std::uint32_t sequence{}, frames{}, errors{};
    bool release_pending{};
};

Jpeg jpeg{};
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t signal{};
std::atomic<bool> live{};
std::atomic<std::uint32_t> demand_until{};
std::atomic<std::uint32_t> waiting{};

std::uint32_t now_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

bool new_engine() {
    jpeg_encode_engine_cfg_t engine{};
    engine.timeout_ms = 100;
    if (jpeg_new_encoder_engine(&engine, &jpeg.engine) == ESP_OK)
        return true;
    jpeg.engine = nullptr;
    return false;
}

void free_slots() {
    for (auto& slot : jpeg.slots) {
        heap_caps_free(slot.data);
        slot = {};
    }
    jpeg.release_pending = false;
}

/** @brief Finds the slot to encode into, reclaiming the oldest unread ready one. */
Slot* reserve_slot() {
    Slot* chosen{};
    portENTER_CRITICAL(&lock);
    for (auto& slot : jpeg.slots)
        if (slot.data && slot.state == SlotState::free && slot.readers == 0U) {
            chosen = &slot;
            break;
        }
    if (!chosen)
        for (auto& slot : jpeg.slots)
            if (slot.data && slot.state == SlotState::ready && slot.readers == 0U &&
                (!chosen || slot.sequence < chosen->sequence))
                chosen = &slot;
    if (chosen)
        chosen->state = SlotState::filling;
    portEXIT_CRITICAL(&lock);
    return chosen;
}

void release_slot(Slot& slot) {
    portENTER_CRITICAL(&lock);
    slot.state = SlotState::free;
    portEXIT_CRITICAL(&lock);
}

/** @brief Makes a filled slot the only ready one and wakes readers. */
void publish(Slot& done, const std::size_t size) {
    portENTER_CRITICAL(&lock);
    for (auto& slot : jpeg.slots)
        if (&slot != &done && slot.state == SlotState::ready && slot.readers == 0U)
            slot.state = SlotState::free;
    done.size = size;
    done.sequence = ++jpeg.sequence;
    done.timestamp_ms = now_ms();
    done.state = SlotState::ready;
    ++jpeg.frames;
    portEXIT_CRITICAL(&lock);
    xSemaphoreGive(signal);
}
} // namespace

bool jpeg_open(const Mode& mode, const jpeg_enc_input_format_t input,
    const jpeg_down_sampling_type_t sampling, const std::uint8_t quality, const std::size_t slots) {
    if (jpeg.release_pending)
        return false;
    if (!signal && !(signal = xSemaphoreCreateBinary()))
        return false;
    if (!new_engine())
        return false;
    jpeg_encode_memory_alloc_cfg_t memory{};
    memory.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
    const auto capacity = static_cast<std::size_t>(mode.width) * mode.height / 2U;
    for (auto& slot : jpeg.slots)
        slot = {};
    for (std::size_t i = 0; i < std::min(slots, kSlots); ++i) {
        auto& slot = jpeg.slots[i];
        slot.data =
            static_cast<std::uint8_t*>(jpeg_alloc_encoder_mem(capacity, &memory, &slot.capacity));
        if (!slot.data) {
            jpeg_close();
            return false;
        }
    }
    jpeg.cfg = {};
    jpeg.cfg.height = mode.height;
    jpeg.cfg.width = mode.width;
    jpeg.cfg.src_type = input;
    jpeg.cfg.sub_sample = sampling;
    jpeg.cfg.image_quality = quality;
    jpeg.sequence = jpeg.frames = jpeg.errors = 0;
    live.store(true);
    return true;
}

void jpeg_close() {
    live.store(false);
    if (jpeg.engine)
        (void)jpeg_del_encoder_engine(jpeg.engine);
    jpeg.engine = nullptr;
    bool held{};
    portENTER_CRITICAL(&lock);
    for (auto& slot : jpeg.slots) {
        held = held || slot.readers != 0U;
        slot.state = SlotState::free;
    }
    jpeg.release_pending = held;
    portEXIT_CRITICAL(&lock);
    if (!held)
        free_slots();
    if (signal)
        xSemaphoreGive(signal); // waiting readers see the stop
}

bool jpeg_wanted() {
    return live.load() && static_cast<std::int32_t>(demand_until.load() - now_ms()) > 0;
}

bool jpeg_waiting() {
    return waiting.load() != 0U;
}

void jpeg_drop_ready() {
    portENTER_CRITICAL(&lock);
    for (auto& slot : jpeg.slots)
        if (slot.state == SlotState::ready && slot.readers == 0U)
            slot.state = SlotState::free;
    portEXIT_CRITICAL(&lock);
}

bool jpeg_encode(const std::uint8_t* frame, const std::size_t frame_size) {
    Slot* slot = reserve_slot();
    if (!slot || !jpeg.engine)
        return false;
    std::uint32_t size{};
    const auto started = esp_timer_get_time();
    const auto result =
        jpeg_encoder_process(jpeg.engine, &jpeg.cfg, frame, static_cast<std::uint32_t>(frame_size),
            slot->data, static_cast<std::uint32_t>(slot->capacity), &size);
    stats.encode_us = static_cast<std::uint32_t>(esp_timer_get_time() - started);
    if (result == ESP_ERR_INVALID_STATE) { // engine stuck: recreate it, as Tasmota does
        (void)jpeg_del_encoder_engine(jpeg.engine);
        jpeg.engine = nullptr;
        (void)new_engine();
        ++stats.jpeg_resets;
    }
    if (result != ESP_OK || size == 0U) {
        release_slot(*slot);
        ++stats.jpeg_errors;
        ++jpeg.errors;
        return false;
    }
    stats.last_size = size;
    stats.min_size = stats.min_size == 0U ? size : std::min(stats.min_size, size);
    stats.max_size = std::max(stats.max_size, size);
    stats.bytes_sent += size;
    publish(*slot, size);
    return true;
}

std::size_t jpeg_capacity() {
    return jpeg.slots[0].capacity;
}

} // namespace newmota::platform::camera_csi::detail

namespace newmota::platform::camera {
namespace jpeg = camera_csi::detail;

bool take(Frame& frame, const std::uint32_t after_sequence, const std::uint32_t timeout_ms) {
    using jpeg::lock;
    jpeg::demand_until.store(jpeg::now_ms() + jpeg::kDemandMs);
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    ++jpeg::waiting;

    struct Done final {
        ~Done() { --jpeg::waiting; }
    } done;

    for (;;) {
        bool live{}, found{};
        portENTER_CRITICAL(&lock);
        live = jpeg::live.load();
        jpeg::Slot* newest{};
        for (auto& slot : jpeg::jpeg.slots)
            if (slot.state == jpeg::SlotState::ready && slot.sequence > after_sequence &&
                (!newest || slot.sequence > newest->sequence))
                newest = &slot;
        if (live && newest) {
            ++newest->readers;
            frame.data = newest->data;
            frame.size = newest->size;
            frame.sequence = newest->sequence;
            frame.timestamp_ms = newest->timestamp_ms;
            frame.slot = static_cast<std::uint8_t>(newest - jpeg::jpeg.slots.data());
            found = true;
        }
        portEXIT_CRITICAL(&lock);
        if (!live)
            return false;
        if (found)
            return true;
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= limit || !jpeg::signal)
            return false;
        const TickType_t slice =
            std::min<TickType_t>(limit - elapsed, pdMS_TO_TICKS(jpeg::kWaitSliceMs));
        (void)xSemaphoreTake(jpeg::signal, slice > 0 ? slice : 1);
    }
}

void give(Frame& frame) {
    using jpeg::lock;
    if (frame.slot >= jpeg::kSlots)
        return;
    bool release{};
    portENTER_CRITICAL(&lock);
    auto& slot = jpeg::jpeg.slots[frame.slot];
    if (slot.readers != 0U)
        --slot.readers;
    if (jpeg::jpeg.release_pending) {
        release = true;
        for (const auto& item : jpeg::jpeg.slots)
            release = release && item.readers == 0U;
    }
    portEXIT_CRITICAL(&lock);
    if (release)
        jpeg::free_slots();
    frame = {};
}

Statistics statistics() {
    Statistics result{};
    result.frames = jpeg::jpeg.frames;
    result.dropped = jpeg::stats.frames_unsent;
    result.invalid = jpeg::jpeg.errors;
    result.buffer_size = jpeg::jpeg_capacity();
    result.capturing = jpeg::live.load();
    return result;
}

} // namespace newmota::platform::camera
