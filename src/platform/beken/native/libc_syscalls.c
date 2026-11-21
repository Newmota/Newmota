/*
  libc_syscalls.c - Beken newlib file and process hooks

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file libc_syscalls.c
 * @brief Beken newlib file and process hooks.
 *
 * The BDK has no newlib files or processes. Descriptors 0-2 are the SDK log
 * UART: stdout and stderr (printf, LittleFS diagnostics, assert) write to it
 * like bk_printf, while console input stays with the Newmota console. Every
 * other hook reports ENOSYS, as the libnosys placeholders did.
 */

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "uart_pub.h"

/* The BDK include path resolves <errno.h> to lwIP's Linux numbering; these
 * hooks report newlib's own codes. */
#include <sys/errno.h>

/* The newlib _r wrappers read the global errno after these calls. */
#undef errno
extern int errno;

/** @brief Reports whether @p file is one of the standard console streams. */
static int is_console(int file) {
    return file >= 0 && file <= 2;
}

/* Unbuffered stdout needs no heap buffer and reaches the UART before a
 * following crash; the UART itself is written one polled byte at a time. */
__attribute__((constructor)) static void unbuffer_stdout(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
}

int _close(int file) {
    (void)file;
    errno = ENOSYS;
    return -1;
}

int _fstat(int file, struct stat* status) {
    if (is_console(file)) {
        status->st_mode = S_IFCHR;
        return 0;
    }
    errno = ENOSYS;
    return -1;
}

int _isatty(int file) {
    if (is_console(file)) {
        return 1;
    }
    errno = ENOSYS;
    return 0;
}

off_t _lseek(int file, off_t offset, int whence) {
    (void)file;
    (void)offset;
    (void)whence;
    errno = ENOSYS;
    return -1;
}

int _read(int file, char* buffer, int length) {
    (void)file;
    (void)buffer;
    (void)length;
    errno = ENOSYS;
    return -1;
}

/** @brief Writes stdout and stderr to the SDK log UART with bk_printf's CRLF. */
int _write(int file, const char* buffer, int length) {
    if (file != 1 && file != 2) {
        errno = ENOSYS;
        return -1;
    }
    static char previous;
    for (int index = 0; index < length; ++index) {
        const char character = buffer[index];
        if (character == '\n' && previous != '\r') {
            uart_write_byte(uart_print_port, '\r');
        }
        uart_write_byte(uart_print_port, character);
        previous = character;
    }
    return length;
}

int _kill(int process, int signal) {
    (void)process;
    (void)signal;
    errno = ENOSYS;
    return -1;
}

int _getpid(void) {
    errno = ENOSYS;
    return -1;
}
