/*
  camera_web.cpp - ESP-IDF snapshot and MJPEG endpoint for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera_web.cpp
 * @brief A second, plain HTTP server for camera frames, like Tasmota's port-81 stream.
 *
 * Frames are larger than the main web service's bounded response, so they bypass it. A
 * stream request is handed off as an asynchronous request to one sender task, which writes
 * each frame to every viewer, as Tasmota's stream does; the server task stays free for pages
 * and snapshots.
 */
#include "newmota/platform/camera.h"
#include "newmota_features.h"
#if USE_CSI_WEBCAM
#include "newmota/platform/camera_csi.h"
#include "lwip/sockets.h"
#endif

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <sys/types.h>

namespace newmota::platform::camera {
namespace {
constexpr char kBoundary[] = "newmotaframe";
// A full-sensor still takes about a second to capture, convert and encode.
constexpr std::uint32_t kFrameTimeoutMs = USE_CSI_STILL ? 5000 : 2000;
constexpr std::size_t kMaxStreams = 3;
// A refused or broken stream retries, so a viewer turned away while the others were full joins.
constexpr char kIndex[] =
    "<!doctype html><html><head><meta name=viewport content=\"width=device-width\">"
    "<title>Camera</title></head><body style=\"margin:0;background:#000\">"
    "<img src=\"/stream\" style=\"width:100%\" onerror=\"setTimeout(()=>"
    "this.src='/stream?'+Date.now(),2000)\"></body></html>";

httpd_handle_t server{};
std::atomic<bool> stopping{};
std::atomic<bool> sending{};
std::atomic<std::uint32_t> started{};
// The server task fills empty slots and the sender empties them.
std::array<std::atomic<httpd_req_t*>, kMaxStreams> streams{};

#if USE_CSI_WEBCAM
// H.264 over the port-82 WebSocket into WebCodecs, Tasmota's player. Each message is a key
// flag byte followed by Annex B data. Safari opens ws:// on plain HTTP only after a kick.
constexpr char kH264Page[] =
    "<!doctype html><html><head><meta name=viewport content=\"width=device-width\">"
    "<title>Camera</title></head><body style=\"margin:0;background:#000\">"
    "<canvas id=c style=\"width:100%\"></canvas><script>(()=>{"
    "const c=document.getElementById('c'),x=c.getContext('2d');"
    "const U=(location.protocol==='https:'?'wss://':'ws://')+location.hostname+':82/';"
    "let w=null,d=null,k=false,run=false,n=0,b=0,t=performance.now(),fps=0,kb=0;"
    "function hud(){x.save();x.font='14px monospace';x.fillStyle='rgba(0,0,0,.6)';"
    "x.fillRect(8,8,150,40);x.fillStyle='#0f0';x.fillText('FPS: '+fps,12,24);"
    "x.fillText('KB/s: '+kb,12,40);x.restore();}"
    "function stop(){run=false;if(w){w.close();w=null;}if(d){d.close();d=null;}}"
    "function open(){return new Promise((y,n)=>{const s=new WebSocket(U);"
    "s.onopen=()=>y(s);s.onerror=n;});}"
    "async function start(){stop();run=true;k=false;"
    "if(!('VideoDecoder' in window)){document.body.append('No WebCodecs');return;}"
    "d=new VideoDecoder({output:f=>{if(c.width!==f.displayWidth)c.width=f.displayWidth;"
    "if(c.height!==f.displayHeight)c.height=f.displayHeight;x.drawImage(f,0,0);hud();"
    "f.close();n++;},error:e=>{console.error(e);stop();}});"
    "d.configure({codec:'avc1.42001E',optimizeForLatency:true});"
    "try{const q=new WebSocket(U);q.onopen=()=>q.close();}catch(e){}"
    "await new Promise(r=>setTimeout(r,80));"
    "try{w=await open();}catch(e){setTimeout(start,1000);return;}"
    "w.binaryType='arraybuffer';w.onclose=()=>{if(run)setTimeout(start,1000);};"
    "w.onmessage=e=>{b+=e.data.byteLength;const u=new Uint8Array(e.data);if(u.length<2)return;"
    "const key=u[0]===1;if(key)k=true;if(!k)return;"
    "try{d.decode(new EncodedVideoChunk({type:key?'key':'delta',"
    "timestamp:performance.now()*1000,data:u.subarray(1)}));}catch(x){console.error(x);}"
    "if(performance.now()-t>1000){fps=n;kb=Math.round(b/1024);n=0;b=0;t=performance.now();}};}"
    "start();})();</script></body></html>";

// WebRTC with the offer posted to this server; LAN only, so no STUN servers.
#if USE_CSI_STILL
// The still session captures one still per request; the button asks for the next.
constexpr char kStillPage[] =
    "<!doctype html><html><head><meta name=viewport content=\"width=device-width\">"
    "<title>Camera</title></head><body style=\"margin:0;background:#000;color:#ccc;"
    "font:14px sans-serif;text-align:center\"><img id=i style=\"width:100%\" alt=\"\">"
    "<p><button id=b onclick=c()>&#128247;</button> <span id=s></span></p><script>"
    "function c(){var i=document.getElementById('i'),b=document.getElementById('b'),"
    "s=document.getElementById('s');b.disabled=true;s.textContent='\u2026';"
    "i.onload=function(){s.textContent=i.naturalWidth+'\u00d7'+i.naturalHeight;"
    "b.disabled=false};i.onerror=function(){s.textContent='\u2715';b.disabled=false};"
    "i.src='/snapshot.jpg?'+Date.now()}c()</script></body></html>";
#endif

constexpr char kWebRtcPage[] =
    "<!doctype html><html><head><meta name=viewport content=\"width=device-width\">"
    "<title>Camera</title></head><body style=\"margin:0;background:#000;color:#ccc\">"
    "<video id=v autoplay muted controls playsinline style=\"width:100%\"></video>"
    "<pre id=l style=\"font-size:10px\"></pre><script>"
    "const l=s=>{document.getElementById('l').textContent+=s+'\\n';};"
    "const p=new RTCPeerConnection({iceServers:[]});"
    "p.ontrack=e=>{const v=document.getElementById('v');"
    "if(e.streams&&e.streams[0])v.srcObject=e.streams[0];"
    "else{if(!v.srcObject)v.srcObject=new MediaStream();v.srcObject.addTrack(e.track);}};"
    "p.oniceconnectionstatechange=()=>l('ICE: '+p.iceConnectionState);"
    "p.addTransceiver('video',{direction:'recvonly'});"
    "p.addTransceiver('audio',{direction:'recvonly'});"
    "p.createOffer().then(o=>p.setLocalDescription(o)).then(()=>fetch('/webrtc/offer',"
    "{method:'POST',headers:{'Content-Type':'text/plain'},body:p.localDescription.sdp}))"
    ".then(r=>{if(!r.ok)throw new Error('Server '+r.status);return r.text();})"
    ".then(s=>p.setRemoteDescription({type:'answer',sdp:s})).then(()=>l('Answer set'))"
    ".catch(e=>l('ERROR: '+e.message));</script></body></html>";

constexpr std::size_t kMaxOffer = 16384;
constexpr std::size_t kMaxAnswer = 4096;

/** @brief Answers a WebRTC offer in the request body with the device's SDP. */
esp_err_t offer_handler(httpd_req_t* request) {
    if (request->content_len == 0 || request->content_len > kMaxOffer) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_send(request, "Body missing or too large", HTTPD_RESP_USE_STRLEN);
    }
    char* offer = static_cast<char*>(std::malloc(request->content_len));
    char* answer = static_cast<char*>(std::malloc(kMaxAnswer));
    std::size_t received{};
    while (offer && answer && received < request->content_len) {
        const int size = httpd_req_recv(request, offer + received, request->content_len - received);
        if (size <= 0)
            break;
        received += static_cast<std::size_t>(size);
    }
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    (void)lwip_getsockname(
        httpd_req_to_sockfd(request), reinterpret_cast<sockaddr*>(&local), &length);
    const std::size_t size =
        offer && answer && received == request->content_len
            ? camera_csi::webrtc_answer(offer, received, local.sin_addr.s_addr, answer, kMaxAnswer)
            : 0U;
    esp_err_t result;
    if (size == 0U) {
        httpd_resp_set_status(request, "500 Internal Server Error");
        result = httpd_resp_send(request, "WebRTC session not ready", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_set_type(request, "application/sdp");
        result = httpd_resp_send(request, answer, static_cast<ssize_t>(size));
    }
    std::free(offer);
    std::free(answer);
    return result;
}
#endif

esp_err_t index_handler(httpd_req_t* request) {
    httpd_resp_set_type(request, "text/html");
#if USE_CSI_WEBCAM
    return httpd_resp_send(request, camera_csi::page(), HTTPD_RESP_USE_STRLEN);
#else
    return httpd_resp_send(request, kIndex, HTTPD_RESP_USE_STRLEN);
#endif
}

esp_err_t snapshot_handler(httpd_req_t* request) {
    Frame frame{};
    if (!take(frame, 0U, kFrameTimeoutMs)) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_send(request, "Camera not streaming", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(request, "image/jpeg");
    httpd_resp_set_hdr(request, "Content-Disposition", "inline; filename=snapshot.jpg");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const auto result =
        httpd_resp_send(request, reinterpret_cast<const char*>(frame.data), frame.size);
    give(frame);
    return result;
}

/** @brief Ends a viewer's stream and hands its session back to the server. */
void finish(httpd_req_t* stream, const bool sent) {
    if (sent)
        (void)httpd_resp_send_chunk(stream, nullptr, 0);
    (void)httpd_req_async_handler_complete(stream);
}

/** @brief Writes each new frame to every viewer until the endpoint stops. */
void send_frames(void*) {
    std::uint32_t last{};
    while (!stopping.load()) {
        bool viewers = false;
        for (auto& slot : streams)
            viewers = viewers || slot.load() != nullptr;
        if (!viewers) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        Frame frame{};
        if (!take(frame, last, kFrameTimeoutMs)) {
            if (!statistics().capturing)
                for (auto& slot : streams)
                    if (auto* const stream = slot.exchange(nullptr))
                        finish(stream, true);
            continue;
        }
        last = frame.sequence;
        char header[96];
        const int length = std::snprintf(header, sizeof(header),
            "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", kBoundary,
            unsigned(frame.size));
        for (auto& slot : streams) {
            auto* const stream = slot.load();
            if (stream == nullptr)
                continue;
            const bool sent =
                httpd_resp_send_chunk(stream, header, length) == ESP_OK &&
                httpd_resp_send_chunk(stream, reinterpret_cast<const char*>(frame.data),
                    static_cast<ssize_t>(frame.size)) == ESP_OK &&
                httpd_resp_send_chunk(stream, "\r\n", 2) == ESP_OK;
            if (!sent) {
                slot.store(nullptr);
                finish(stream, false);
            }
        }
        give(frame);
    }
    for (auto& slot : streams)
        if (auto* const stream = slot.exchange(nullptr))
            finish(stream, true);
    sending.store(false);
    vTaskDelete(nullptr);
}

/** @brief Ends the sender, which ends its streams within one frame wait or send timeout. */
void stop_sender() {
    stopping.store(true);
    while (sending.load())
        vTaskDelay(pdMS_TO_TICKS(10));
}

esp_err_t stream_handler(httpd_req_t* request) {
    // Only this task fills slots, so a slot found empty stays empty until it is filled.
    std::atomic<httpd_req_t*>* empty{};
    for (auto& slot : streams)
        if (empty == nullptr && slot.load() == nullptr)
            empty = &slot;
    httpd_req_t* stream{};
    if (empty == nullptr || httpd_req_async_handler_begin(request, &stream) != ESP_OK) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_send(request, "Too many viewers", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(stream, "multipart/x-mixed-replace;boundary=newmotaframe");
    httpd_resp_set_hdr(stream, "Cache-Control", "no-store");
    empty->store(stream);
    // A sender that already swept its slots on the way out leaves this one to us.
    if (stopping.load())
        if (auto* const late = empty->exchange(nullptr))
            finish(late, false);
    return ESP_OK;
}

bool route(const char* uri, esp_err_t (*handler)(httpd_req_t*), httpd_method_t method = HTTP_GET) {
    const httpd_uri_t entry{uri, method, handler, nullptr};
    return httpd_register_uri_handler(server, &entry) == ESP_OK;
}
} // namespace

bool serve(const bool enable, const std::uint16_t port) {
    if (!enable) {
        if (server) {
            stop_sender();
            (void)httpd_stop(server);
            server = nullptr;
        }
        return true;
    }
    if (server)
        return true;
#if USE_CSI_WEBCAM
    if (!camera_csi::network_available())
        return false;
#endif
    stopping.store(false);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = static_cast<std::uint16_t>(32768U + port); // the main server keeps 32768
    config.max_uri_handlers = 6;
    config.max_open_sockets = kMaxStreams + 1U; // the streams plus a page or snapshot
    config.lru_purge_enable = true;
    config.stack_size = USE_CSI_WEBCAM ? 8192 : 4096; // the SDP answer needs room
    sending.store(true);
    if (xTaskCreate(send_frames, "camstream", 4096, nullptr, config.task_priority, nullptr) !=
        pdPASS) {
        sending.store(false);
        return false;
    }
    if (httpd_start(&server, &config) != ESP_OK) {
        stop_sender();
        server = nullptr;
        return false;
    }
    if (!route("/", index_handler) || !route("/snapshot.jpg", snapshot_handler) ||
        !route("/cam.jpg", snapshot_handler) || !route("/stream", stream_handler) ||
        !route("/cam.mjpeg", stream_handler)
#if USE_CSI_WEBCAM
        || !route("/webrtc/offer", offer_handler, HTTP_POST)
#endif
    ) {
        stop_sender();
        (void)httpd_stop(server);
        server = nullptr;
        return false;
    }
    started.fetch_add(1U);
    return true;
}

bool serving() {
    return server != nullptr;
}

std::uint32_t serves() {
    return started.load();
}

} // namespace newmota::platform::camera

#if USE_CSI_WEBCAM
namespace newmota::platform {
const char* camera_csi::page() {
    switch (info().session) {
    case Session::h264:
        return camera::kH264Page;
    case Session::webrtc:
        return camera::kWebRtcPage;
#if USE_CSI_STILL
    case Session::still:
        return camera::kStillPage;
#endif
    default:
        return camera::kIndex;
    }
}
} // namespace newmota::platform
#endif
