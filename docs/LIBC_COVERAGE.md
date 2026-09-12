<!-- SPDX-License-Identifier: GPL-3.0-only -->

# C runtime coverage

The Phipia SDK is a scoped freestanding runtime, not a complete ISO C, POSIX,
musl, or glibc implementation.

| Area | Available | Important limits |
| --- | --- | --- |
| Startup and errors | `main(argc, argv, envp)`, `exit`, `_Exit`, `atexit`, thread-local `errno`, `assert` | No signals or process environment mutation. |
| Memory | `malloc`, `calloc`, `realloc`, `free` | Private anonymous mappings; no shared memory or `mmap` compatibility API. |
| Bytes and strings | `memcpy`, `memmove`, `memset`, comparisons, bounded string/search/token helpers | ASCII-oriented locale. |
| Conversion | integer conversions, `qsort`, `bsearch`, deterministic abort/exit paths | No wide-character conversion. |
| Formatted I/O | buffered `FILE`, stdin/stdout/stderr, `printf`/`fprintf`/`snprintf`, character/line/block I/O, seek/tell/flush | Formatting is the documented SDK subset; no locale formatting. |
| Files | `open`-style flags through `fopen`, read/write/seek/stat, remove/rename, mkdir, directory enumeration, truncate, sync and free-space native calls | FAT32 ASCII 8.3 paths, 16 MiB files, application-rooted Data. |
| Time | realtime and monotonic `clock_gettime`, `time`, `gmtime[_r]`, UTC `localtime[_r]`, `nanosleep`, native absolute sleep | RTC resolution is one second; no timezone database, pre-1970 conversion, or anti-rollback policy. `strftime` and `mktime` remain unsupported. |
| Events | native multi-handle wait, timer create/set/cancel, explicit timeout results | At most eight wait items; supported readiness is defined per typed handle. |
| Threads | `pthread_create/join/exit/self/equal`, attributes, mutex, once, local-exec `_Thread_local` | One process, single core, at most eight threads; no cancellation API or condition variables yet. |
| Math | scalar functions required by Lua/SQLite and compiler support | Userspace x87/SSE is preserved; kernel code never uses it. Coverage is not a full libm claim. |
| Networking | Phipia DNS, TCP, UDP, deadlines, address query, cancellation, bounded BearSSL TLS 1.2 and strict HTTPS GET | Typed native handles; no POSIX socket namespace, general Internet/security suite, HTTP/2, IPv6, or mutable host trust store. |
| Graphics/input | window, xRGB surface, bounded damage, event read/wait, pointer capture | One native window per process in ABI v1. |
| Locale/signals | C locale stubs and the narrow signal surface needed by ports | No asynchronous Unix signal delivery. |
| Absent | `fork`, `exec`, pipes, Unix IPC, runtime `dlopen`/`dlsym`, terminal ioctls, users/groups | Manifest-authenticated startup DSOs are supported, but ports requiring a hosted dynamic-loader API still need deliberate native services. |
