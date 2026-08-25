# GoldenEye 007 — VR layer

An OpenXR VR layer for this GoldenEye 007 decompilation, aimed at SteamVR on
the desktop (which is how a Quest 3 connects through Virtual Desktop or Link).

The controls are built on the game's own **2.4 Goodhead** control style — the
stock dual-controller layout where one stick aims and the other moves. Feeding
it two synthetic N64 pads gives true twin-stick without rewriting any of the
player movement code.

## Status, honestly

The VR layer itself is written, builds warning-clean, and is covered by 370
headless assertions. **The game is not yet playable in VR**, and the reason is
worth being precise about:

This repository is a *matching decompilation*. It builds an N64 ROM with a MIPS
toolchain — it has no PC target at all. Nothing in it draws a pixel on a
desktop GPU: rendering goes out as N64 display lists to be executed by RSP
microcode (`rsp/graphics/gmain.s`). A VR headset needs a PC renderer, and one
does not exist here yet.

So the work splits cleanly in two:

| Piece | State |
|---|---|
| OpenXR session, swapchains, stereo frame loop | **done** |
| Controller bindings (Touch, Index, Vive, WMR, simple) | **done** |
| Goodhead twin-stick mapping + head servo | **done, tested** |
| Stereo camera, off-axis projection, roomscale | **done, tested** |
| Comfort: snap turn, vignette, recentre, haptics | **done** |
| Game-side hooks, inert without `GE_VR` | **done** |
| Calibration harness you can run in the headset | **done** |
| PC platform layer (libultra shim) | **not started** |
| Graphics backend (display lists → GPU) | **not started** |
| Audio backend | **not started** |

The two "not started" rows are the large ones, and they are a port project in
their own right rather than a VR problem. [Architecture.md](Architecture.md)
lays out what each involves.

## What you can run today

`ge007vr-calibrate` is a real OpenXR application. It brings up a session, binds
the real action set, runs the real control mapper, and drives a stand-in for
the engine's movement integrator. You put the headset on and walk around a
one-metre grid using exactly the scheme the game will use.

That makes it useful for three things before the renderer exists:

- confirming SteamVR sees your headset and both Touch controllers
- feeling the twin-stick mapping and tuning snap angle, deadzones and servo gain
- measuring `world_scale` — walk a known number of grid squares and compare

```sh
cmake -S vr -B build/vr -DCMAKE_BUILD_TYPE=Release
cmake --build build/vr -j
cp vr/config/gevr.ini build/vr/
./build/vr/ge007vr-calibrate
```

Tests, which need neither a headset nor a GPU:

```sh
ctest --test-dir build/vr --output-on-failure
```

## Assets

Same rule as the rest of the repository: it does not contain the game's assets,
and you need your own copy of GoldenEye 007 to extract them. Nothing in the VR
layer changes that.

## The ROM build is unaffected

Every hook in `src/` sits inside `#ifdef GE_VR`, which the ROM build never
defines. `src/joy.c` in particular carries code commented "required for
matching", so this is enforced rather than assumed: `vr/shim/gevr_shim.c`
preprocesses to zero non-blank lines without `GE_VR`, and the CMake target
`gevr_shim_inert` exists to keep it that way.

## Documentation

- [Controls.md](Controls.md) — the control scheme, and why it is built this way
- [QuestSetup.md](QuestSetup.md) — Quest 3, Virtual Desktop and SteamVR setup
- [Architecture.md](Architecture.md) — how the layer works, and what is left
