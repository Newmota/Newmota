/*
  compiler_gate.c - Space out AmebaD compiler launches

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/**
 * @file compiler_gate.c
 * @brief Starts one compiler per interval, then runs it unchanged.
 *
 * Every Realtek ASDK gcc driver briefly takes a per-user lock for its license check and, when
 * the lock is busy, sleeps a whole second before retrying. Jobs started together collide and
 * then retry together, which makes parallel builds slower than serial ones. The check itself
 * still runs in every driver; the gate only keeps the drivers from starting at the same moment.
 */

#define _DEFAULT_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

/** Measured on a bw16-2m build: long enough to avoid nearly all driver lock collisions. */
#define LAUNCH_SPACING_NS 5000000L

/**
 * @brief Waits for this process's launch slot. Any failure only loses the spacing.
 * @param path Lock file shared by all compiler launches of this user.
 */
static void wait_for_launch_slot(const char *path)
{
    if (path == NULL) {
        return;
    }
    const int descriptor = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (descriptor < 0) {
        return;
    }
    if (flock(descriptor, LOCK_EX) == 0) {
        const struct timespec spacing = {0, LAUNCH_SPACING_NS};
        nanosleep(&spacing, NULL);
    }
    close(descriptor);
}

/**
 * @brief Runs argv[1] with its arguments once a launch slot is free.
 * @return Does not return on success; 2 on misuse and 127 when the compiler cannot start.
 */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <compiler> [arguments...]\n", argv[0]);
        return 2;
    }
    wait_for_launch_slot(getenv("NEWMOTA_COMPILER_GATE_LOCK"));
    execvp(argv[1], &argv[1]);
    perror(argv[1]);
    return 127;
}
