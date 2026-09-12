/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  Phipia thread backend addition for SDL 2.32.10. This file is distributed
  under the same zlib license as SDL.
*/

#include "../../SDL_internal.h"

#ifdef SDL_THREAD_PHIPIA

#include "../SDL_systhread.h"

#include <phipia/runtime.h>

#include <errno.h>
#include <pthread.h>

struct SDL_mutex
{
    pthread_mutex_t native;
    SDL_threadID owner;
    Uint32 recursion;
};

struct SDL_semaphore
{
    volatile Uint32 count;
};

struct SDL_cond
{
    volatile Uint32 sequence;
};

static _Thread_local SDL_TLSData *phipia_tls_data;

static uint64_t PHIPIA_TimeoutDeadline(Uint32 timeout)
{
    const uint64_t now = phipia_monotonic_ns();
    const uint64_t delta = (uint64_t)timeout * UINT64_C(1000000);
    return delta > UINT64_MAX - now ? UINT64_MAX : now + delta;
}

static long PHIPIA_FutexWait(volatile Uint32 *word, Uint32 expected,
                             uint64_t deadline)
{
    const struct phipia_futex_request request = {
        sizeof(request), PHIPIA_ABI_VERSION,
        (uint64_t)(uintptr_t)word, deadline, expected, 0U
    };
    return phipia_syscall1(PHIPIA_SYS_FUTEX_WAIT,
        (uint64_t)(uintptr_t)&request);
}

static void PHIPIA_FutexWake(volatile Uint32 *word, Uint32 count)
{
    const struct phipia_futex_request request = {
        sizeof(request), PHIPIA_ABI_VERSION,
        (uint64_t)(uintptr_t)word, 0U, 0U, count
    };
    (void)phipia_syscall1(PHIPIA_SYS_FUTEX_WAKE,
        (uint64_t)(uintptr_t)&request);
}

static void *PHIPIA_RunThread(void *argument)
{
    SDL_RunThread((SDL_Thread *)argument);
    return NULL;
}

int SDL_SYS_CreateThread(SDL_Thread *thread)
{
    pthread_attr_t attributes;
    SDL_bool attributes_initialized = SDL_FALSE;
    int status;

    status = pthread_attr_init(&attributes);
    if (status == 0) {
        attributes_initialized = SDL_TRUE;
    }
    if (status == 0 && thread->stacksize != 0U) {
        if (thread->stacksize > UINT32_MAX) {
            status = EINVAL;
        } else {
            status = pthread_attr_setstacksize(&attributes,
                (Uint32)thread->stacksize);
        }
    }
    if (status == 0) {
        status = pthread_create(&thread->handle, &attributes,
            PHIPIA_RunThread, thread);
    }
    if (attributes_initialized) {
        (void)pthread_attr_destroy(&attributes);
    }
    return status == 0 ? 0 : SDL_SetError("Phipia thread creation failed: %d",
        status);
}

void SDL_SYS_SetupThread(const char *name)
{
    (void)name;
}

SDL_threadID SDL_ThreadID(void)
{
    const pthread_t current = pthread_self();
    return current == UINT64_MAX ? UINT64_MAX : (SDL_threadID)current + 1U;
}

int SDL_SYS_SetThreadPriority(SDL_ThreadPriority priority)
{
    (void)priority;
    return 0;
}

void SDL_SYS_WaitThread(SDL_Thread *thread)
{
    (void)pthread_join(thread->handle, NULL);
}

void SDL_SYS_DetachThread(SDL_Thread *thread)
{
    /* Phipia v1 has no detached-thread reclamation primitive. Joining here
       preserves resource ownership instead of leaking a live generation. */
    (void)pthread_join(thread->handle, NULL);
}

void SDL_SYS_InitTLSData(void)
{
}

SDL_TLSData *SDL_SYS_GetTLSData(void)
{
    return phipia_tls_data;
}

int SDL_SYS_SetTLSData(SDL_TLSData *data)
{
    phipia_tls_data = data;
    return 0;
}

void SDL_SYS_QuitTLSData(void)
{
    phipia_tls_data = NULL;
}

