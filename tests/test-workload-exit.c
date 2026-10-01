/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
#include <assert.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/pact.h"
#include "../src/utils.h"

int main(void)
{
    int gate[2];
    assert(!pipe(gate));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(gate[1]);
        char byte;
        _exit(read(gate[0], &byte, 1) == 1 ? 0 : 1);
    }
    close(gate[0]);
    int pidfd = syscall(SYS_pidfd_open, child, 0);
    assert(pidfd >= 0);
    pact_workload_t wl = {.target_pid = child};
    pact_context_t ctx = {.workload = &wl};
#ifndef PUBLIC_BASELINE
    ctx.target_pidfd = pidfd;
#endif
    assert(!pact_check_all_targets_exited(&ctx));
    assert(write(gate[1], "x", 1) == 1);
    close(gate[1]);
    siginfo_t info;
    assert(!waitid(P_PID, child, &info, WEXITED | WNOWAIT));
    assert(!kill(child, 0)); /* The unreaped process still has a PID. */
    bool zombie_exited = pact_check_all_targets_exited(&ctx);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(pact_check_all_targets_exited(&ctx));
    close(pidfd);
    assert(zombie_exited);
    return 0;
}
