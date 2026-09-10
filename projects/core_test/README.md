# Core test project

Run from the repository root:

```sh
make -f Makefile.core run
```

Press Tab to release or capture the pointer. With the pointer released, right-click anywhere for the
nested menu, drag the `Core UI` panel by its title bar, and use its button, recessed slider and checkbox.
The two square modules at the right are fixed tiles snapped into a dock column; the lower tile contains
three flush transport buttons inside one shallow indent.
The UI text uses the external 8x16 GNU Unifont BDF at native size with point filtering.

Or launch the built executable directly:

```sh
./build/core/core_test
```

The scene contains a textured, animated CC0 character, a ground plane built with MB, and a sampled-depth inset. Rendering goes through an RGBA8 colour target with a depth **texture**. The shaders use GLSL 120; the textured shader is unlit so the sample's painted texture is directly visible.


Controls:

- WASD moves; Space/Left Ctrl moves vertically; Shift accelerates; mouse looks.
- Tab releases/captures the mouse; Escape exits.
- 1 cycles clips; 2 plays a one-shot; 3 blends to clip 0, frame 0 and holds.
- L toggles additive aim; H collapses the upper-body chain; F toggles a fixed bone correction.
- V displays a miniature character through the shared viewmodel projection function.
- F1 toggles depth preview. N switches noclip/ground movement; C crouches in ground mode.

The default loop uses fixed updates at 60 Hz. `--variable` selects `fixed_dt=0`; `--slow-fixed` uses 20 Hz to make render interpolation visible.

```sh
make -f Makefile.core smoke
./build/core/core_test --smoke --variable
./build/core/core_test --smoke --slow-fixed
```

Smoke mode runs the same project for 100 rendered frames, reports checks and returns nonzero on failure. It checks input consumption, animation matrices against raylib, explicit blend destinations, one-shot completion, bone pivots/collapse, changing GPU-rendered poses, unchanged CPU animated vertices, and depth sampling. Scene, depth, UI and nested-menu captures plus build products go in `build/core/`. There is no separate test suite.

The build needs a C compiler, make, git, Python 3, tar and the Linux/X11 development dependencies used by raylib. A working X display is needed to run it. It exports raylib's committed source into the build directory and builds it with `GRAPHICS_API_OPENGL_21`; neither vendored files nor the existing game build are changed. This excludes the existing uncommitted game-specific `raylib/src/config.h` decoder reductions.

The character is `greenman.glb` by @iP, CC0; see `assets/LICENSE`. It is copied unchanged from raylib's sample assets and loaded using raylib's normal model/animation loaders. Its texture is in the native GLB asset, not embedded in the executable. The project explicitly supplies `1000/17` frames per second to match this version of raylib's glTF sampling interval. Core has no format-specific playback rate.

## Known limits

GPU skinning and sampleable depth are required. Startup logs limits and checks them against the shader budget. `CORE_BONE_CAPACITY` in `core/config.h` is the single capacity definition; the disk shader receives it from the shader loader. Below-limit machines and invalid required shaders/targets fail; no CPU or encoded-colour-depth fallback is implemented.

Forcing `MESA_GL_VERSION_OVERRIDE=2.1 MESA_GLSL_VERSION_OVERRIDE=120` can leave a cosmetic `GL_INVALID_ENUM` from raylib initialization. Core logs and clears pre-existing initialization errors before its own checks. No raylib patch is applied. The override still uses the modern driver's capabilities and is not a minimum-spec hardware test.

Bone hiding collapses a chain; triangles weighted partly to surviving bones can stretch instead of disappearing. Pose interpolation uses local TRS and quaternion slerp. It inherits raylib's TRS representation, including its inability to represent arbitrary shear. `ActorMatLerp` is only a componentwise matrix helper and is not used for skeletal blending.
