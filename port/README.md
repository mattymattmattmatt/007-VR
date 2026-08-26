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
| `src/gfx_texture.c` — N64 texture formats to RGBA8 | **done, 69 assertions** |
| `src/gfx_gl.c` — OpenGL 3.3 backend | **first pass, builds** |
| `src/video.c` — window, GL context, frame loop | **done, builds** |
| SP task interception | **done, 19 assertions** |

| `src/audio.c` — AI output path | **done** |
| `src/audio_abi.c` — software audio microcode | **first pass, 350 assertions** |
| `src/audio_sdl.c` — audio device | **builds, unrun** |
| `src/input.c` | not started |

Nothing here is wired into the game yet. The platform layer is built and
tested on its own first, because a shim with subtly wrong queue semantics
produces a game that boots and then deadlocks, which is miserable to debug
later with a headset on.

## Compiling the game's own sources

The next milestone is building the 313 translation units in `src/` against
this platform layer. `port/tools/compile-survey.sh` reports where that stands
and groups whatever still fails by cause, so the remaining work stays
measurable.

It went **42 -> 262 of 313** in one pass, and almost none of that was porting
work — it was three findings:

**Include paths, not portability (42 -> 195).** The bulk of the early failures
were headers that simply were not on the search path: `assets/images.def`,
`os_internal.h`, `mbi.h`, `io/controller.h`. Adding the repository root,
`include/PR`, and `src/libultra` fixed 153 files without touching a line of
code.

**A circular include (195 -> 240).** `bondtypes.h` includes
`game/chrobjdata.h`, which includes `bondtypes.h` straight back. With include
guards, whichever header a translation unit reaches first decides the order
the declarations appear in — and `chrobjdata.h` needs types declared further
down `bondtypes.h`. Units that reach `bondtypes.h` first therefore saw extern
arrays whose element structs did not exist yet. The ROM build survives because
its units happen to reach `chrobjdata.h` first; the PC build cannot rely on
that, so it takes the include at the bottom of `bondtypes.h` instead.

**BITFLAG only existed on the SGI compiler (240 -> 262).** `bondconstants.h`
generates its bitflag enums with a macro taking a fixed 33 parameters, relying
on the IDO preprocessor letting call sites omit the trailing ones. GCC and
clang reject that, so the macro is guarded on `__sgi` — and *everything else
got an empty definition*, which silently deleted every enum it declares.
`PLAYERFLAG` was one, and its absence took 47 translation units down. The PC
build uses a variadic equivalent in `port/include/gepc_bitflag.h`; call sites
are unchanged.

Both edits to `src/` are guarded on `GEPC` and the ROM build is provably
unaffected: preprocess `bondconstants.h` and `bondtypes.h` with and without
the patch, strip the `# line` markers, and the token streams are identical.
(Comparing the raw preprocessor output does *not* show that, because added
lines shift every line marker — worth knowing before concluding a guarded
change has leaked.)

The remaining 51 failures are a long tail rather than one blocker: struct
members that have moved, a handful of undeclared identifiers, an
`osSyncPrintf` arity mismatch, and some token-pasting in the model macros.

## What the platform layer still owes the game: nothing

`port/tools/link-inventory.sh` compiles every game translation unit that will
compile, plus the port layer, and asks the linker what is referenced and never
defined. It reports **no SDK symbols outstanding**.

Getting there took two corrections worth recording.

**Which SDK sources to replace.** Only the *hardware* layers belong to the
port: `src/libultra/os/` (threads, scheduler, TLB) and `src/libultra/io/`
(VI, PI, SI, controllers), plus all of `src/libultrare/`. Excluding the whole
of `src/libultra/` was too broad and showed up straight away as unresolved
`gu*` symbols: `gu/` is portable matrix and trig maths (`guPerspective`,
`guLookAt`), and `audio/` is the sequence player that builds the very Acmd
list the software microcode consumes. Both are kept and compiled.

**The last 43 symbols.** With the hardware layers excluded, the linker asked
for 43 more, and they grouped cleanly:

| Group | Treatment |
|---|---|
| `osVi*`, `osDp*` | Replaced, not emulated — the renderer intercepts the display list long before either would see anything |
| `osCont*` | **Real.** joy.c drives it every frame, and the VR layer's synthetic pads travel the same path |
| `osEeprom*` | **Real**, file-backed — it is the player's save data |
| TLB, FPU control, SI internals | Hardware-only, stubbed |

