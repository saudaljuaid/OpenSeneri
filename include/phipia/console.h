/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_CONSOLE_H
#define PHIPIA_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

void console_initialize(void);
void console_putc(char character);
void console_write(const char *text);
void console_write_n(const char *text, size_t length);
void console_write_hex(uint64_t value);
void console_write_u64(uint64_t value);
void console_serial_write(const char *text);
void console_serial_write_u64(uint64_t value);
_Noreturn void console_halt(void);
_Noreturn void console_panic(const char *message);

#endif
