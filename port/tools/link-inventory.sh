#!/bin/bash
#
# Asks the linker what the platform layer still owes the game.
#
# Compiles every game translation unit that will compile, plus the port layer,
# then reports which symbols are referenced and never defined -- split into SDK
# symbols (the shim's responsibility) and game symbols (translation units that
# do not compile yet, or asset data extracted from the player's ROM).
#
#   ./port/tools/link-inventory.sh
#
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 1

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

FLAGS=(
    -c -std=gnu99
    -D_LANGUAGE_C -DGEPC -DVERSION_US -DREFRESH_NTSC
    -Iport/include -include gepc_prelude.h
    -idirafter . -idirafter include -idirafter include/PR
    -idirafter src -idirafter src/game -idirafter src/libultra
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -w
)

# Only the SDK's *hardware* layers are replaced by port/src/libultra.c:
# os/ (threads, scheduler, TLB) and io/ (VI, PI, SI, controllers), plus the
# whole of libultrare. Compiling those would drag in the assembly-only
# internals they call.
#
# src/libultra/gu/ and audio/ are kept. gu/ is portable matrix and trig maths
# -- guPerspective, guLookAt, guTranslate -- and audio/ is the sequence player
# that builds the Acmd list the software microcode consumes. Excluding those
# was too broad, and showed up immediately as unresolved gu* symbols.
while read -r f; do
    case "$f" in
        src/libultra/os/*|src/libultra/io/*|src/libultrare/*) continue;;
    esac
    o="$tmp/game_$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')"
    gcc "${FLAGS[@]}" -o "$o" "$f" 2>/dev/null || rm -f "$o"
done < <(find src -name '*.c' | sort)

for f in port/src/*.c; do
    case "$f" in *gfx_gl.c|*video.c|*audio_sdl.c) continue;; esac
    gcc "${FLAGS[@]}" -o "$tmp/port_$(basename "${f%.c}").o" "$f" 2>/dev/null
done

nm -u "$tmp"/*.o 2>/dev/null | awk '{print $2}' | sort -u > "$tmp/undef"
nm --defined-only "$tmp"/*.o 2>/dev/null | awk 'NF==3 {print $3}' | sort -u > "$tmp/def"
comm -23 "$tmp/undef" "$tmp/def" > "$tmp/gap"

echo "game objects   : $(ls "$tmp"/game_*.o 2>/dev/null | wc -l)"
echo "port objects   : $(ls "$tmp"/port_*.o 2>/dev/null | wc -l)"
echo
echo "SDK symbols still owed by the platform layer:"
if grep -qE '^(os|__os|gu[A-Z])' "$tmp/gap"; then
    grep -E '^(os|__os|gu[A-Z])' "$tmp/gap" | sed 's/^/  /'
else
    echo "  none"
fi
echo
echo "game symbols unresolved (uncompiled units + ROM-extracted asset data): $(grep -vcE '^(os|__os|gu[A-Z])' "$tmp/gap")"
exit 0