`osContInit` reports **two** controllers present on purpose: the 2.4 Goodhead
control style the VR layer targets reads two pads, and a game that believes
only one is connected will not offer that style at all.

Two traps found while writing these:

- `osContStartReadData` must post to the caller's message queue. The game
  blocks on it immediately afterwards, so a read that completed without
  posting would hang — the same trap as the PI DMA path.
- `OSContStatus`'s error field is literally named `errno`. Any translation
  unit that pulls in `<errno.h>` would macro-expand it and break the struct
  outright, so nothing in the port includes it.

And one real bug, caught by `-Werror=type-limits`: the EEPROM bounds check
compared a `u8` address against 256, which can never be true. The compiler was
right that the check was dead — but the *long* read and write variants compute
a block index that genuinely can run past the end, and narrowing it back to
`u8` would have wrapped it round to the start, quietly corrupting the
beginning of the save instead of failing. The range is now checked in `int`
before any narrowing.

## What the first end-to-end run found

`ge007-selftest` drives the real stack: it builds a display list in the RDRAM
arena, submits it through the SP task interception, and **reads the
framebuffer back**. That last part is what made it useful. Every bug below
left the triangle and draw-call counters looking perfectly healthy.

```sh
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/port/ge007-selftest
```

**The arena broke the segment-index invariant.** It was placed anywhere below
4 GB, and landed at `0x40c00000`. Segmented addresses carry their segment
number in bits 24-27, and on hardware RDRAM is physical `0x00000000-0x007fffff`
so a direct pointer always has segment index 0. At `0x40c00000` every direct
pointer in a display list was read as a segment reference, `G_VTX` resolved to
nothing, and every triangle drew degenerate. The arena is now constrained
below 16 MB and the placement is verified rather than assumed.

**Segment 0 was not the identity mapping.** Even at a correct address, an
unbound segment 0 made every direct pointer resolve to NULL. It is now bound
to zero by default, as hardware has it.

**Thirteen commands were never dispatched.** Among them `G_MOVEWORD`, which is
how `gSPSegment` binds a segment — so nothing segmented could resolve at all —
and the whole `G_SETTILE` / `G_SETTILESIZE` / `G_LOADBLOCK` / `G_LOADTLUT`
group, meaning no texture ever uploaded. The GL backend had handlers for all
of them; they were simply never called.

**Nested sub-lists spent the caller's command budget.** `data_size` describes
the list the task was handed; lists it calls into are separate allocations and
are not counted in it. An earlier note in this repository called `data_size` an
"exact bound" for the walk — that was wrong. A frame aborted as soon as it
called a sub-list, and GoldenEye's model code does that constantly, so nearly
every real frame would have silently lost geometry. The bound now applies to
the top-level list only, detected by the walk landing exactly on the buffer's
end address; a runaway guard covers everything else. Note that measuring
distance from the start instead is doubly wrong, because a branch leaves the
buffer for good and subtracting pointers into different objects is undefined
behaviour.

**Nothing cleared the framebuffer between frames.** Frames composited onto one
another, and stale depth values rejected the new frame's geometry wherever it
sat at the same depth — so the picture simply stopped updating. Colour and
depth are now cleared at frame start.

One harness lesson worth keeping: `glReadPixels` after `SDL_GL_SwapWindow`
reads a stale buffer, which can make a black screen look like a passing test.
`videoSetPresentEnabled(0)` keeps the finished frame where it can be read.

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

## Audio

The audio path is the mirror image of the graphics one. `alAudioFrame()` in
`src/libultra/audio/synthesizer.c` builds a list of `Acmd` commands; on
hardware the RSP audio microcode executes them against a 4 KB scratchpad and
leaves a frame of PCM behind, which the game hands to the DAC with
`osAiSetNextBuffer`. So the port interprets that command list in software
(`audio_abi.c`) and models the DAC as a ring buffer (`audio.c`).

**The pacing has to be truthful.** `src/audi.c` sizes each frame as
`g_FrameSize - (osAiGetLength() >> 2)`, so `osAiGetLength` must fall as the
device consumes audio. Returning a constant zero makes the game generate
maximum-size frames forever and run away from the DAC; over-reporting starves
it. That is why the ring is modelled properly rather than stubbed.

