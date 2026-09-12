/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define FILE_READ 1U
#define FILE_WRITE 2U
#define FILE_APPEND 4U
#define FILE_CONSOLE 8U
#define FILE_STATIC 16U
#define FILE_BUFFER_DIRTY 32U

struct phipia_FILE {
    phipia_handle_t handle;
    unsigned int flags;
    unsigned int error;
    unsigned int eof;
    unsigned char buffer[BUFSIZ];
    size_t position;
    size_t length;
    int pushed;
    volatile uint32_t lock;
};

static struct phipia_FILE input_stream = {
    0U, FILE_READ | FILE_CONSOLE | FILE_STATIC, 0U, 0U, {0}, 0U, 0U, -1, 0U
};
static struct phipia_FILE output_stream = {
    0U, FILE_WRITE | FILE_CONSOLE | FILE_STATIC, 0U, 0U, {0}, 0U, 0U, -1, 0U
};
static struct phipia_FILE error_stream = {
    0U, FILE_WRITE | FILE_CONSOLE | FILE_STATIC, 0U, 0U, {0}, 0U, 0U, -1, 0U
};
FILE *stdin = &input_stream;
FILE *stdout = &output_stream;
FILE *stderr = &error_stream;

static int flush_locked(FILE *stream)
{
    size_t offset = 0U;

    if (stream->length == 0U ||
        (stream->flags & (FILE_WRITE | FILE_BUFFER_DIRTY)) !=
            (FILE_WRITE | FILE_BUFFER_DIRTY)) {
        return 0;
    }
    while (offset < stream->length) {
        long result;

        if ((stream->flags & FILE_CONSOLE) != 0U) {
            result = phipia_syscall2(PHIPIA_SYS_CONSOLE_WRITE,
                (uint64_t)(uintptr_t)(stream->buffer + offset),
                stream->length - offset);
        } else {
            result = phipia_file_write(stream->handle,
                stream->buffer + offset, stream->length - offset);
        }
        if (result <= 0) {
            stream->error = 1U;
            if (result < 0) {
                errno = (int)-result;
            }
            return EOF;
        }
        offset += (size_t)result;
    }
    stream->length = 0U;
    stream->position = 0U;
    stream->flags &= ~FILE_BUFFER_DIRTY;
    return 0;
}

int fflush(FILE *stream)
{
    int result = 0;

    if (stream == NULL) {
        if (fflush(stdout) == EOF || fflush(stderr) == EOF) {
            result = EOF;
        }
        return result;
    }
    phipia_runtime_lock(&stream->lock);
    result = flush_locked(stream);
    phipia_runtime_unlock(&stream->lock);
    return result;
}

static int mode_flags(const char *mode, uint32_t *open_flags)
{
    int flags = 0;
    int update;

    if (mode == NULL || mode[0] == '\0') {
        return 0;
    }
    update = strchr(mode, '+') != NULL;
    if (mode[0] == 'r') {
        flags = FILE_READ | (update ? FILE_WRITE : 0);
        *open_flags = PHIPIA_OPEN_READ |
            (update ? PHIPIA_OPEN_WRITE : 0U);
    } else if (mode[0] == 'w') {
        flags = FILE_WRITE | (update ? FILE_READ : 0);
        *open_flags = PHIPIA_OPEN_WRITE | PHIPIA_OPEN_CREATE |
            PHIPIA_OPEN_TRUNCATE | (update ? PHIPIA_OPEN_READ : 0U);
    } else if (mode[0] == 'a') {
        flags = FILE_WRITE | FILE_APPEND | (update ? FILE_READ : 0);
        *open_flags = PHIPIA_OPEN_WRITE | PHIPIA_OPEN_CREATE |
            (update ? PHIPIA_OPEN_READ : 0U);
    }
    return flags;
}

FILE *fopen(const char *path, const char *mode)
{
    struct phipia_runtime_path parsed;
    uint32_t open_flags = 0U;
    const int flags = mode_flags(mode, &open_flags);
    long handle;
    FILE *stream;

    if (flags == 0 || phipia_runtime_path(path, &parsed) != 0) {
        errno = EINVAL;
        return NULL;
    }
    handle = phipia_file_open(parsed.volume, parsed.text, open_flags);
    if (handle < 0) {
        errno = (int)-handle;
        return NULL;
    }
    stream = calloc(1U, sizeof(*stream));
    if (stream == NULL) {
        (void)phipia_handle_close((phipia_handle_t)handle);
        return NULL;
    }
    stream->handle = (phipia_handle_t)handle;
    stream->flags = (unsigned int)flags;
    stream->pushed = -1;
    if ((flags & FILE_APPEND) != 0 &&
        phipia_file_seek(stream->handle, 0, PHIPIA_SEEK_END) < 0) {
        (void)fclose(stream);
        return NULL;
    }
    return stream;
}