SDL_mutex *SDL_CreateMutex(void)
{
    SDL_mutex *mutex = (SDL_mutex *)SDL_calloc(1, sizeof(*mutex));

    if (mutex == NULL) {
        SDL_OutOfMemory();
        return NULL;
    }
    if (pthread_mutex_init(&mutex->native, NULL) != 0) {
        SDL_free(mutex);
        SDL_SetError("Phipia mutex initialization failed");
        return NULL;
    }
    return mutex;
}

void SDL_DestroyMutex(SDL_mutex *mutex)
{
    if (mutex != NULL) {
        if (pthread_mutex_destroy(&mutex->native) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_SYSTEM,
                "Phipia destroyed a locked SDL mutex");
        }
        SDL_free(mutex);
    }
}

int SDL_LockMutex(SDL_mutex *mutex)
{
    const SDL_threadID current = SDL_ThreadID();

    if (mutex == NULL) {
        return 0;
    }
    if (mutex->owner == current) {
        if (mutex->recursion == UINT32_MAX) {
            return SDL_SetError("Phipia SDL mutex recursion overflow");
        }
        ++mutex->recursion;
        return 0;
    }
    if (pthread_mutex_lock(&mutex->native) != 0) {
        return SDL_SetError("Phipia mutex lock failed");
    }
    mutex->owner = current;
    mutex->recursion = 0U;
    return 0;
}

int SDL_TryLockMutex(SDL_mutex *mutex)
{
    const SDL_threadID current = SDL_ThreadID();
    int status;

    if (mutex == NULL) {
        return 0;
    }
    if (mutex->owner == current) {
        if (mutex->recursion == UINT32_MAX) {
            return SDL_SetError("Phipia SDL mutex recursion overflow");
        }
        ++mutex->recursion;
        return 0;
    }
    status = pthread_mutex_trylock(&mutex->native);
    if (status == EBUSY) {
        return SDL_MUTEX_TIMEDOUT;
    }
    if (status != 0) {
        return SDL_SetError("Phipia mutex try-lock failed: %d", status);
    }
    mutex->owner = current;
    mutex->recursion = 0U;
    return 0;
}

int SDL_UnlockMutex(SDL_mutex *mutex)
{
    if (mutex == NULL) {
        return 0;
    }
    if (mutex->owner != SDL_ThreadID()) {
        return SDL_SetError("Phipia SDL mutex is not owned by this thread");
    }
    if (mutex->recursion != 0U) {
        --mutex->recursion;
        return 0;
    }
    mutex->owner = 0U;
    return pthread_mutex_unlock(&mutex->native) == 0 ? 0 :
        SDL_SetError("Phipia mutex unlock failed");
}

SDL_sem *SDL_CreateSemaphore(Uint32 initial_value)
{
    SDL_sem *semaphore = (SDL_sem *)SDL_calloc(1, sizeof(*semaphore));

    if (semaphore == NULL) {
        SDL_OutOfMemory();
        return NULL;
    }
    semaphore->count = initial_value;
    return semaphore;
}

void SDL_DestroySemaphore(SDL_sem *semaphore)
{
    SDL_free(semaphore);
}

int SDL_SemTryWait(SDL_sem *semaphore)
{
    Uint32 observed;

    if (semaphore == NULL) {
        return SDL_InvalidParamError("semaphore");
    }
    observed = __atomic_load_n(&semaphore->count, __ATOMIC_ACQUIRE);
    while (observed != 0U) {
        if (__atomic_compare_exchange_n(&semaphore->count, &observed,
                observed - 1U, SDL_FALSE, __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE)) {
            return 0;
        }
    }
    return SDL_MUTEX_TIMEDOUT;
}

