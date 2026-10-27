/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* A thread that exits between the task directory scan and the read of its
 * stat file is dropped from the per-thread counter refresh, and the refresh
 * succeeds. The stub opens the victim thread's stat file, then makes the
 * victim exit and waits until the kernel removed it, so the real read gets
 * ESRCH. Only a stat line that does not parse gives EIO. When every thread
 * of a workload is gone, the refresh returns PACT_WORKLOAD_GONE. Perf event
 * creation is mocked; the threads, the child process and /proc are real. */
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

long fake_perf_syscall(long number, ...);
int fake_perf_ioctl(int fd, unsigned long request, ...);
static FILE *stub_fopen(const char *path, const char *mode);
static int stub_close(int fd);
#define syscall fake_perf_syscall
#define ioctl fake_perf_ioctl
#define fopen(path, mode) stub_fopen(path, mode)
#define close(fd) stub_close(fd)
#include "../src/pmu.c"
#undef syscall
#undef ioctl
#undef fopen
#undef close

pmu_platform_t g_pmu_platform;

/* Every fake perf descriptor: its thread and how often it was closed. */
static struct {
    pid_t tid;
    int fd;
    int closes;
} opened[64];
static size_t nr_opened;

/* The stub's behavior for /proc/<pid>/task/<tid>/stat. */
enum stat_stub {
    STAT_REAL,       /* real file; when armed, remove the victim after its open */
    STAT_TEXT,       /* a stat line from stat_text */
    STAT_EMPTY,      /* an empty file: the read ends without a line */
    STAT_READ_FAILS, /* a stream whose read fails with EISDIR */
};
static enum stat_stub stub_mode = STAT_REAL;
static const char *stat_text;
static char stat_buffer[256];
static pid_t victim_tid;
static pthread_t victim_thread;
static atomic_bool victim_release;
static atomic_int started_tid;
static bool remove_armed; /* the next open of the victim's stat removes it */
static bool victim_removed;

long fake_perf_syscall(long number, ...)
{
    assert(number == __NR_perf_event_open && nr_opened < 64);
    va_list args;
    va_start(args, number);
    (void)va_arg(args, struct perf_event_attr *);
    pid_t tid = va_arg(args, int);
    va_end(args);
    int fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    opened[nr_opened].tid = tid;
    opened[nr_opened].fd = fd;
    opened[nr_opened++].closes = 0;
    return fd;
}

int fake_perf_ioctl(int fd, unsigned long request, ...)
{
    if (request == PERF_EVENT_IOC_ID) {
        va_list args;
        va_start(args, request);
        uint64_t *id = va_arg(args, uint64_t *);
        *id = (uint64_t)fd;
        va_end(args);
    }
    return 0;
}

static int stub_close(int fd)
{
    for (size_t i = 0; i < nr_opened; i++) {
        if (opened[i].fd == fd) {
            opened[i].closes++;
        }
    }
    return close(fd);
}

/* Make the victim exit, reap it, and wait until the kernel released it:
 * its task directory is gone, so a read through an open stat file of the
 * victim returns ESRCH. */
static void remove_victim(void)
{
    char dir[96];
    snprintf(dir, sizeof(dir), "/proc/%d/task/%d", getpid(), victim_tid);
    atomic_store(&victim_release, true);
    assert(!pthread_join(victim_thread, NULL));
    struct stat st;
    for (int i = 0; stat(dir, &st) == 0; i++) {
        assert(i < 100000); /* 10 s */
        usleep(100);
    }
    victim_removed = true;
}

static FILE *stub_fopen(const char *path, const char *mode)
{
    if (strstr(path, "/task/") && strstr(path, "/stat")) {
        if (stub_mode == STAT_TEXT) {
            snprintf(stat_buffer, sizeof(stat_buffer), "%s", stat_text);
            return fmemopen(stat_buffer, strlen(stat_buffer), "r");
        }
        if (stub_mode == STAT_EMPTY) {
            return fopen("/dev/null", mode);
        }
        if (stub_mode == STAT_READ_FAILS) {
            return fopen("/proc/self", mode); /* a directory: read fails */
        }
    }
    FILE *fp = fopen(path, mode);
    char victim_path[96];
    snprintf(victim_path, sizeof(victim_path), "/proc/%d/task/%d/stat", getpid(), victim_tid);
    if (fp && remove_armed && strcmp(path, victim_path) == 0) {
        remove_armed = false;
        remove_victim();
    }
    return fp;
}

static void *victim(void *unused)
{
    (void)unused;
    atomic_store(&started_tid, syscall(SYS_gettid));
    while (!atomic_load(&victim_release)) {
        usleep(100);
    }
    return NULL;
}

static void start_victim(void)
{
    atomic_store(&victim_release, false);
    atomic_store(&started_tid, 0);
    victim_removed = false;
    assert(!pthread_create(&victim_thread, NULL, victim, NULL));
    while (!atomic_load(&started_tid)) {
        usleep(100);
    }
    victim_tid = atomic_load(&started_tid);
}

