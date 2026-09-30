# Makefile for the PACT runtime.

CC      = gcc
CFLAGS  = -Wall -Wextra -O3 -g -D_GNU_SOURCE -I. -MMD -MP
LDFLAGS = -lm -lnuma -lpthread

TARGET  = pact

# All sources except logging.c, which is an optional build (its symbols are
# no-ops via logging.h unless PACT_ENABLE_LOGGING is set; see `make logging`).
SOURCES = $(filter-out logging.c,$(wildcard *.c))
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
	$(MAKE) CFLAGS="$(CFLAGS) -DPACT_ENABLE_LOGGING" SOURCES="$(wildcard *.c)"

clean:
	rm -f *.o *.d $(TARGET) build-info.h .buildflags

# Style checks (see CODING_STYLE.md).
# check-style: grep-based brace-style check, no dependencies.
# check-format: full clang-format check. Prefer clang-format-18 (the
# version CI pins, so local and CI agree), fall back to the unversioned
# binary, or override with CLANG_FORMAT=<binary>.
CLANG_FORMAT ?= $(shell command -v clang-format-18 || command -v clang-format)
CLANG_FORMAT_VER = $(shell $(CLANG_FORMAT) --version 2>/dev/null | grep -oE '[0-9]+' | head -1)
FMT_SRCS = $(filter-out khashl.h minicoro.h build-info.h,$(wildcard *.c *.h))

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
	$(CLANG_FORMAT) -i $(FMT_SRCS)
check-format:
	$(require_clang_format)
	$(CLANG_FORMAT) --dry-run -Werror $(FMT_SRCS)

.PHONY: test-pebs-startup
test-pebs-startup:
	@set -e; test_bin=$$(mktemp); trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) -Wall -Wextra -O1 -g -D_GNU_SOURCE -I. \
	    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
	    ../tests/test-pebs-startup.c error.c -Wl,--gc-sections -lm -o "$$test_bin"; \
	"$$test_bin"
