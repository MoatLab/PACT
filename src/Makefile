# Makefile for the PACT runtime.

CC      = gcc
CFLAGS  = -Wall -Wextra -O3 -g -D_GNU_SOURCE -I. -MMD -MP
LDFLAGS = -lm -lnuma -lpthread

TARGET  = pact

# Optional diagnostics build: `make DIAG=1` defines PACT_DIAG and adds
# diag.c, which provide --dump-pac-table, --dump-pac-interval,
# --no-promote-wl and --cat-trace.
ifeq ($(DIAG),1)
CFLAGS += -DPACT_DIAG
DIAG_SOURCES = diag.c
endif

# All sources except logging.c, which is an optional build (its symbols are
# no-ops via logging.h unless PACT_ENABLE_LOGGING is set; see `make logging`),
# and diag.c (see DIAG above).
SOURCES = $(filter-out logging.c diag.c,$(wildcard *.c)) $(DIAG_SOURCES)
OBJECTS = $(SOURCES:.c=.o)
DEPS    = $(OBJECTS:.o=.d)

.PHONY: all clean logging format check-format check-style FORCE

all: $(TARGET)

# usage.c prints the version string from the generated build-info.h.
# Checked every run; the file (and usage.o) only refresh when the git
# version actually changes, so incremental builds stay incremental.
build-info.h: FORCE
	@GIT=$$(git describe --always --dirty --tags 2>/dev/null || echo unknown); \
	if ! grep -qsF "\"$$GIT\"" $@; then \
	    printf '#define PACT_GIT_VERSION "%s"\n#define PACT_BUILD_DATE "%s"\n' \
	        "$$GIT" "$$(date -u +%Y-%m-%dT%H:%M:%SZ)" > $@; \
	fi

FORCE:

usage.o: build-info.h

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

# Rebuild everything when CFLAGS change (e.g. after `make logging`, whose
# -DPACT_ENABLE_LOGGING objects must not be reused by a plain build).
.buildflags: FORCE
	@echo '$(CFLAGS)' | cmp -s - $@ 2>/dev/null || echo '$(CFLAGS)' > $@

%.o: %.c .buildflags
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

# Optional build with file logging enabled.
logging:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -DPACT_ENABLE_LOGGING" \
	    SOURCES="$(filter-out diag.c,$(wildcard *.c)) $(DIAG_SOURCES)"

clean:
	rm -f *.o *.d $(TARGET) build-info.h .buildflags

