/*
  update_web_service.h - bounded browser firmware upload endpoint

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/update/update_session.h"
#include "core/system/restart.h"

#include <cstdint>
#include <string_view>

namespace newmota::platform {
struct WebRequest;
struct WebResponse;
class UpdateWriter;
} // namespace newmota::platform

namespace newmota {

/** @brief Handles authenticated, bounded browser requests for one native update. */
class UpdateWebService final {
  public:
    /** @brief Binds one native writer and the delayed application restart. */
    UpdateWebService(platform::UpdateWriter& writer, RestartController& restart);

    /** @brief Handles one /api/update request after its caller checks browser auth. */
    [[nodiscard]] bool handle(
        const platform::WebRequest& request, platform::WebResponse& response, std::uint32_t now);

    /** @brief Names the requests that pace themselves and so skip the per-second limits: the
     * browser sends one chunk after the other, each at the offset the session expects, and an
     * abort must always reach the session so it does not hold the update for five minutes.
     */
    [[nodiscard]] static bool self_paced(const bool post, const std::string_view path) {
        return post && (path == "/api/update/chunk" || path == "/api/update/abort");
    }

    /** @brief Aborts a stalled receiving session after five minutes. */
    void poll(std::uint32_t now);

    /** @brief Reports whether an upload or pending activation holds the update lease. */
    [[nodiscard]] bool active() const {
        const auto state = session_.state();
        return state == UpdateSessionState::receiving || state == UpdateSessionState::verified ||
               state == UpdateSessionState::activated;
    }

  private:
    platform::UpdateWriter& writer_;
    UpdateSession session_;
    RestartController& restart_;
    std::uint32_t last_activity_{}, update_rate_window_{};
    unsigned int update_requests_{};
};

} // namespace newmota
