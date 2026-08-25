# GoldenEye 007 — PC port (layer 2)

The decomp builds an N64 ROM. This directory makes the game run on a PC.
`vr/` then puts it in a headset. See
[docs/VR/Architecture.md](../docs/VR/Architecture.md) for how the three layers
relate, and why Perfect Dark's port is the template.

```sh
cmake -S port -B build/port -DCMAKE_BUILD_TYPE=Release
cmake --build build/port -j
ctest --test-dir build/port --output-on-failure
```

## State

| Piece | State |
|---|---|
| `src/system.c` — clock, sleep, paths, logging | **done** |
| `src/rdram.c` — low-memory arena for game memory | **done, 40 assertions** |
| `src/libultra.c` — threads, message queues, timers | **done, 236 assertions** |
| `src/libultra.c` — PI DMA against the ROM image | **done** |
| `src/romdata.c` — assets from the player's ROM | **done, 53 assertions** |
| `src/sha1.c` — ROM identification | **done** |
| `src/gbi_walk.c` — display-list decoder/validator | **done, 61 assertions** |
| `src/gfx_state.c` — RSP/RDP state machine | **done, 88 assertions** |
| GPU backend (`gfx_backend.h` implementor) | not started |
| `src/video.c` — interception at `fr.c` | not started |
| `src/audio.c` | not started |
| `src/input.c` | not started |

Nothing here is wired into the game yet. The platform layer is built and
tested on its own first, because a shim with subtly wrong queue semantics
produces a game that boots and then deadlocks, which is miserable to debug
later with a headset on.

## Bring-up notes

These were all found the hard way. Anything compiling the game's headers on a
PC needs them.

### The compile recipe

```
-D_LANGUAGE_C -idirafter <repo>/include -idirafter <repo>/src
```

**`_LANGUAGE_C`** — `PR/ultratypes.h` wraps every typedef in
`#if defined(_LANGUAGE_C)`. Without it the headers parse and declare nothing,
and you get a confusing wall of "unknown type name `OSThread`".

**`-idirafter`, not `-I`** — the repository ships N64 replacements for seven
libc headers: `stdarg.h`, `stddef.h`, `string.h`, `stdlib.h`, `math.h`,
`assert.h`, `limits.h`. With a plain `-Iinclude`, the host's `stdio.h` includes
`<stdarg.h>`, finds the N64 one, which includes `ultra64.h`, and the build
collapses in a way that points nowhere near the real cause. `-idirafter` puts
the repo's directories *after* the system ones, so libc wins for those names
while `PR/*` and `ultra64.h` still resolve.

### bcopy, bcmp and bzero collide with glibc — twice

`PR/os.h` declares these three with `int` lengths, as the N64 SDK did. glibc
declares them with `size_t`. That is a hard conflict, and it arrives by two
separate routes that need two separate fixes:

1. **Declarations**, from `<strings.h>`, pulled in by `<string.h>`.
   Fixed by `port/include/strings.h`, a deliberate shim that shadows glibc's
   and forwards only the non-conflicting functions.
2. **Definitions**, from `bits/strings_fortified.h`, pulled in by
   `pthread.h` → `features.h`. These are fortified inline *definitions*, so no
   include ordering can avoid them. Fixed by building the port with
   `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0`.

The second only triggers at `-O2` and above. A debug build can look perfectly
healthy and Release then fails, so both fixes are in `CMakeLists.txt` rather
than left to whoever hits it next.

### CMake de-duplicates repeated flags

`target_compile_options(... -idirafter A -idirafter B)` collapses the two
identical `-idirafter` tokens into one and leaves `B` stranded as a bare
argument, silently dropping a directory from the search path. The `SHELL:`
prefix keeps each flag and its path together.

### 64-bit: settled, with a low arena

`PR/os.h` declares `u32 osVirtualToPhysical(void *)`, because on the N64 every
address genuinely was 32 bits. The game relies on it: `src/game/model.c` feeds
the result straight into display lists through `gSPVertex`.

