/*
  url_update_limits.cpp - host regression tests for OTA writer chunk limits

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/update/url_update_limits.h"

#include <cstdio>

int main() {
    using newmota::url_update_detail::bounded_writer_chunk_capacity;
    using newmota::url_update_detail::writer_chunk_limits_are_usable;
    int failures = 0;
    const auto expect = [&failures](const bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "URL update limits: %s\n", message);
            ++failures;
        }
    };

    expect(bounded_writer_chunk_capacity(4096U) == 1024U,
        "4 KiB Safeboot writers fit the 1 KiB transport buffer");
    expect(bounded_writer_chunk_capacity(512U) == 512U,
        "smaller native writer limits remain unchanged");
    expect(writer_chunk_limits_are_usable(4096U, 416U),
        "a native initial prefix fits the capped first write");
    expect(!writer_chunk_limits_are_usable(4096U, 1025U),
        "an initial prefix larger than the transport buffer is rejected");
    expect(!writer_chunk_limits_are_usable(0U, 1U), "zero-sized writer chunks are rejected");
    return failures == 0 ? 0 : 1;
}
