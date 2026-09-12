/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_STDLIB_H
#define PHIPIA_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
_Noreturn void abort(void);
_Noreturn void exit(int status);
int atexit(void (*function)(void));
char *getenv(const char *name);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);
long long strtoll(const char *text, char **end, int base);
unsigned long long strtoull(const char *text, char **end, int base);
double strtod(const char *text, char **end);
int atoi(const char *text);
int abs(int value);
long labs(long value);
void qsort(void *base, size_t count, size_t size,
    int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
    int (*compare)(const void *, const void *));
int rand(void);
void srand(unsigned int seed);
int system(const char *command);

#endif