On a 64-bit host a heap pointer does not fit. Silently truncating would fill
display lists with addresses that are wrong in a way nothing detects until
geometry renders as garbage, so the shim **panics instead of truncating**.

**The port builds 64-bit and confines game memory to a low arena.** Perfect
Dark's port takes the other route and builds for i686; this one keeps the
renderer, the OpenXR loader and the port itself 64-bit, and instead places
everything the game can see inside the 8 MB arena in `src/rdram.c`, reserved
below 4 GB (`MAP_32BIT` on Linux, a base-address walk on Windows). Game
pointers then fit in a `u32` naturally.

That avoids multilib and the hunt for a 32-bit VR runtime, and leaves headroom
for stereo rendering. It also matches what the game already expects:
`src/boss.c` hands its pool allocator the span from the end of BSS to the TLB
block — whatever RDRAM is left over — and here that span is simply the tail of
this arena, carved up by `mempCheckMemflagTokens` exactly as on hardware.

If the arena cannot be placed low, the port refuses to start rather than
corrupt display lists. `osVirtualToPhysical` keeps its panic as a backstop for
port-side memory leaking into a game structure.

Fast3D resolves *segmented* addresses through its own segment table, so this
only has to cover direct pointers.

### Deliberate differences from real hardware

**Scheduling.** The N64 is strictly priority-preemptive — the highest-priority
runnable thread always runs. Here the host scheduler decides and `OSPri` is
advisory. Game code that quietly relied on a lower-priority thread not running
can therefore race. The fix when it bites is an explicit message-queue
handshake, not rebuilding a priority scheduler on top of the host's.

**Queue synchronisation.** `OSMesgQueue` has no room for a mutex or condition
variable and its layout has to stay as the game's headers declare it, so every
queue shares one global mutex and condition variable, woken by broadcast. With
the handful of threads an N64 title runs, contention is irrelevant.

**`osClockRate`.** On hardware `osInitialize` sets it to `OS_CLOCK_RATE` and
then scales it by 3/4, so game code always reads the count-register rate. The
port never runs `osInitialize`, so the shim defines it already carrying the
post-init value. Seeding it with `OS_CLOCK_RATE` instead would make every
timer and elapsed-time reading run 33% fast.

**Thread suspension.** Real libultra can suspend an arbitrary thread.
`osStopThread` here records the intent and lets the thread notice, because
forcibly suspending a host thread mid-`malloc` deadlocks.

**RSP/RDP calls are absent on purpose.** `osSpTaskLoad`, `osSpTaskStartGo`,
`osDpSetNextBuffer` and the VI framebuffer calls are not implemented. A Fast3D
port intercepts the finished display list before the RSP would see it, so
emulating them would be work in service of nothing. Perfect Dark's port omits
them for the same reason.

## The renderer gap is three commands

Before vendoring 7.5k lines of Fast3D it was worth checking the claim the whole
plan rests on: that GoldenEye's display lists are in a format an existing
translator can decode. They are, almost entirely.

**GoldenEye builds the F3DEX (GBI 1) branch** of `PR/gbi.h` — no `F3DEX_GBI_2`
define exists anywhere in the build.

**Rare's `G_TRI4` extension is shared with Perfect Dark.** `include/gbi_extension.h`
adds a packed four-triangle command and redefines `gSP2Triangles` in terms of
it. GoldenEye's opcode is `G_IMMFIRST-14`; Perfect Dark's is the same value,
and its Fast3D already has a case for it. This is the single most important
compatibility result: a stock decoder that only knows `G_TRI1`/`G_TRI2` would
see almost no geometry at all.

Comparing the 66 distinct GBI macros GoldenEye emits against the opcodes
Perfect Dark's Fast3D decodes leaves exactly **three** unhandled:

