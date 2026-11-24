/* Guest semaphores for the cooperative thread model (see guest_threads.h).
   recomp_start.py inlines this file after guest_threads.h. The RSemaphore
   handle word at offset 0 of the guest object indexes this table. */

#ifndef GUEST_SYNC_H
#define GUEST_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#define GUEST_SEM_COUNT 32
#define SEM_BAD_WAIT (-2)

struct guest_sem {
    uint32_t handle;
    int32_t count;
};

static struct guest_sem guest_sems[GUEST_SEM_COUNT];
/* RHandleBase::Close also accepts thread handles. Keep sync handles in
   a separate range so closing a mutex cannot close a thread instead. */
static uint32_t guest_sem_next_handle = UINT32_C(0x10000000);

static inline struct guest_sem *guest_sem_find(uint32_t handle) {
    for (int i = 0; i < GUEST_SEM_COUNT; i++)
        if (guest_sems[i].handle == handle && handle != 0) return &guest_sems[i];
    return NULL;
}

/* RSemaphore::CreateLocal(TInt aCount, TOwnerType): stores the handle word in *this. */
static inline int sem_create_local(uint32_t *stack, uint32_t self, int32_t count) {
    if (count < 0) return -6; /* KErrArgument */
    for (int i = 0; i < GUEST_SEM_COUNT; i++) {
        if (guest_sems[i].handle == 0) {
            guest_sems[i].handle = guest_sem_next_handle++;
            guest_sems[i].count = count;
            guest_write(stack, self, guest_sems[i].handle);
            return 0;
        }
    }
    return -4; /* KErrNoMemory */
}

static inline bool sem_close(uint32_t handle) {
    struct guest_sem *s = guest_sem_find(handle);
    if (!s) return false;
    s->handle = 0;
    /* An already blocked call must retry and report the invalid handle. */
    for (unsigned i = 0; i < GUEST_THREAD_COUNT; i++) {
        struct guest_thread *thread = &guest_threads[i];
        if (thread->state == BLOCKED && thread->waiting_sem == handle)
            thread->state = RUNNABLE;
    }
    return true;
}

/* RSemaphore::Signal(TInt aCount). */
static inline int sem_signal(uint32_t handle, int32_t n) {
    struct guest_sem *s = guest_sem_find(handle);
    if (!s || n < 0) return -8; /* KErrBadHandle */
    if (n > INT32_MAX - s->count) return -22; /* KErrOverflow */
    s->count += n;
    for (unsigned i = 0; n && i < GUEST_THREAD_COUNT; i++) {
        struct guest_thread *thread = &guest_threads[i];
        if (thread->state == BLOCKED && thread->waiting_sem == handle)
            thread->state = RUNNABLE;
    }
    return 0;
}

/* Wait succeeds only after taking the requested units. A worker can retry
   the import after a signal; a main-thread or nested call keeps its frame
   while running other workers. */
static inline int sem_wait(uint32_t *stack, struct guest_thread *ctx, uint32_t handle, int32_t n) {
    struct guest_sem *s = guest_sem_find(handle);
    if (!s || n < 0) return SEM_BAD_WAIT;
    while (s->count < n) {
        if (is_guest_thread(ctx)) {
            ctx->waiting_sem = handle;
            ctx->state = BLOCKED;
            return THREAD_YIELD;
        }
        unsigned i = 0;
        while (i < GUEST_THREAD_COUNT && guest_threads[i].state != RUNNABLE) i++;
        if (i == GUEST_THREAD_COUNT) {
            fprintf(stderr, "guest semaphore deadlock: handle %u count %d need %d lr=0x%08x\n",
                    handle, s->count, n, ctx->r[14]);
            return 3;
        }
        int status = threads_run_ready(stack);
        if (status) return status;
        s = guest_sem_find(handle);
        if (!s) return SEM_BAD_WAIT;
    }
    s->count -= n;
    ctx->waiting_sem = 0;
    return 0;
}

/* RMutex is a binary semaphore in this model; recursive holds are not expected. */
static inline int mutex_create_local(uint32_t *stack, uint32_t self) {
    return sem_create_local(stack, self, 1);
}

#endif
