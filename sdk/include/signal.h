/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_SIGNAL_H
#define PHIPIA_SIGNAL_H

typedef void (*sighandler_t)(int);
typedef int sig_atomic_t;
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIGINT 2
#define SIGABRT 6
#define SIGFPE 8
#define SIGSEGV 11
#define SIGTERM 15
sighandler_t signal(int signal_number, sighandler_t handler);
int raise(int signal_number);

#endif