int SDL_SemWaitTimeout(SDL_sem *semaphore, Uint32 timeout)
{
    const uint64_t deadline = timeout == SDL_MUTEX_MAXWAIT ? 0U :
        PHIPIA_TimeoutDeadline(timeout);

    if (semaphore == NULL) {
        return SDL_InvalidParamError("semaphore");
    }
    for (;;) {
        const int acquired = SDL_SemTryWait(semaphore);
        long result;

        if (acquired == 0) {
            return 0;
        }
        if (timeout == 0U) {
            return SDL_MUTEX_TIMEDOUT;
        }
        result = PHIPIA_FutexWait(&semaphore->count, 0U, deadline);
        if (result == -PHIPIA_ETIMEDOUT) {
            return SDL_MUTEX_TIMEDOUT;
        }
        if (result < 0 && result != -PHIPIA_EAGAIN) {
            return SDL_SetError("Phipia semaphore wait failed: %ld", result);
        }
    }
}

int SDL_SemWait(SDL_sem *semaphore)
{
    return SDL_SemWaitTimeout(semaphore, SDL_MUTEX_MAXWAIT);
}

Uint32 SDL_SemValue(SDL_sem *semaphore)
{
    return semaphore == NULL ? 0U :
        __atomic_load_n(&semaphore->count, __ATOMIC_ACQUIRE);
}

int SDL_SemPost(SDL_sem *semaphore)
{
    Uint32 observed;

    if (semaphore == NULL) {
        return SDL_InvalidParamError("semaphore");
    }
    observed = __atomic_load_n(&semaphore->count, __ATOMIC_ACQUIRE);
    for (;;) {
        if (observed == UINT32_MAX) {
            return SDL_SetError("Phipia semaphore count overflow");
        }
        if (__atomic_compare_exchange_n(&semaphore->count, &observed,
                observed + 1U, SDL_FALSE, __ATOMIC_RELEASE,
                __ATOMIC_ACQUIRE)) {
            PHIPIA_FutexWake(&semaphore->count, 1U);
            return 0;
        }
    }
}

SDL_cond *SDL_CreateCond(void)
{
    SDL_cond *condition = (SDL_cond *)SDL_calloc(1, sizeof(*condition));
    if (condition == NULL) {
        SDL_OutOfMemory();
    }
    return condition;
}

void SDL_DestroyCond(SDL_cond *condition)
{
    SDL_free(condition);
}

int SDL_CondSignal(SDL_cond *condition)
{
    if (condition == NULL) {
        return SDL_InvalidParamError("condition");
    }
    (void)__atomic_add_fetch(&condition->sequence, 1U, __ATOMIC_RELEASE);
    PHIPIA_FutexWake(&condition->sequence, 1U);
    return 0;
}

int SDL_CondBroadcast(SDL_cond *condition)
{
    if (condition == NULL) {
        return SDL_InvalidParamError("condition");
    }
    (void)__atomic_add_fetch(&condition->sequence, 1U, __ATOMIC_RELEASE);
    PHIPIA_FutexWake(&condition->sequence, UINT32_MAX);
    return 0;
}

int SDL_CondWaitTimeout(SDL_cond *condition, SDL_mutex *mutex, Uint32 timeout)
{
    Uint32 sequence;
    uint64_t deadline;
    long result;

    if (condition == NULL || mutex == NULL) {
        return SDL_InvalidParamError("condition or mutex");
    }
    sequence = __atomic_load_n(&condition->sequence, __ATOMIC_ACQUIRE);
    deadline = timeout == SDL_MUTEX_MAXWAIT ? 0U :
        PHIPIA_TimeoutDeadline(timeout);
    if (SDL_UnlockMutex(mutex) != 0) {
        return -1;
    }
    result = PHIPIA_FutexWait(&condition->sequence, sequence, deadline);
    if (SDL_LockMutex(mutex) != 0) {
        return -1;
    }
    if (result == -PHIPIA_ETIMEDOUT) {
        return SDL_MUTEX_TIMEDOUT;
    }
    return result < 0 && result != -PHIPIA_EAGAIN ?
        SDL_SetError("Phipia condition wait failed: %ld", result) : 0;
}

int SDL_CondWait(SDL_cond *condition, SDL_mutex *mutex)
{
    return SDL_CondWaitTimeout(condition, mutex, SDL_MUTEX_MAXWAIT);
}

#endif /* SDL_THREAD_PHIPIA */