/* Each fake descriptor of `tid` was closed exactly `closes` times. */
static void check_closes(pid_t tid, int closes)
{
    size_t seen = 0;
    for (size_t i = 0; i < nr_opened; i++) {
        if (opened[i].tid == tid) {
            assert(opened[i].closes == closes);
            seen++;
        }
    }
    assert(seen == CORE_EVENT_COUNT + 1);
}

int main(void)
{
    set_log_level(LOG_LEVEL_ERROR);
    pid_t self = getpid();
    uint64_t start = 0;

    /* A live thread gives its start time. */
    assert(thread_start_time(self, self, &start) == 0 && start > 0);

    /* The victim exits after the open of its stat file and before the
     * read: the read gets ESRCH, and the result is THREAD_GONE. */
    start_victim();
    remove_armed = true;
    assert(thread_start_time(self, victim_tid, &start) == THREAD_GONE);
    assert(victim_removed);
    /* Its stat file is gone now: also THREAD_GONE (ENOENT at the open). */
    assert(thread_start_time(self, victim_tid, &start) == THREAD_GONE);

    /* A zombie or dead state is THREAD_GONE. A line that does not parse,
     * or no line, is EIO. A failed read keeps its own errno. */
    stub_mode = STAT_TEXT;
    stat_text = "7 (a b) Z 1 2 3";
    assert(thread_start_time(self, self, &start) == THREAD_GONE);
    stat_text = "7 (a) X";
    assert(thread_start_time(self, self, &start) == THREAD_GONE);
    stat_text = "no closing parenthesis";
    errno = 0;
    assert(thread_start_time(self, self, &start) == -1 && errno == EIO);
    stat_text = "7 (a) S 1 2 3";
    errno = 0;
    assert(thread_start_time(self, self, &start) == -1 && errno == EIO);
    stat_text = "7 (a) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 4242 19";
    assert(thread_start_time(self, self, &start) == 0 && start == 4242);
    stub_mode = STAT_EMPTY;
    errno = 0;
    assert(thread_start_time(self, self, &start) == -1 && errno == EIO);
    stub_mode = STAT_READ_FAILS;
    errno = 0;
    assert(thread_start_time(self, self, &start) == -1 && errno == EISDIR);
    stub_mode = STAT_REAL;

    /* Refresh: both threads are covered. Then the victim exits between the
     * scan and its stat read. The refresh succeeds, the victim leaves the
     * thread list (and with it the LLC-miss sum and the sample count), and
     * each of its groups closes once. The main thread keeps its groups. */
    start_victim();
    pact_workload_t wl = {.target_pid = self};
    assert(setup_workload_counting_events(&wl) == 0);
    assert(wl.nr_threads == 2);
    assert(workload_covers_tid(&wl, self) && workload_covers_tid(&wl, victim_tid));
    check_closes(victim_tid, 0);
    pid_t gone = victim_tid;
    remove_armed = true;
    assert(setup_workload_counting_events(&wl) == 0);
    assert(victim_removed);
    fprintf(stderr, "threads after the exit=%zu\n", wl.nr_threads);
    assert(wl.nr_threads == 1);
    assert(workload_covers_tid(&wl, self) && !workload_covers_tid(&wl, gone));
    check_closes(gone, 1);
    check_closes(self, 0);

    /* The next refresh keeps the main thread and closes nothing more. */
    assert(setup_workload_counting_events(&wl) == 0 && wl.nr_threads == 1);
    check_closes(gone, 1);
    check_closes(self, 0);
    cleanup_workload_counting_events(&wl);
    check_closes(self, 1);

    /* Every thread gone: a child process exits and stays unreaped (its one
     * thread is a zombie). The refresh returns PACT_WORKLOAD_GONE and closes
     * the old groups once. After the reap, the task directory is gone: also
     * PACT_WORKLOAD_GONE. */
    int gate[2];
    assert(!pipe(gate));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        char byte;
        close(gate[1]);
        _exit(read(gate[0], &byte, 1) == 1 ? 0 : 1);
    }
    close(gate[0]);
    pact_workload_t cw = {.target_pid = child};
    assert(setup_workload_counting_events(&cw) == 0 && cw.nr_threads == 1);
    check_closes(child, 0);
    assert(write(gate[1], "x", 1) == 1);
    close(gate[1]);
    siginfo_t info;
    assert(!waitid(P_PID, child, &info, WEXITED | WNOWAIT));
    assert(setup_workload_counting_events(&cw) == PACT_WORKLOAD_GONE);
    assert(cw.nr_threads == 0 && !cw.threads);
    check_closes(child, 1);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(setup_workload_counting_events(&cw) == PACT_WORKLOAD_GONE);
    check_closes(child, 1);

    /* A stat line that does not parse stays an error, not a gone workload. */
    pact_workload_t sw = {.target_pid = self};
    stub_mode = STAT_TEXT;
    stat_text = "no closing parenthesis";
    errno = 0;
    assert(setup_workload_counting_events(&sw) == -1 && errno == EIO);
    stub_mode = STAT_REAL;
    cleanup_workload_counting_events(&sw);
    return 0;
}
