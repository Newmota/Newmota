/*
  web_response.cpp - Shared platform web response helpers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file web_response.cpp
 * @brief Shared platform web response helpers.
 */

#include "newmota/platform/web.h"

namespace newmota::platform {

// An out-of-line constructor avoids storing empty response bodies in flash merely
// because the response also has nonzero default metadata.
WebResponse::WebResponse() = default;

const char* web_status_line(const unsigned int status) {
    switch (status) {
    case 200:
        return "200 OK";
    case 202:
        return "202 Accepted";
    case 400:
        return "400 Bad Request";
    case 401:
        return "401 Unauthorized";
    case 403:
        return "403 Forbidden";
    case 404:
        return "404 Not Found";
    case 406:
        return "406 Not Acceptable";
    case 408:
        return "408 Request Timeout";
    case 409:
        return "409 Conflict";
    case 413:
        return "413 Content Too Large";
    case 414:
        return "414 URI Too Long";
    case 415:
        return "415 Unsupported Media Type";
    case 422:
        return "422 Unprocessable Content";
    case 429:
        return "429 Too Many Requests";
    case 431:
        return "431 Request Header Fields Too Large";
    case 503:
        return "503 Service Unavailable";
    case 504:
        return "504 Gateway Timeout";
    case 507:
        return "507 Insufficient Storage";
    default:
        return "500 Internal Server Error";
    }
}

} // namespace newmota::platform