FILE *freopen(const char *path, const char *mode, FILE *stream)
{
    FILE *replacement;

    if (stream == NULL || (stream->flags & FILE_STATIC) != 0U) {
        errno = EINVAL;
        return NULL;
    }
    replacement = fopen(path, mode);
    if (replacement == NULL) {
        (void)fclose(stream);
        return NULL;
    }
    (void)fflush(stream);
    (void)phipia_handle_close(stream->handle);
    *stream = *replacement;
    free(replacement);
    return stream;
}

int fclose(FILE *stream)
{
    int result;

    if (stream == NULL || (stream->flags & FILE_STATIC) != 0U) {
        errno = EINVAL;
        return EOF;
    }
    result = fflush(stream);
    if (phipia_handle_close(stream->handle) < 0) {
        result = EOF;
    }
    free(stream);
    return result;
}

static size_t read_locked(void *pointer, size_t bytes, FILE *stream)
{
    unsigned char *output = pointer;
    size_t completed = 0U;

    if ((stream->flags & FILE_READ) == 0U) {
        stream->error = 1U;
        errno = EBADF;
        return 0U;
    }
    if ((stream->flags & FILE_BUFFER_DIRTY) != 0U &&
        flush_locked(stream) == EOF) {
        return 0U;
    }
    if (stream->pushed >= 0 && bytes != 0U) {
        output[completed++] = (unsigned char)stream->pushed;
        stream->pushed = -1;
    }
    if ((stream->flags & FILE_CONSOLE) != 0U && completed < bytes) {
        const long result = phipia_console_read(output + completed,
            bytes - completed);

        if (result < 0) {
            stream->error = 1U;
            errno = (int)-result;
            return completed;
        }
        return completed + (size_t)result;
    }
    while (completed < bytes) {
        if (stream->position < stream->length) {
            size_t chunk = stream->length - stream->position;

            if (chunk > bytes - completed) {
                chunk = bytes - completed;
            }
            (void)memcpy(output + completed,
                stream->buffer + stream->position, chunk);
            stream->position += chunk;
            completed += chunk;
            continue;
        }
        const long result = phipia_file_read(stream->handle, stream->buffer,
            sizeof(stream->buffer));

        if (result < 0) {
            stream->error = 1U;
            errno = (int)-result;
            break;
        }
        if (result == 0) {
            stream->eof = 1U;
            break;
        }
        stream->position = 0U;
        stream->length = (size_t)result;
    }
    return completed;
}

size_t fread(void *pointer, size_t size, size_t count, FILE *stream)
{
    size_t bytes;
    size_t completed;

    if (stream == NULL || pointer == NULL ||
        (size != 0U && count > SIZE_MAX / size)) {
        errno = EINVAL;
        return 0U;
    }
    bytes = size * count;
    phipia_runtime_lock(&stream->lock);
    completed = read_locked(pointer, bytes, stream);
    phipia_runtime_unlock(&stream->lock);
    return size == 0U ? 0U : completed / size;
}

static size_t write_locked(const void *pointer, size_t bytes, FILE *stream)
{
    const unsigned char *input = pointer;
    size_t completed = 0U;

    if ((stream->flags & FILE_WRITE) == 0U) {
        stream->error = 1U;
        errno = EBADF;
        return 0U;
    }
    if ((stream->flags & FILE_BUFFER_DIRTY) == 0U &&
        stream->length != 0U) {
        const size_t unread = stream->length - stream->position;
        long seek_result = 0;

        if ((stream->flags & FILE_CONSOLE) == 0U && unread != 0U) {
            seek_result = phipia_file_seek(stream->handle, -(int64_t)unread,
                PHIPIA_SEEK_CURRENT);
        }
        if (seek_result < 0) {
            stream->error = 1U;
            errno = (int)-seek_result;
            return 0U;
        }
        stream->position = 0U;
        stream->length = 0U;
    }
    stream->flags |= FILE_BUFFER_DIRTY;
    while (completed < bytes) {
        size_t chunk = sizeof(stream->buffer) - stream->length;

        if (chunk > bytes - completed) {
            chunk = bytes - completed;
        }
        (void)memcpy(stream->buffer + stream->length,
            input + completed, chunk);
        stream->length += chunk;
        completed += chunk;
        if (stream->length == sizeof(stream->buffer) ||
            ((stream->flags & FILE_CONSOLE) != 0U &&
                memchr(input + completed - chunk, '\n', chunk) != NULL)) {
            if (flush_locked(stream) == EOF) {
                break;
            }
        }
    }
    return completed;
}

