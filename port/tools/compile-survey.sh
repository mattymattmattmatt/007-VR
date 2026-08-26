#!/bin/bash
#
# Reports how many of the game's own translation units compile against the PC
# platform layer, and groups whatever still fails by cause.
#
# The point is to keep the remaining work measurable. Run it from the
# repository root:
#
#   ./port/tools/compile-survey.sh          # summary
#   ./port/tools/compile-survey.sh -v       # also list the failing files
#
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 1

# Mirrors the flags in port/CMakeLists.txt. See port/README.md for why each of
# them is needed -- in particular why the repository's include/ must come
# *after* the system directories.
FLAGS=(
    -fsyntax-only -std=gnu99
    -D_LANGUAGE_C -DGEPC
    -DVERSION_US -DLANG_US -DREFRESH_NTSC
    -DLEFTOVERDEBUG -DLEFTOVERSPECTRUM -DBUGFIX_R0 -DBYTEMATCH
    -Iport/include -include gepc_prelude.h
    -idirafter . -idirafter include -idirafter include/PR
    -idirafter src -idirafter src/game -idirafter src/libultra
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
    -fms-extensions
    -w
)

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# src/libultra/os, src/libultra/io and all of src/libultrare are the N64
# hardware layers; port/src/libultra.c replaces them, so the PC build never
# compiles them and counting them would misstate the remaining work.
#
# src/libultra/libc/string.c goes the same way. It defines strchr, strlen and
# memcpy over `const unsigned char *`, which is what the N64's freestanding
# libc wanted; on a hosted build those names are glibc's, with the same
# behaviour and incompatible prototypes. The port takes glibc's.
find src -name '*.c' \
    ! -path 'src/libultra/os/*' \
    ! -path 'src/libultra/io/*' \
    ! -path 'src/libultra/libc/string.c' \
    ! -path 'src/libultrare/os/*' \
    ! -path 'src/libultrare/io/*' \
    | sort > "$tmp/files"
total=$(wc -l < "$tmp/files")

while read -r f; do
    if out=$(gcc "${FLAGS[@]}" "$f" 2>&1) && [ -z "$out" ]; then
        printf 'OK|%s\n' "$f"
    else
        first=$(printf '%s\n' "$out" | grep -m1 'error:' | sed 's/.*error: //')
        printf 'FAIL|%s|%s\n' "$f" "$first"
    fi
done < "$tmp/files" > "$tmp/results"

okc=$(grep -c '^OK|' "$tmp/results")
failc=$(grep -c '^FAIL|' "$tmp/results")

echo "compile survey: $okc / $total translation units"
echo "still failing : $failc"
echo
echo "causes:"
grep '^FAIL|' "$tmp/results" | cut -d'|' -f3- \
    | sed -E "s/'[^']*'/'X'/g" | sort | uniq -c | sort -rn | head -20

if [ "${1:-}" = "-v" ]; then
    echo
    echo "failing files:"
    grep '^FAIL|' "$tmp/results" | cut -d'|' -f2 | sed 's/^/  /'
fi

exit 0
