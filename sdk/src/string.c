/* SPDX-License-Identifier: GPL-3.0-only */
#include <string.h>

#include <errno.h>
#include <stdint.h>

void *memcpy(void *destination, const void *source, size_t length)
{
    unsigned char *output = destination;
    const unsigned char *input = source;

    for (size_t index = 0U; index < length; ++index) {
        output[index] = input[index];
    }
    return destination;
}

void *memmove(void *destination, const void *source, size_t length)
{
    unsigned char *output = destination;
    const unsigned char *input = source;

    if (output <= input) {
        return memcpy(destination, source, length);
    }
    while (length != 0U) {
        --length;
        output[length] = input[length];
    }
    return destination;
}

void *memset(void *destination, int value, size_t length)
{
    unsigned char *output = destination;

    for (size_t index = 0U; index < length; ++index) {
        output[index] = (unsigned char)value;
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t length)
{
    const unsigned char *first = left;
    const unsigned char *second = right;

    for (size_t index = 0U; index < length; ++index) {
        if (first[index] != second[index]) {
            return first[index] < second[index] ? -1 : 1;
        }
    }
    return 0;
}

void *memchr(const void *memory, int value, size_t length)
{
    const unsigned char *bytes = memory;

    for (size_t index = 0U; index < length; ++index) {
        if (bytes[index] == (unsigned char)value) {
            return (void *)(uintptr_t)(bytes + index);
        }
    }
    return NULL;
}

size_t strlen(const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

size_t strnlen(const char *text, size_t limit)
{
    size_t length = 0U;

    while (length < limit && text[length] != '\0') {
        ++length;
    }
    return length;
}

char *strcpy(char *destination, const char *source)
{
    return memcpy(destination, source, strlen(source) + 1U);
}

char *strncpy(char *destination, const char *source, size_t length)
{
    size_t index = 0U;

    while (index < length && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    while (index < length) {
        destination[index++] = '\0';
    }
    return destination;
}

char *strcat(char *destination, const char *source)
{
    (void)strcpy(destination + strlen(destination), source);
    return destination;
}

char *strncat(char *destination, const char *source, size_t length)
{
    char *cursor = destination + strlen(destination);
    size_t count = strnlen(source, length);

    (void)memcpy(cursor, source, count);
    cursor[count] = '\0';
    return destination;
}

int strcmp(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char first = (unsigned char)left[index];
        const unsigned char second = (unsigned char)right[index];

        if (first != second || first == 0U) {
            return first - second;
        }
    }
    return 0;
}

int strcoll(const char *left, const char *right)
{
    return strcmp(left, right);
}

size_t strspn(const char *text, const char *accept)
{
    size_t length = 0U;

    while (text[length] != '\0' && strchr(accept, text[length]) != NULL) {
        ++length;
    }
    return length;
}

size_t strcspn(const char *text, const char *reject)
{
    size_t length = 0U;

    while (text[length] != '\0' && strchr(reject, text[length]) == NULL) {
        ++length;
    }
    return length;
}

char *strchr(const char *text, int character)
{
    do {
        if (*text == (char)character) {
            return (char *)(uintptr_t)text;
        }
    } while (*text++ != '\0');
    return NULL;
}

char *strrchr(const char *text, int character)
{
    const char *result = NULL;

    do {
        if (*text == (char)character) {
            result = text;
        }
    } while (*text++ != '\0');
    return (char *)(uintptr_t)result;
}

char *strpbrk(const char *text, const char *accept)
{
    while (*text != '\0') {
        if (strchr(accept, *text) != NULL) {
            return (char *)(uintptr_t)text;
        }
        ++text;
    }
    return NULL;
}

char *strstr(const char *text, const char *needle)
{
    const size_t length = strlen(needle);

    if (length == 0U) {
        return (char *)(uintptr_t)text;
    }
    while (*text != '\0') {
        if (strncmp(text, needle, length) == 0) {
            return (char *)(uintptr_t)text;
        }
        ++text;
    }
    return NULL;
}

char *strerror(int error)
{
    switch (error) {
    case 0: return "success";
    case EPERM: return "operation not permitted";
    case ENOENT: return "not found";
    case EIO: return "input/output error";
    case EBADF: return "bad handle";
    case EAGAIN: return "try again";
    case ENOMEM: return "out of memory";
    case EACCES: return "permission denied";
    case EFAULT: return "invalid address";
    case EBUSY: return "busy";
    case EEXIST: return "already exists";
    case EINVAL: return "invalid argument";
    case ENOSPC: return "no space";
    case EROFS: return "read-only filesystem";
    case ETIMEDOUT: return "timed out";
    case ECANCELED: return "cancelled";
    case ESTALE: return "stale handle";
    default: return "unknown error";
    }
}