| Command | GoldenEye usage |
|---|---|
| `G_MODIFYVTX` | the only `gSPModifyVertex` in the source is commented out |
| `G_SETBLENDCOLOR` | one call site, `src/boss.c` |
| `G_SETPRIMDEPTH` | the adjacent line in `src/boss.c` |

So the practical gap is a single adjacent pair in one file. `gbi_walk.c` flags
all three rather than letting them pass as ordinary traffic.

One trap worth recording: `gbi_extension.h` also defines `G_SETTEX` as `0xc0`,
which **collides with `G_NOOP`**. The macro that would emit it
(`gsSPUseTexture`) has no callers, so treating `0xc0` as a no-op is safe today
— but a backend that starts seeing `0xc0` with a non-zero payload is looking at
`G_SETTEX`, not a no-op.

Two API notes from building the walker:

- `G_TRI4` slots whose three indices are all zero are **padding**, not
  degenerate geometry. Counting them inflates every model by up to half.
- `gbiWalk` takes an explicit command bound. A display list carries no length,
  only a `G_ENDDL` terminator, so an unterminated one cannot be detected
  except by refusing to read past a caller-supplied limit — by the time any
  counter noticed, the walk has already run off the buffer.

## Renderer structure

The renderer splits in two, and only the first half is GoldenEye-specific:

- **`gfx_state.c`** does the N64 work — segment resolution, the matrix stack,
  the vertex cache, transforming vertices, and turning Rare's packed `G_TRI4`
  into triangles. It is testable with no GPU present.
- **A backend** implements `gfx_backend.h` and only has to know how to draw.
  It never sees a display list.

Three decoding rules the state machine has to get right, all pinned by tests
that build their lists with the game's own macros rather than hand-written
words:

- **`G_TRI1` indices are pre-multiplied by 10** and live in `w1`. GoldenEye
  builds the branch where that is true; assuming the F3DEX "times two" form
  yields indices five times too large.
- **`G_TRI4` indices are raw 4-bit** and cap at vertex 15, which is why
  `G_TRI1` still exists for the rest of the 32-entry cache. Mixing the two
  conventions up is the single easiest way to render nothing.
- **All-zero `G_TRI4` slots are padding.** Drawing them adds a stray triangle
  at vertex 0 to a large share of the game's models.

The fixed-point matrix format is also worth noting: the N64 splits a 4x4 into
eight words of integer halves followed by eight of fractional halves. This
reads them arithmetically out of the `s32`s rather than casting to `s16 *`, so
the decode does not depend on host byte order. No transpose is needed —
libultra matrices are row-vector and this layer is column-vector, and a linear
copy between the two storage orders already is that transpose.

## How assets are read

The port does **not** reimplement GoldenEye's file table, segment layout or rz
decompression. All of that already exists in the game and works; it just
expects to reach the data over PI DMA from a cartridge.

So `romdata.c` loads the player's ROM into memory and `osPiStartDma` reads out
of it. `romCopy()` in `src/ramrom.c` — which every asset path funnels
through — then behaves exactly as it did on hardware, and the entire asset
pipeline comes along for free.

Three details worth keeping:

- **The completion message is posted on every path, including failure.**
  `romCopy()` does a DMA and then blocks on `osRecvMesg` unconditionally. A
  failed read that quietly returned without posting would hang the game
  forever rather than show a bad texture. There is a test for this.
- **Failed reads zero the destination** instead of leaving it undefined.
  Garbage in a display list is far harder to diagnose than empty geometry.
- **Unrecognised ROMs warn rather than refuse.** The three known SHA-1s come
  from the `ge007.*.sha1` files at the repository root. A romhack or an
  unlisted revision may well work, so the loader says what it found and
  carries on.

All three dump formats are accepted — z64, v64 and n64 — and normalised to
big-endian on load, decided from the header magic rather than the extension.

## Assets

Unchanged from the rest of the repository: no assets are included or
redistributed. The port will read them from the player's own ROM at runtime,
the way Perfect Dark's port does.