### What is solid, and what is not

Implemented exactly and tested against hand-computed values: buffer clears,
DMEM moves, DMA in and out, mixing, interleave, the big-endian DMEM layout,
and ADPCM nibble extraction, sign handling, scaling and clamping.

Every command the game can emit is now handled. The list is short and worth
checking against: `grep -o 'a[A-Z][A-Za-z]*(ptr' src/libultra/audio/*.c
src/libultrare/audio/*.c | sort -u` names fourteen builders, and there is a
case for each.

The three commands that carry state — resample, envelope mix and the reverb's
pole filter — were the interesting ones, and in each case the game's own
source settles what the microcode does rather than leaving it to guesswork:

- **Envelope mixer.** `_getRate` and `_getVol` in
  `src/libultrare/audio/env.c` are the SDK's model of the hardware. The
  telling detail is the `0.125` in `ivol += (rate * samples) * 0.125`: the
  rate is a signed 16.16 value added to the volume once per *block of eight
  samples*, so the envelope is a staircase, not a slope. The other half is the
  state block — `_pullSubFrame` sends the volume registers once at `A_INIT`
  and never again, so a continue frame has to recover volume, target, rate and
  the dry/wet sends from its own state or it mixes at whatever the last voice
  in the list happened to set.
- **Pole filter.** `init_lpfilter` in `src/libultrare/audio/drvrNew.c` writes
  eight zeros, then `fc` and its powers up to `fc^8`, and sets the command's
  gain to `SCALE - fc` with `SCALE` of 16384. A table of consecutive powers of
  one coefficient is what you build to unroll `y[n] = (fgain * x[n] + fc *
  y[n-1]) >> 14` across eight lanes; on a CPU the recurrence is just the
  recurrence, and `fgain = SCALE - fc` giving unity gain at DC is the check
  that the shift is 14 rather than 15.
- **Resampler.** Carries the fractional read position and the sample either
  side of a buffer boundary, so pitched voices are continuous across frames.
  The test for this asserts that splitting a run in two changes nothing:
  sixteen outputs from one call must equal two calls of eight with the input
  refilled, sample for sample.

**The one remaining approximation:** the resampler's interpolation kernel. The
hardware's 64-phase filter coefficient table is not in this repository, and a
table reproduced from memory would be both hard to hear and impossible to
attribute, so this uses a four-point Catmull-Rom spline instead. That is the
right shape for a four-tap interpolator and reproduces a straight line
exactly, but it is not the same filter: pitched voices will differ from
hardware in their high-frequency detail. It is the first place to look if the
audio sounds subtly wrong, and it wants checking against real output before
anyone calls the audio finished.

One bug worth recording, found by the compiler rather than by a test: the
ADPCM decoder shifted sign-extended nibbles left, and left-shifting a negative
value is undefined behaviour in C. It happened to do the right thing under
gcc, which is exactly what makes that class of bug surface years later under a
different compiler. Both sites now multiply instead.

## Where the display list is intercepted

The game builds a display list, wraps it in an `OSTask` and hands it to the
scheduler, which calls `osSpTaskLoad` / `osSpTaskStartGo`. On hardware the RSP
would execute it from there. The port intercepts the task instead and sends
the list to the renderer, which is exactly why GoldenEye's custom microcode
never matters.

`rspGfxTaskStart` in `src/game/rsp.c` fills in `data_ptr` with the first `Gfx`
and `data_size` with the list's length in bytes, so the interception gets an
**exact** command bound rather than trusting the list is terminated. That pairs
with the bound `gfxStateRun` already takes.

Three things this layer has to get right, each pinned by a test:

- **Dispatch on Load, not on both.** The scheduler calls `osSpTaskLoad` and
  `osSpTaskStartGo` in sequence; handling both renders every frame twice.
- **Keep the task types apart.** Feeding an audio task to the triangle decoder
  would be spectacular, and the two arrive through the same call.
- **Survive having no handler.** The game submits frames during boot, before
  video is up.

Routing goes through a registered handler (`gfxhook.h`) rather than calling
the renderer directly, so `libultra.c` stays free of SDL and GL and the
interception itself is testable with a mock and no GPU.

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
