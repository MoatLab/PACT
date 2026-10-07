/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#define main pact_runtime_main
#include "../src/pact.c"
#undef main

/* Link-time syscall wrappers can change these outside the compiler's call graph. */
static volatile bool interrupt_read, interrupt_write, enqueue_during_ack;
static pact_context_t *race_ctx;
static _Atomic bool published;
ssize_t __real_read(int fd, void *buffer, size_t count);
ssize_t __real_write(int fd, const void *buffer, size_t count);

ssize_t __wrap_write(int fd, const void *buffer, size_t count)
{
    if (interrupt_write) {
        interrupt_write = false;
        errno = EINTR;
        return -1;
    }
    return __real_write(fd, buffer, count);
}

ssize_t __wrap_read(int fd, void *buffer, size_t count)
{
    if (interrupt_read) {
        interrupt_read = false;
        errno = EINTR;
        return -1;
    }
    ssize_t result = __real_read(fd, buffer, count);
    if (enqueue_during_ack && result == sizeof(uint64_t)) {
        enqueue_during_ack = false;
        /* Producer publishes after eventfd read but before pending is reset. */
        atomic_store(&published, true);
        wake_migration_worker(race_ctx);
    }
    return result;
}

/* Fortified libc headers can redirect read to __read_chk at compile time. */
ssize_t __wrap___read_chk(int fd, void *buffer, size_t count, size_t capacity)
{
    assert(count <= capacity);
    return __wrap_read(fd, buffer, count);
}

int main(void)
{
    pact_context_t ctx = {.migration_wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
    assert(ctx.migration_wake_fd >= 0);
    atomic_init(&ctx.migration_wake_pending, false);
    atomic_init(&ctx.migration_error, 0);
    race_ctx = &ctx;
    interrupt_write = true;
    for (int i = 0; i < 1000; i++) {
        wake_migration_worker(&ctx);
    }
    uint64_t notifications;
    assert(read(ctx.migration_wake_fd, &notifications, sizeof(notifications)) ==
           sizeof(notifications));
    assert(notifications == 1 && !interrupt_write);
    assert(write(ctx.migration_wake_fd, &notifications, sizeof(notifications)) ==
           sizeof(notifications));
    enqueue_during_ack = true;
    interrupt_read = true;
    consume_migration_wakeup(&ctx);
    assert(!interrupt_read && atomic_load(&published));
    assert(!atomic_load(&ctx.migration_wake_pending));
    wake_migration_worker(&ctx);
    assert(read(ctx.migration_wake_fd, &notifications, sizeof(notifications)) ==
           sizeof(notifications));
    assert(notifications == 1 && !atomic_load(&ctx.migration_error));
    /* Restore and consume the token before checking an empty read. */
    assert(write(ctx.migration_wake_fd, &notifications, sizeof(notifications)) ==
           sizeof(notifications));
    consume_migration_wakeup(&ctx);
    consume_migration_wakeup(&ctx);
    assert(!atomic_load(&ctx.migration_error));
    close(ctx.migration_wake_fd);
    wake_migration_worker(&ctx);
    assert(atomic_load(&ctx.migration_error) == EBADF);
    atomic_store(&ctx.migration_error, 0);
    consume_migration_wakeup(&ctx);
    assert(atomic_load(&ctx.migration_error) == EBADF);
    puts("migration wakeup: coalescing, acknowledgement race, EINTR and errors passed");
    return 0;
}