size_t fwrite(const void *pointer, size_t size, size_t count, FILE *stream)
{
    size_t bytes;
    size_t completed;

    if (stream == NULL || pointer == NULL ||
        (size != 0U && count > SIZE_MAX / size)) {
        errno = EINVAL;
        return 0U;
    }
    bytes = size * count;
    phipia_runtime_lock(&stream->lock);
    completed = write_locked(pointer, bytes, stream);
    phipia_runtime_unlock(&stream->lock);
    return size == 0U ? 0U : completed / size;
}

int fseek(FILE *stream, long offset, int origin)
{
    long result;

    if (stream == NULL || (stream->flags & FILE_CONSOLE) != 0U ||
        origin < SEEK_SET || origin > SEEK_END) {
        errno = EINVAL;
        return -1;
    }
    phipia_runtime_lock(&stream->lock);
    if (flush_locked(stream) == EOF) {
        phipia_runtime_unlock(&stream->lock);
        return -1;
    }
    if ((stream->flags & FILE_READ) != 0U && origin == SEEK_CUR) {
        offset -= (long)(stream->length - stream->position);
    }
    stream->position = 0U;
    stream->length = 0U;
    stream->pushed = -1;
    stream->eof = 0U;
    result = phipia_file_seek(stream->handle, offset, (uint32_t)origin);
    phipia_runtime_unlock(&stream->lock);
    if (result < 0) {
        errno = (int)-result;
        return -1;
    }
    return 0;
}

long ftell(FILE *stream)
{
    long result;

    if (stream == NULL || (stream->flags & FILE_CONSOLE) != 0U) {
        errno = EINVAL;
        return -1L;
    }
    phipia_runtime_lock(&stream->lock);
    result = phipia_file_seek(stream->handle, 0, PHIPIA_SEEK_CURRENT);
    if (result >= 0 && (stream->flags & FILE_BUFFER_DIRTY) != 0U) {
        result += (long)stream->length;
    } else if (result >= 0 && (stream->flags & FILE_READ) != 0U) {
        result -= (long)(stream->length - stream->position);
    }
    phipia_runtime_unlock(&stream->lock);
    if (result < 0) {
        errno = (int)-result;
        return -1L;
    }
    return result;
}

void rewind(FILE *stream) { (void)fseek(stream, 0L, SEEK_SET); clearerr(stream); }
int fgetpos(FILE *stream, fpos_t *position)
{
    const long result = ftell(stream);
    if (result < 0 || position == NULL) return -1;
    *position = result;
    return 0;
}
int fsetpos(FILE *stream, const fpos_t *position)
{
    return position == NULL ? -1 : fseek(stream, (long)*position, SEEK_SET);
}
int feof(FILE *stream) { return stream == NULL ? 0 : (int)stream->eof; }
int ferror(FILE *stream) { return stream == NULL ? 0 : (int)stream->error; }
void clearerr(FILE *stream)
{
    if (stream != NULL) { stream->error = 0U; stream->eof = 0U; }
}

