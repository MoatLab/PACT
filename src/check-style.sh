#!/bin/bash
# check-style.sh: lightweight style check for PACT C sources.
#
# Project style: braces around every if/else/for/while body, guard clauses
# included. `make check-format` (clang-format InsertBraces) enforces it.
# This grep gate is coarser: it allows same-line guard bodies.
#
# What this script flags:
#   1. `if (cond) stmt;`           where stmt is NOT one of:
#                                    return [val];  break;  continue;  goto X;  exit(...);
#   2. `else stmt;`                where stmt is NOT return, break, continue or goto
#   3. `} else stmt;`              - same as above
#
# What this script does NOT yet flag (clang-format does, see `make format`):
#   - next-line braceless bodies (`if (...)\n    stmt;`)
#   - for / while / do-while with same issues
#
# Run `make format` (requires clang-format >= 16) for full mechanical fixes.

set -euo pipefail
# Resolve symlinks (the documented pre-commit setup symlinks this script
# into .git/hooks/, where a plain dirname would land in .git/ and the
# globs below would silently match nothing).
cd "$(dirname "$(readlink -f "$0")")/.."

SRC=src
if [[ ! -d $SRC ]]; then
    echo "check-style: cannot find $SRC/ from $PWD" >&2
    exit 2
fi
violations=0
RED='\033[31m'; RST='\033[0m'; [[ -t 2 ]] || { RED=''; RST=''; }

# Files exempt from the check (third-party vendored headers).
EXCLUDE_RE='/(khashl|minicoro)\.h$'

# 1. Same-line if-body that isn't a guard. A guard is recognized by its body
#    starting with: return, break, continue, goto, exit.
while IFS=: read -r file line text; do
    # Skip vendored third-party headers.
    case "$file" in *khashl.h|*minicoro.h) continue ;; esac
    # Skip same-line braced bodies (already correct: `if (x) { foo; }`)
    case "$text" in
        *"if ("*") {"*) continue ;;
    esac
    # Extract body after the closing `)` of the condition.
    body=$(printf '%s\n' "$text" | sed -E 's/.*if \([^)]*\)[[:space:]]+//')
    case "$body" in
        return\ *|return\;*|break\;*|continue\;*|goto\ *|exit\(*)
            continue ;;  # guard clause: allowed
    esac
    printf "${RED}%s:%s:${RST} same-line non-guard if-body: %s\n" \
        "$file" "$line" "$text" >&2
    violations=$((violations + 1))
done < <(grep -nE '^\s*[}]?[[:space:]]*if \([^)]*\)[[:space:]]+[a-zA-Z_]' \
         "$SRC"/*.c "$SRC"/*.h 2>/dev/null \
         | grep -vE 'if \([^)]*\)[[:space:]]+\{' \
         | grep -vE '^\s*\*')

# 2. Same-line else-body. A guard body (return, break, continue, goto) is
#    allowed.
while IFS=: read -r file line text; do
    case "$file" in *khashl.h|*minicoro.h) continue ;; esac
    case "$text" in
        *"else {"*) continue ;;
        *"else if ("*) continue ;;
    esac
    case "$text" in
        *else[[:space:]]return*|*else[[:space:]]break*|*else[[:space:]]continue*|*else[[:space:]]goto*)
            # tolerate guard-like else (rare but valid)
            continue ;;
    esac
    printf "${RED}%s:%s:${RST} same-line else-body: %s\n" \
        "$file" "$line" "$text" >&2
    violations=$((violations + 1))
done < <(grep -nE '(^\s*else|\}[[:space:]]*else)[[:space:]]+[a-zA-Z_]' \
         "$SRC"/*.c "$SRC"/*.h 2>/dev/null)

if (( violations == 0 )); then
    echo "check-style: PASS - no same-line brace violations found."
    echo
    echo "Note: this is a coarse grep-based check. For full coverage run:"
    echo "    make check-format    # requires clang-format >= 16"
    exit 0
fi

echo
echo "check-style: $violations violation(s) found." >&2
echo "Fix manually, or run 'make format' to auto-fix with clang-format." >&2
exit 1
