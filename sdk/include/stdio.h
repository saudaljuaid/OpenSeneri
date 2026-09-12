/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_STDIO_H
#define PHIPIA_STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define EOF (-1)
#define BUFSIZ 1024
#define FILENAME_MAX 128
#define L_tmpnam 32
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IONBF 0
#define _IOLBF 1
#define _IOFBF 2

typedef int64_t fpos_t;
typedef struct phipia_FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);
int fflush(FILE *stream);
size_t fread(void *pointer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *pointer, size_t size, size_t count, FILE *stream);
int fseek(FILE *stream, long offset, int origin);
long ftell(FILE *stream);
void rewind(FILE *stream);
int fgetpos(FILE *stream, fpos_t *position);
int fsetpos(FILE *stream, const fpos_t *position);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
int getchar(void);
char *fgets(char *text, int count, FILE *stream);
int ungetc(int character, FILE *stream);
int fputc(int character, FILE *stream);
int putc(int character, FILE *stream);
int putchar(int character);
int fputs(const char *text, FILE *stream);
int puts(const char *text);
int fprintf(FILE *stream, const char *format, ...);
int printf(const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list arguments);
int snprintf(char *output, size_t capacity, const char *format, ...);
int vsnprintf(char *output, size_t capacity, const char *format,
    va_list arguments);
int sprintf(char *output, const char *format, ...);
int remove(const char *path);
int rename(const char *source, const char *destination);
int setvbuf(FILE *stream, char *buffer, int mode, size_t size);
void setbuf(FILE *stream, char *buffer);
void perror(const char *prefix);
FILE *tmpfile(void);
char *tmpnam(char *name);

#endif