int fgetc(FILE *stream)
{
    unsigned char result;
    return fread(&result, 1U, 1U, stream) == 1U ? (int)result : EOF;
}
int getc(FILE *stream) { return fgetc(stream); }
int getchar(void) { return fgetc(stdin); }
char *fgets(char *text, int count, FILE *stream)
{
    int index = 0;
    if (text == NULL || count <= 0) return NULL;
    while (index + 1 < count) {
        const int character = fgetc(stream);
        if (character == EOF) break;
        text[index++] = (char)character;
        if (character == '\n') break;
    }
    text[index] = '\0';
    return index == 0 ? NULL : text;
}
int ungetc(int character, FILE *stream)
{
    if (stream == NULL || character == EOF || stream->pushed >= 0) return EOF;
    stream->pushed = (unsigned char)character;
    stream->eof = 0U;
    return stream->pushed;
}
int fputc(int character, FILE *stream)
{
    const unsigned char value = (unsigned char)character;
    return fwrite(&value, 1U, 1U, stream) == 1U ? value : EOF;
}
int putc(int character, FILE *stream) { return fputc(character, stream); }
int putchar(int character) { return fputc(character, stdout); }
int fputs(const char *text, FILE *stream)
{
    const size_t length = strlen(text);
    return fwrite(text, 1U, length, stream) == length ? 0 : EOF;
}
int puts(const char *text)
{
    return fputs(text, stdout) == 0 && fputc('\n', stdout) != EOF ? 0 : EOF;
}

struct format_destination {
    FILE *stream;
    char *buffer;
    size_t capacity;
    size_t length;
};

static void emit(struct format_destination *output, char value)
{
    if (output->stream != NULL) {
        if (fputc(value, output->stream) == EOF) output->capacity = 0U;
    } else if (output->capacity != 0U && output->length + 1U < output->capacity) {
        output->buffer[output->length] = value;
    }
    ++output->length;
}

static void emit_text(struct format_destination *output, const char *text,
    size_t limit)
{
    for (size_t index = 0U; text[index] != '\0' && index < limit; ++index) {
        emit(output, text[index]);
    }
}

static void emit_unsigned(struct format_destination *output,
    unsigned long long value, unsigned int base, int width, int uppercase,
    int negative)
{
    char digits[32];
    int count = 0;
    const char *alphabet = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    do { digits[count++] = alphabet[value % base]; value /= base; } while (value != 0U);
    if (negative) ++count;
    while (count < width) { emit(output, ' '); ++count; }
    if (negative) emit(output, '-');
    while (count-- > negative) emit(output, digits[count - negative]);
}

static void emit_double(struct format_destination *output, double value,
    int precision)
{
    unsigned long long whole;
    if (value < 0.0) { emit(output, '-'); value = -value; }
    if (value > (double)ULLONG_MAX) { emit_text(output, "inf", SIZE_MAX); return; }
    whole = (unsigned long long)value;
    emit_unsigned(output, whole, 10U, 0, 0, 0);
    emit(output, '.');
    if (precision < 0) precision = 6;
    for (int index = 0; index < precision; ++index) {
        value = (value - (double)whole) * 10.0;
        const unsigned int digit = (unsigned int)value;
        emit(output, (char)('0' + digit));
        value -= digit;
        whole = 0U;
    }
}

static int format(struct format_destination *output, const char *format,
    va_list arguments)
{
    while (*format != '\0') {
        int width = 0;
        int precision = -1;
        int length = 0;
        char specifier;
        if (*format != '%') { emit(output, *format++); continue; }
        ++format;
        if (*format == '%') { emit(output, *format++); continue; }
        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format++ - '0');
        }
        if (*format == '.') {
            precision = 0; ++format;
            while (*format >= '0' && *format <= '9') {
                precision = precision * 10 + (*format++ - '0');
            }
        }
        if (*format == 'l') { length = 1; ++format; if (*format == 'l') { length = 2; ++format; } }
        else if (*format == 'z') { length = 2; ++format; }
        specifier = *format == '\0' ? '\0' : *format++;
        if (specifier == 's') {
            const char *text = va_arg(arguments, const char *);
            emit_text(output, text == NULL ? "(null)" : text,
                precision < 0 ? SIZE_MAX : (size_t)precision);
        } else if (specifier == 'c') {
            emit(output, (char)va_arg(arguments, int));
        } else if (specifier == 'd' || specifier == 'i') {
            long long value = length == 2 ? va_arg(arguments, long long) :
                (length == 1 ? va_arg(arguments, long) : va_arg(arguments, int));
            const int negative = value < 0;
            const unsigned long long magnitude = negative ?
                (unsigned long long)(-(value + 1)) + 1U : (unsigned long long)value;
            emit_unsigned(output, magnitude, 10U, width, 0, negative);
        } else if (specifier == 'u' || specifier == 'x' || specifier == 'X') {
            unsigned long long value = length == 2 ?
                va_arg(arguments, unsigned long long) : (length == 1 ?
                    va_arg(arguments, unsigned long) : va_arg(arguments, unsigned int));
            emit_unsigned(output, value, specifier == 'u' ? 10U : 16U,
                width, specifier == 'X', 0);
        } else if (specifier == 'p') {
            emit_text(output, "0x", SIZE_MAX);
            emit_unsigned(output, (uintptr_t)va_arg(arguments, void *), 16U,
                0, 0, 0);
        } else if (specifier == 'f' || specifier == 'g') {
            emit_double(output, va_arg(arguments, double), precision);
        } else if (specifier == '\0') {
            break;
        } else {
            emit(output, '%'); emit(output, specifier);
        }
    }
    if (output->stream == NULL && output->capacity != 0U) {
        const size_t end = output->length < output->capacity ?
            output->length : output->capacity - 1U;
        output->buffer[end] = '\0';
    }
    return output->length > INT_MAX ? -1 : (int)output->length;
}

