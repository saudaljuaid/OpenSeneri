/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PTHREAD_H
#define PHIPIA_PTHREAD_H

#include <stdint.h>

typedef uint64_t pthread_t;
typedef struct { volatile uint32_t value; } pthread_mutex_t;
typedef struct { volatile uint32_t state; } pthread_once_t;
typedef struct { uint32_t stack_size; } pthread_attr_t;

#define PTHREAD_MUTEX_INITIALIZER {0U}
#define PTHREAD_ONCE_INIT {0U}

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
    void *(*entry)(void *), void *argument);
int pthread_join(pthread_t thread, void **result);
_Noreturn void pthread_exit(void *result);
pthread_t pthread_self(void);
int pthread_equal(pthread_t left, pthread_t right);
int pthread_mutex_init(pthread_mutex_t *mutex, const void *attributes);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_once(pthread_once_t *once, void (*function)(void));
int pthread_attr_init(pthread_attr_t *attributes);
int pthread_attr_destroy(pthread_attr_t *attributes);
int pthread_attr_setstacksize(pthread_attr_t *attributes, uint32_t size);

#endif
