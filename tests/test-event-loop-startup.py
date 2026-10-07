#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 MoatLab, Virginia Tech.
"""Inject startup failures into the real event loop and main shutdown path.

Replace only main's privileged setup with a minimal fixture. Preserve its
event-loop call, destruction, exit status, and marker code verbatim. Thread
creation/join are simulated; this is not a live-worker or hardware test.
"""
import errno
import os
from pathlib import Path
import re
import subprocess
import tempfile

src = Path(__file__).resolve().parents[1] / "src"
if not (src / "build-info.h").exists():
    subprocess.run(["make", "-C", str(src), "build-info.h"], check=True)
runtime = (src / "pact.c").read_text()
main = runtime.index("int main(int argc, char *argv[])")
call = re.search(r"^    (?:bool \w+ = )?run_pact_event_loop\(g_pact\);$",
                 runtime[main:], re.M)
assert call, "Cannot locate main's event-loop call"
fixture = r'''
#include <assert.h>
#include "pact.h"
static void fixture_setup(int argc, char **argv);
#define main runtime_main
'''
fixture += runtime[:main]
fixture += "int main(int argc, char *argv[])\n{\n    fixture_setup(argc, argv);\n"
fixture += runtime[main + call.start():]
fixture += r'''
#undef main
static int fault, allocations, batch_allocations, creates, joins, running_at_destroy;
static bool armed;
static void *(*pending_start)(void *);
static void *pending_arg;
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size)
{
    if (armed && fault >= 10 && ++batch_allocations == fault - 9) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_malloc(size);
}
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size)
{
    if (armed && fault > 0 && fault < 10 && ++allocations == fault) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_calloc(count, size);
}
void __real_free(void *ptr);
void __wrap_free(void *ptr)
{
    if (ptr && ptr == g_pact_ctx) {
        running_at_destroy = g_pact_ctx->migration_thread_running;
    }
    __real_free(ptr);
}
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*start)(void *), void *arg)
{
    (void)thread; (void)attr;
    pending_start = start;
    pending_arg = arg;
    creates++;
    errno = 0;
    return fault == 0 ? EAGAIN : 0;
}
int __wrap_pthread_join(pthread_t thread, void **result)
{
    (void)thread; (void)result;
    assert(fault != 0 && creates == 1);
    joins++;
    /* Run the actual worker after cleanup has set its stop flag. */
    pending_start(pending_arg);
    return 0;
}
static void fixture_setup(int argc, char **argv)
{
    assert(argc == 3);
    fault = atoi(argv[1]);
    g_pact_ctx = calloc(1, sizeof(*g_pact_ctx));
    assert(g_pact_ctx);
    g_pact_ctx->workload = calloc(1, sizeof(*g_pact_ctx->workload));
    assert(g_pact_ctx->workload);
    g_pact_ctx->workload->pac_table = pac_table_init();
    g_pact_ctx->workload->binning = calloc(1, sizeof(*g_pact_ctx->workload->binning));
    assert(g_pact_ctx->workload->pac_table && g_pact_ctx->workload->binning);
    g_pact_ctx->monitor_cpu = -1;
    g_pact_ctx->migration_cpu = -1;
    g_pact_ctx->target_pidfd = -1;
    g_pact_ctx->max_migrations_per_cycle = 32;
    g_pact_ctx->tsc_freq_hz = 1000000000;
    g_pact_ctx->sampling_failed = fault == -2;
    set_log_level(LOG_LEVEL_ERROR);
    pact_signal_set_crash_marker_path(argv[2]);
    armed = true;
}
int main(int argc, char **argv)
{
    alarm(5);
    int result = runtime_main(argc, argv);
    printf("FIXTURE creates=%d joins=%d running_at_destroy=%d\n",
           creates, joins, running_at_destroy);
    return result;
}
'''

with tempfile.TemporaryDirectory(prefix="pact-startup-") as directory:
    work = Path(directory)
    source = work / "fixture.c"
    binary = work / "fixture"
    source.write_text(fixture)
    logging = os.environ.get("PACT_TEST_LOGGING") == "1"
    excluded = ("pact.c",) if logging else ("pact.c", "logging.c")
    sources = sorted(str(p) for p in src.glob("*.c") if p.name not in excluded)
    command = [os.environ.get("CC", "gcc"), "-Wall", "-Wextra", "-O1", "-g",
               "-D_GNU_SOURCE", "-I" + str(src), "-fsanitize=address,undefined",
               str(source), *sources, "-Wl,--wrap=calloc", "-Wl,--wrap=malloc", "-Wl,--wrap=free",
               "-Wl,--wrap=pthread_create", "-Wl,--wrap=pthread_join",
               "-lm", "-lnuma", "-lpthread", "-o", str(binary)]
    if logging:
        command.insert(1, "-DPACT_ENABLE_LOGGING")
    subprocess.run(command, check=True)
    failures = []
    for fault in (0, 1, 2, 3, 4, 5, 10, 11, 12, 13, -1, -2):
        marker = work / f"marker-{fault}"
        run = subprocess.run([str(binary), str(fault), str(marker)],
                             capture_output=True, text=True, timeout=10)
        success = fault == -1
        errors = []
        if run.returncode != (0 if success else 1):
            errors.append(f"exit={run.returncode}")
        if marker.exists() != success:
            errors.append(f"marker_exists={marker.exists()}")
        if success and marker.exists() and marker.read_text() != "ok signal=0\n":
            errors.append("incorrect clean marker")
        if "running_at_destroy=0" not in run.stdout:
            errors.append("incorrect worker state at destruction")
        expected_creates = int(fault <= 0)
        expected_joins = int(fault < 0)
        if f"creates={expected_creates} joins={expected_joins}" not in run.stdout:
            errors.append("incorrect worker ownership")
        if fault == 0 and os.strerror(errno.EAGAIN) not in run.stderr:
            errors.append("missing pthread error")
        if "ERROR: AddressSanitizer" in run.stderr or "runtime error:" in run.stderr:
            errors.append("sanitizer failure")
        if errors:
            failures.append((fault, errors, run.stdout, run.stderr))
        print(f"fault={fault}: {'FAIL ' + repr(errors) if errors else 'PASS'}")
    assert not failures, failures