int vfprintf(FILE *stream, const char *text, va_list arguments)
{
    struct format_destination output = {stream, NULL, SIZE_MAX, 0U};
    return format(&output, text, arguments);
}
int fprintf(FILE *stream, const char *text, ...)
{
    va_list arguments; int result;
    va_start(arguments, text); result = vfprintf(stream, text, arguments); va_end(arguments);
    return result;
}
int printf(const char *text, ...)
{
    va_list arguments; int result;
    va_start(arguments, text); result = vfprintf(stdout, text, arguments); va_end(arguments);
    return result;
}
int vsnprintf(char *output, size_t capacity, const char *text, va_list arguments)
{
    struct format_destination destination = {NULL, output, capacity, 0U};
    if (capacity != 0U && output == NULL) { errno = EINVAL; return -1; }
    return format(&destination, text, arguments);
}
int snprintf(char *output, size_t capacity, const char *text, ...)
{
    va_list arguments; int result;
    va_start(arguments, text); result = vsnprintf(output, capacity, text, arguments); va_end(arguments);
    return result;
}
int sprintf(char *output, const char *text, ...)
{
    va_list arguments; int result;
    va_start(arguments, text); result = vsnprintf(output, SIZE_MAX, text, arguments); va_end(arguments);
    return result;
}

int remove(const char *path)
{
    struct phipia_runtime_path parsed;
    struct phipia_path request;
    long result;
    if (phipia_runtime_path(path, &parsed) != 0) return -1;
    request.address = (uint64_t)(uintptr_t)parsed.text;
    request.length = (uint32_t)parsed.length;
    request.volume = parsed.volume; request.reserved = 0U;
    result = phipia_syscall2(PHIPIA_SYS_PATH_UNLINK,
        (uint64_t)(uintptr_t)&request, 0U);
    return phipia_result(result);
}
int rename(const char *source, const char *destination)
{
    struct phipia_runtime_path from, to;
    struct phipia_rename_request request;
    if (phipia_runtime_path(source, &from) != 0 ||
        phipia_runtime_path(destination, &to) != 0) return -1;
    request.size = sizeof(request); request.version = PHIPIA_ABI_VERSION;
    request.source = (struct phipia_path){(uint64_t)(uintptr_t)from.text,
        (uint32_t)from.length, from.volume, 0U};
    request.destination = (struct phipia_path){(uint64_t)(uintptr_t)to.text,
        (uint32_t)to.length, to.volume, 0U};
    request.flags = 0U; request.reserved = 0U;
    return phipia_result(phipia_syscall1(PHIPIA_SYS_PATH_RENAME,
        (uint64_t)(uintptr_t)&request));
}
int setvbuf(FILE *stream, char *buffer, int mode, size_t size)
{
    (void)stream; (void)buffer; (void)size;
    if (mode < _IONBF || mode > _IOFBF) { errno = EINVAL; return -1; }
    return 0;
}
void setbuf(FILE *stream, char *buffer) { (void)setvbuf(stream, buffer, buffer == NULL ? _IONBF : _IOFBF, BUFSIZ); }
void perror(const char *prefix)
{
    if (prefix != NULL && *prefix != '\0') fprintf(stderr, "%s: ", prefix);
    fprintf(stderr, "%s\n", strerror(errno));
}
FILE *tmpfile(void) { errno = ENOTSUP; return NULL; }
char *tmpnam(char *name) { (void)name; errno = ENOTSUP; return NULL; }
