/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_SETJMP_H
#define PHIPIA_SETJMP_H

typedef unsigned long jmp_buf[8];

int setjmp(jmp_buf environment);
_Noreturn void longjmp(jmp_buf environment, int value);

#endif
