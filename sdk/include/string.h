/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_STRING_H
#define PHIPIA_STRING_H

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t length);
void *memmove(void *destination, const void *source, size_t length);
void *memset(void *destination, int value, size_t length);
int memcmp(const void *left, const void *right, size_t length);
void *memchr(const void *memory, int value, size_t length);
size_t strlen(const char *text);
size_t strnlen(const char *text, size_t limit);
char *strcpy(char *destination, const char *source);
char *strncpy(char *destination, const char *source, size_t length);
char *strcat(char *destination, const char *source);
char *strncat(char *destination, const char *source, size_t length);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t length);
int strcoll(const char *left, const char *right);
size_t strspn(const char *text, const char *accept);
size_t strcspn(const char *text, const char *reject);
char *strchr(const char *text, int character);
char *strrchr(const char *text, int character);
char *strpbrk(const char *text, const char *accept);
char *strstr(const char *text, const char *needle);
char *strerror(int error);

#endif