# Style checks (see CODING_STYLE.md).
# check-style: grep-based brace-style check, no dependencies.
# check-format: full clang-format check. Prefer clang-format-18 (the
# version CI pins, so local and CI agree), fall back to the unversioned
# binary, or override with CLANG_FORMAT=<binary>. The check also covers the
# C files in tests/ and run/tools/. Those directories have no .clang-format,
# so the style file is passed explicitly.
CLANG_FORMAT ?= $(shell command -v clang-format-18 || command -v clang-format)
CLANG_FORMAT_VER = $(shell $(CLANG_FORMAT) --version 2>/dev/null | grep -oE '[0-9]+' | head -1)
FMT_SRCS = $(filter-out khashl.h minicoro.h build-info.h,$(wildcard *.c *.h)) \
	$(wildcard ../tests/*.c ../tests/*.h ../run/tools/*.c ../run/tools/*.h)

# .clang-format uses InsertBraces (clang-format >= 15); the style is pinned to
# 16+. Ubuntu 22.04's unversioned clang-format is 14 and would spew "unknown
# key" errors for every file, so fail early with a clear message instead.
define require_clang_format
	@test -n "$(CLANG_FORMAT)" || { \
	    echo "clang-format not found; install clang-format-18 or set CLANG_FORMAT=<binary>"; exit 1; }
	@test "$(CLANG_FORMAT_VER)" -ge 16 2>/dev/null || { \
	    echo "clang-format >= 16 required (found: $(CLANG_FORMAT_VER)); install clang-format-18"; exit 1; }
endef

check-style:
	bash check-style.sh
format:
	$(require_clang_format)
	$(CLANG_FORMAT) --style=file:.clang-format -i $(FMT_SRCS)
check-format:
	$(require_clang_format)
	$(CLANG_FORMAT) --style=file:.clang-format --dry-run -Werror $(FMT_SRCS)

.PHONY: test-event-loop-startup
test-event-loop-startup:
	CC="$(CC)" python3 ../tests/test-event-loop-startup.py

.PHONY: test-pebs-startup
test-pebs-startup:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
	    ../tests/test-pebs-startup.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-log-levels
test-log-levels: all
	CC="$(CC)" python3 ../tests/test-log-levels.py

.PHONY: test-sampling-window
test-sampling-window:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O2 -D_GNU_SOURCE -I. -ffunction-sections -fdata-sections \
	    ../tests/test-sampling-window.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-thread-coverage
test-thread-coverage:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. -pthread \
	    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
	    ../tests/test-thread-coverage.c perf.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-thread-exit
test-thread-exit:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. -pthread \
	    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
	    ../tests/test-thread-exit.c perf.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-workload-gone
test-workload-gone:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
	    ../tests/test-workload-gone.c utils.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-workload-exit
test-workload-exit:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O2 -D_GNU_SOURCE -I. -ffunction-sections -fdata-sections \
	    ../tests/test-workload-exit.c utils.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-pebs-quality
test-pebs-quality:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
	    ../tests/test-pebs-quality.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-cha-freshness
test-cha-freshness:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
	    ../tests/test-cha-freshness.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-pmu-control
test-pmu-control:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
	    ../tests/test-pmu-control.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-bin-index
test-bin-index:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-bin-index.c admission.c binning.c cooling.c obj-pool.c error.c \
	    -Wl,--gc-sections -lm -lnuma -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-binning
test-binning:
	@set -eu; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -D_GNU_SOURCE -I. -O1 -g -fsanitize=address,undefined \
	    -ffunction-sections -fdata-sections ../tests/test-binning.c binning.c \
	    -Wl,--gc-sections -lm -o "$$test_bin"; "$$test_bin"

.PHONY: test-pac-stats
test-pac-stats:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined,float-cast-overflow -ffunction-sections -fdata-sections \
	    ../tests/test-pac-stats.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-pac-threshold
test-pac-threshold:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-pac-threshold.c admission.c binning.c cooling.c obj-pool.c error.c \
	    -Wl,--gc-sections -lm -lnuma -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-quiet-stats
test-quiet-stats:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-quiet-stats.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-startup-alloc
test-startup-alloc:
	@set -eu; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -D_GNU_SOURCE -I. -O1 -g -ffunction-sections -fdata-sections \
	    -fsanitize=undefined -fno-sanitize-recover=all \
	    ../tests/test-startup-alloc.c admission.c binning.c config.c error.c obj-pool.c \
	    cgroup.c cat.c ldlat-dyn.c utils.c \
	    -Wl,--gc-sections -Wl,--wrap=calloc -Wl,--wrap=free \
	    -Wl,--wrap=sched_getaffinity -lm -lnuma -o "$$test_bin"; \
	python3 ../tests/test-startup-alloc.py "$$test_bin"

.PHONY: test-pool-limit
test-pool-limit:
	@set -eu; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -D_GNU_SOURCE -I. -O1 -g -ffunction-sections -fdata-sections \
	    -fsanitize=undefined -fno-sanitize-recover=all \
	    ../tests/test-pool-limit.c cli.c config.c error.c obj-pool.c ldlat-dyn.c cgroup.c cat.c \
	    -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-pool-fallback
test-pool-fallback:
	@set -eu; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -D_GNU_SOURCE -I. -O1 -g -ffunction-sections -fdata-sections \
	    -fsanitize=undefined -fno-sanitize-recover=all \
	    ../tests/test-pool-fallback.c error.c obj-pool.c \
	    -Wl,--gc-sections -Wl,--wrap=pool_create -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-migration-wait-stress
test-migration-wait-stress:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
	    ../tests/test-migration-wait-stress.c error.c -Wl,--gc-sections \
	    -Wl,--wrap=poll -Wl,--wrap=__poll_chk -lm -lpthread -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-migration-wakeup
test-migration-wakeup:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
	    ../tests/test-migration-wakeup.c error.c -Wl,--gc-sections \
	    -Wl,--wrap=read -Wl,--wrap=__read_chk -Wl,--wrap=write -lm -lpthread -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-migration-stop
test-migration-stop:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
	    ../tests/test-migration-stop.c error.c -Wl,--gc-sections -lm -lpthread -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-task-scan-error
test-task-scan-error:
	@set -eu; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -Werror -std=gnu11 -O1 -g -I. \
	    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
	    ../tests/test-task-scan-error.c error.c perf.c \
	    -Wl,--gc-sections -lm -lnuma -lpthread -o "$$test_bin"; \
	"$$test_bin" 1; "$$test_bin" 0

.PHONY: test-migration-recovery
test-migration-recovery:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
	    ../tests/test-migration-recovery.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-cli-modes
test-cli-modes:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-cli-modes.c cli.c config.c validate.c utils.c error.c ldlat-dyn.c \
	    cgroup.c cat.c \
	    -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin" 2>/dev/null

.PHONY: test-tierce-admission
test-tierce-admission:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-tierce-admission.c admission.c binning.c error.c \
	    -Wl,--gc-sections -lm -lnuma -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-workload-mlp
test-workload-mlp:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-workload-mlp.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-domain-round-robin
test-domain-round-robin:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-domain-round-robin.c error.c -Wl,--gc-sections -lm -lpthread \
	    -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-tierce-sampling
test-tierce-sampling:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-tierce-sampling.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-ldlat-dyn
test-ldlat-dyn:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-ldlat-dyn.c ldlat-dyn.c cgroup.c error.c -Wl,--gc-sections -lm \
	    -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-cat-control
test-cat-control:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-cat-control.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-cat-start
test-cat-start:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-cat-start.c perf.c error.c -Wl,--gc-sections -lm -lpthread \
	    -o "$$test_bin"; \
	"$$test_bin"

# The diagnostics build compiles and lists its options; the mode tests and
# the PAC-table dump test run with PACT_DIAG. Built in temporary files, so
# the normal build stays.
.PHONY: test-diag-build
test-diag-build: build-info.h
	@set -e; test_bin=$$(mktemp); cli_bin=$$(mktemp); \
	trap 'rm -f "$$test_bin" "$$cli_bin"' EXIT; \
	$(CC) -Wall -Wextra -Werror -O2 -D_GNU_SOURCE -I. -DPACT_DIAG \
	    $(filter-out logging.c,$(wildcard *.c)) -o "$$test_bin" $(LDFLAGS); \
	help=$$("$$test_bin" --help); \
	for opt in --dump-pac-table --dump-pac-interval --no-promote-wl --cat-trace \
	    --trace-migration; do \
	    case "$$help" in *"$$opt"*) ;; *) echo "missing $$opt in DIAG help"; exit 1;; esac; \
	done; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. -DPACT_DIAG \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-cli-modes.c cli.c config.c validate.c utils.c error.c ldlat-dyn.c \
	    cgroup.c cat.c diag.c -Wl,--gc-sections -lm -o "$$cli_bin"; \
	"$$cli_bin" >/dev/null 2>&1; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. -DPACT_DIAG \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-pac-dump.c diag.c admission.c binning.c error.c \
	    -Wl,--gc-sections -lm -lnuma -o "$$cli_bin"; \
	"$$cli_bin"; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. -DPACT_DIAG \
	    -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-bin-index.c admission.c binning.c cooling.c obj-pool.c error.c diag.c \
	    -Wl,--gc-sections -lm -lnuma -o "$$cli_bin"; \
	"$$cli_bin"

.PHONY: test-migration-trace
test-migration-trace:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-migration-trace.c trace.c balance.c cgroup.c utils.c error.c \
	    -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-cgroup
test-cgroup:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-cgroup.c cgroup.c balance.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"

.PHONY: test-pact-global
test-pact-global:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -ffunction-sections -fdata-sections \
	    ../tests/test-pact-global.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"
