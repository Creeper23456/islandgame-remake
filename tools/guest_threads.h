/* Cooperative guest threads for the standalone AppUi profile.
   recomp_start.py inlines this file into the generated translation unit
   after window_gc.h/fbs_bitmap.h. All state lives in the same guest address
   space; only the register file and continuation are per thread. */

#ifndef GUEST_THREADS_H
#define GUEST_THREADS_H

#include <stdint.h>

/* Thread states, matching the shared profile contract. */
enum { CREATED, RUNNABLE, RUNNING, BLOCKED, DEAD };

/* Returned by guest_run() to the scheduler when a blocking import yields. */
#define THREAD_YIELD (-1)

#define GUEST_THREAD_COUNT 16
#define GUEST_THREAD_SENTINEL UINT32_C(0xff00ff00)

struct guest_cleanup {
    struct { uint32_t pointer, operation, kind; } items[64];
    unsigned depth;
    uint32_t handler;
};

struct guest_thread {
    uint32_t handle;
    uint32_t entry, arg, stack_size;
    int state;
    uint32_t target;
    uint32_t r[16];
    uint32_t saved_sp;
    struct guest_cleanup cleanup;
    uint32_t scheduler, active_queue;
    int scheduler_running;
    uint32_t waiting_sem;
};

static struct guest_thread guest_threads[GUEST_THREAD_COUNT];
static uint32_t guest_thread_next_handle = 1;
static struct guest_thread *current_worker;

/* Provided by recomp_start.py's generated preamble. */
static uint32_t heap_try_alloc(uint32_t size);

/* Provided by the generated program (recomp_start.py); the exact definition
   this header assumes is:

       static int guest_run(struct guest_thread *ctx, uint32_t *stack);

   It runs ctx from ctx->target, leaving the next continuation in
   ctx->target and the stack pointer in ctx->r[13]. A blocking import ends
   the run with `return THREAD_YIELD;`; a thread that returns to
   GUEST_THREAD_SENTINEL (or otherwise finishes) ends with return 0. */
static int guest_run(struct guest_thread *ctx, uint32_t *stack);

static inline struct guest_thread *guest_thread_find(uint32_t handle) {
    if (handle) {
        for (uint32_t i = 0; i < GUEST_THREAD_COUNT; i++)
            if (guest_threads[i].handle == handle) return &guest_threads[i];
    }
    return 0;
}

/* True when ctx is a created guest thread (not the main thread or a nested call context). */
static inline int is_guest_thread(const struct guest_thread *ctx) {
    for (uint32_t i = 0; i < GUEST_THREAD_COUNT; i++)
        if (ctx == &guest_threads[i]) return 1;
    return 0;
}

/* RThread::RequestComplete: update the caller's pointer and the target's request. */
static inline int thread_request_complete(uint32_t *stack, uint32_t handle, uint32_t ref, uint32_t reason) {
    struct guest_thread *ctx = handle == UINT32_C(0xffff8000) ? current_worker : guest_thread_find(handle);
    if (!ctx || ctx->state == DEAD) return 0;
    uint32_t status = guest_read(stack, ref);
    if (!status) return 1;
    guest_write(stack, ref, 0);
    guest_write(stack, status, reason);
    guest_write(stack, status + 4, guest_read(stack, status + 4) & ~2u);
    return 1;
}

/* RThread::Create (euser ordinals 1158/1159). Writes the new handle to the
   caller's RThread object at out_handle_ptr and returns KErrNone or a
   negative error. A created thread stays CREATED until thread_resume(). */
static inline int thread_create(uint32_t *stack, uint32_t name_desc, uint32_t entry,
                                uint32_t stack_size, uint32_t heap_or_zero, uint32_t arg,
                                uint32_t owner_type, uint32_t out_handle_ptr) {
    struct guest_thread *ctx = 0;
    uint32_t base, top;
    (void)name_desc;
    (void)heap_or_zero;
    (void)owner_type;
    if (!out_handle_ptr) return -22; /* KErrArgument */
    guest_write(stack, out_handle_ptr, 0); /* invalid until the slot exists */
    if (!entry || stack_size < 512) return -22; /* KErrArgument: minimum stack */
    for (uint32_t i = 0; i < GUEST_THREAD_COUNT; i++) {
        if (!guest_threads[i].handle) {
            ctx = &guest_threads[i];
            break;
        }
    }
    if (!ctx) return -12; /* KErrNoMemory */
    base = heap_try_alloc(stack_size);
    if (!base) return -12; /* KErrNoMemory */
    *ctx = (struct guest_thread){0};
    top = base + ((stack_size + 7) & ~UINT32_C(7));
    ctx->entry = entry;
    ctx->arg = arg;
    ctx->stack_size = stack_size;
    ctx->state = CREATED;
    ctx->target = entry;
    ctx->r[0] = arg;
    ctx->r[13] = top;
    ctx->r[14] = GUEST_THREAD_SENTINEL;
    ctx->saved_sp = top;
    if (!guest_thread_next_handle) guest_thread_next_handle = 1;
    ctx->handle = guest_thread_next_handle++;
    guest_write(stack, out_handle_ptr, ctx->handle);
    return 0;
}

/* RThread::Resume (euser ordinal 1795). */
static inline int thread_resume(uint32_t *stack, uint32_t handle) {
    struct guest_thread *ctx = guest_thread_find(handle);
    (void)stack;
    if (!ctx) return -8; /* KErrBadHandle */
    if (ctx->state == CREATED) ctx->state = RUNNABLE;
    return 0;
}

/* RThread::Kill (euser ordinal 1154); the handle stays valid for Close(). */
static inline int thread_kill(uint32_t *stack, uint32_t handle) {
    struct guest_thread *ctx = guest_thread_find(handle);
    (void)stack;
    if (!ctx) return -8; /* KErrBadHandle */
    ctx->state = DEAD;
    return 0;
}

/* RHandleBase::Close (euser ordinal 120); releases the table slot. */
static inline int thread_close(uint32_t *stack, uint32_t handle) {
    struct guest_thread *ctx = guest_thread_find(handle);
    (void)stack;
    if (!ctx) return -8; /* KErrBadHandle */
    ctx->state = DEAD;
    ctx->handle = 0;
    return 0;
}

/* Scheduler hook: run every RUNNABLE thread once, in table order, until it
   yields (stays RUNNABLE for the next pass) or finishes (becomes DEAD). */
static inline int threads_run_ready(uint32_t *stack) {
    for (uint32_t i = 0; i < GUEST_THREAD_COUNT; i++) {
        struct guest_thread *ctx = &guest_threads[i];
        int status;
        if (ctx->state != RUNNABLE) continue;
        ctx->state = RUNNING;
        struct guest_thread *previous = current_worker;
        current_worker = ctx;
        status = guest_run(ctx, stack);
        current_worker = previous;
        ctx->saved_sp = ctx->r[13];
        if (status == THREAD_YIELD) {
            if (ctx->state == RUNNING) ctx->state = RUNNABLE;
        } else {
            ctx->state = DEAD;
            if (status) return status;
        }
    }
    return 0;
}

#endif
