# Architecture

## Layout

```
vr/
  include/          public headers
    gevr_math.h       vectors, quaternions, matrices, off-axis projection
    gevr_input.h      VR input state and the synthetic N64 pads
    gevr_config.h     tunables, loaded from gevr.ini
    gevr_controls.h   the Goodhead mapper
    gevr_camera.h     stereo view and projection
    gevr_engine.h     GoldenEye angle conventions <-> VR conventions
    gevr_xr.h         OpenXR session
    gevr_gl.h         framebuffers, vignette, mirror
  src/              implementations
  shim/             the seam into the engine; empty without GE_VR
  tools/            ge007vr-calibrate
  tests/            headless assertions, no GPU or headset needed
  config/gevr.ini   documented defaults
```

`gevr_core` (math, controls, camera, config, engine) has no dependency on
OpenXR, SDL or GL. That is deliberate: it is the part with a right answer, so
it is built and tested on its own.

## Division of labour

The rule the whole design follows:

> **The engine owns position. The VR layer owns orientation.**

Movement, collision, stairs, lifts and hitscan stay exactly as Rare wrote them,
driven by the synthetic move pad. The camera orientation comes from the live
headset pose, with a servo keeping the engine's own angles in sync so gameplay
agrees with what you see. `Controls.md` covers why.

## Frame flow

```
gevr_shim_frame_begin()      xrWaitFrame, xrBeginFrame, xrSyncActions,
                             xrLocateViews -> gevr_input_state
gevr_shim_inject_pads()      gevr_controls_update -> two N64 pads,
                             written into g_ContDataPtr->samples[curlast]
  ... the engine ticks unchanged ...
for each eye:
  gevr_shim_begin_eye(e)     acquire swapchain image, build the eye's
                             view/projection, bind the framebuffer
  ... the engine renders ...
  gevr_shim_end_eye(e)       vignette, release image
gevr_shim_frame_end()        xrEndFrame with the projection layer
```

## Hook points in the engine

Two, both inside `#ifdef GE_VR`:

- `src/joy.c`, end of `joyConsumeSamplesWrapper` — inject the pads after the
  real controllers are consumed and before any game code reads them.
- `src/fr.c`, after `guPerspectiveF` — replace the main view's projection with
  the headset's asymmetric frustum for the eye being rendered.

The second is not an adjustment, it is a replacement. `guPerspective` builds a
*symmetric* frustum, and a real HMD's is off-centre — approximating it with a
symmetric matrix shears the world toward the nose. `gevr_projection_from_fov`
takes the runtime's four half-angles directly.

One subtlety worth naming: libultra's float matrices are row-vector convention
(`v * M`), the VR layer's are OpenGL column-vector (`M * v`). They are
transposes. `gevr_shim_eye_projection_n64` does the transpose explicitly rather
than letting a `memcpy` produce a scrambled frustum.

## Keeping the ROM build safe

`vr/shim/gevr_shim.c` preprocesses to zero non-blank lines without `GE_VR`, and
`gevr_shim.h` supplies no-op macros so call sites compile away. The CMake target
`gevr_shim_inert` builds it precisely to prove that stays true. This matters
because `src/joy.c` contains code commented "required for matching" — a stray
statement in the wrong place changes the ROM.

## What is left

### 1. PC platform layer

The decomp targets N64 hardware through libultra: `osCreateThread`,
`osSendMesg`, `osViSwapBuffer`, PI/SI DMA, the scheduler in `src/sched.c`. A PC
build needs those backed by real threads, queues and timers. It is a large but
well-understood job — the sister Perfect Dark decomp has an established PC port
that took this route, and the two engines share ancestry.

### 2. Graphics backend

The real work. The engine emits N64 display lists executed by RSP microcode
(`rsp/graphics/gmain.s`). A PC renderer has to consume those display lists and
translate them to GPU draw calls: matrix stack, vertex cache, the combiner,
texture formats (CI4/CI8/IA/RGBA16), and the fog and Z modes the game leans on.

GoldenEye uses custom microcode rather than stock F3DEX2, so an off-the-shelf
translator will not drop straight in — expect to handle its command set
specifically. This is the single largest remaining item and the one that gates
"playable".

Once it exists, the VR layer needs only that it can render to a supplied
framebuffer with a supplied projection, which is what the hooks already pass.

### 3. Audio backend

The sequence and sample playback in `src/libultra/audio` targets the N64's
audio DSP. Needs an equivalent over SDL audio or similar. Independent of VR.

### 4. Things worth doing once it runs

- **HUD.** The 2D HUD draws in screen space, which is painful in a headset.
  `hud_distance` and `hud_scale` exist for projecting it onto a floating panel;
  the panel itself needs the renderer first.
- **World scale.** `world_scale = 100` is the starting estimate from
  `eyeheight` being a few hundred units. Measure it properly with the calibrate
  tool and correct the default.
- **Pitch sign.** `GEVR_ENGINE_PITCH_SIGN` in `gevr_engine.c` encodes which way
  the engine counts pitch. The decomp does not state it unambiguously; confirm
  it against real hardware and remove the ambiguity.
- **Weapon models.** Rendering the gun on the right controller instead of
  bolted to the view would want a hook near `bondinv`/`gunfire` model drawing.

## Testing

`vr/tests/test_controls.c` — 370 assertions, no GPU or headset:

axis encoding and the deadzone notch, radial stick shaping, angle wrapping at
the ±180° seam, quaternion yaw including gimbal lock, off-axis projection
against the symmetric case, Goodhead pad routing, snap-turn latching, servo
convergence and settling (including across the seam), `invert_pitch` polarity,
stereo eye separation and IPD scaling, view-matrix inversion, roomscale and
crouch offsets, config clamping, and the engine angle round-trips.

The servo tests model the engine's turn integrator rather than calling it, so
they run headless. The model is simple, but it is enough to catch divergence,
overshoot and sign errors — all three of which it did catch during development.
