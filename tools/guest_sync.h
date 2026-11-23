/* Guest semaphores for the cooperative thread model (see guest_threads.h).
   recomp_start.py inlines this file after guest_threads.h. The RSemaphore
   handle word at offset 0 of the guest object indexes this table. */

#ifndef GUEST_SYNC_H
#define GUEST_SYNC_H

#include <stdint.h>

#define GUEST_SEM_COUNT 32

struct guest_sem {
    uint32_t handle;
    int32_t count;
};

static struct guest_sem guest_sems[GUEST_SEM_COUNT];
static uint32_t guest_sem_next_handle = 1;

static inline struct guest_sem *guest_sem_find(uint32_t handle) {
    for (int i = 0; i < GUEST_SEM_COUNT; i++)
        if (guest_sems[i].handle == handle && handle != 0) return &guest_sems[i];
    return NULL;
}

/* RSemaphore::CreateLocal(TInt aCount, TOwnerType): stores the handle word in *this. */
static inline int sem_create_local(uint32_t *stack, uint32_t self, int32_t count) {
    if (count < 0) return -22; /* KErrArgument */
    for (int i = 0; i < GUEST_SEM_COUNT; i++) {
        if (guest_sems[i].handle == 0) {
            guest_sems[i].handle = guest_sem_next_handle++;
            guest_sems[i].count = count;
            guest_write(stack, self, guest_sems[i].handle);
            return 0;
        }
    }
    return -12; /* KErrNoMemory */
}

/* RSemaphore::Signal(TInt aCount). */
static inline int sem_signal(uint32_t handle, int32_t n) {
    struct guest_sem *s = guest_sem_find(handle);
    if (!s || n < 0) return -8; /* KErrBadHandle */
    s->count += n;
    return 0;
}

/* RSemaphore::Wait(TInt aCount). Blocking waits are not scheduled yet: an
   attempt to take more than is available stops with a diagnostic instead of
   returning as if the semaphore had been signalled. */
static inline int sem_wait(uint32_t handle, int32_t n) {
    struct guest_sem *s = guest_sem_find(handle);
    if (!s || n < 0) return -8;
    if (s->count < n) {
        fprintf(stderr, "guest semaphore wait would block: handle %u count %d need %d\n",
                handle, s->count, n);
        exit(3);
    }
    s->count -= n;
    return 0;
}

/* RMutex is a binary semaphore in this model: Wait takes the single unit,
   Signal returns it. Recursive holds are not expected in the traced paths;
   a second Wait while held stops with the same diagnostic as sem_wait. */
static inline int mutex_create_local(uint32_t *stack, uint32_t self) {
    return sem_create_local(stack, self, 1);
}

#endif
