/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_ASSERT_H
#define PHIPIA_ASSERT_H

void __phipia_assert(const char *expression, const char *file, int line);
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression) ((expression) ? (void)0 : \
    __phipia_assert(#expression, __FILE__, __LINE__))
#endif

#endif
