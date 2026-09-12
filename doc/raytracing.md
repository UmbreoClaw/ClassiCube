# Ray tracing

ClassiCube can render the world with a GPU path tracer instead of the usual chunk meshes.
It provides:

* ray traced sun shadows (optionally soft), cast by blocks, clouds, players and other entities
* one bounce of global illumination (light bouncing off blocks, coloured by them)
* light from full bright blocks such as lava and lamps, sampled explicitly so it spreads
  smoothly around them
* mirror reflections on water and other translucent blocks

Everything else (sky, clouds, entities, particles, held block, GUI) is still drawn by the
normal renderer, and composites correctly on top of the traced world because the ray tracer
writes depth.

## How it works

The world is a voxel grid, so the tracer walks rays directly through a 3D texture of block ids
(a 3D DDA) instead of building a triangle acceleration structure. A second, coarse texture
marks which 8x8x8 regions contain any blocks at all, so rays crossing open air (sky rays,
long shadow rays) jump a whole region at a time instead of stepping block by block. That
means there is no BVH/BLAS to rebuild when blocks change - a block edit is a single voxel
upload plus a one byte update of its region - and it runs on any GPU with OpenGL 4.3
compute shaders. RT cores are not required or used: for a blocky
world the voxel walk is already the fast path, and the visual result is the same.

Per frame:

1. `rt_trace.comp` traces one camera ray per pixel, then a shadow ray towards the sun,
   a cosine weighted bounce ray (global illumination), a ray towards one nearby emitting
   block (chosen with probability proportional to 1/distance^2), and for water a reflection
   ray. It writes a G-buffer (position, normal, block) and separate lighting terms. Water is
   recorded as a separate translucent layer; the surface behind it is the primary hit.
2. `rt_temporal.comp` reprojects each pixel into the previous frame and accumulates the
   noisy terms over time when the same surface was visible there.
3. `rt_atrous.comp` runs an edge aware blur (3 passes) over the indirect light.
4. `rt_composite.frag` combines albedo, direct and indirect terms, applies the same fog as
   the rasteriser, and writes colour + depth for the opaque world.
5. After entities and particles have been drawn, `rt_water.frag` blends the water layer
   (with its reflection) over them, so a player standing in water is visible through it.

Shadow rays also test the cloud layer (the scrolling cloud texture at the cloud height) and
the entity models drawn this frame. Entity geometry is captured as the game draws each model
(`Model.c` hands the transformed quads to `RayTracer_AddEntityVertices`), including the local
player's own model in first person, which the game otherwise never draws.

The lighting model deliberately reproduces the classic look: a face in full sun gets the
environment's sun colour, a face that only sees the sky gets the shadow colour, and the usual
per-side shading (`SunXSide`, `SunZSide`, `SunYMin`) is kept. Ray tracing then adds real
occlusion, bounce light and reflections on top.

Sources:

* `src/RayTracer.c` / `src/RayTracer.h` - OpenGL setup, world/block upload, frame dispatch
* `misc/raytracing/*.glsl|comp|vert|frag` - the shaders
* `src/_RayTracerShaders.h` - generated from the shaders by `misc/raytracing/embed_shaders.py`.
  Re-run that script after editing any shader (`python3 misc/raytracing/embed_shaders.py`).

## Requirements

* A GPU and driver with OpenGL 4.3 or newer (any NVIDIA GeForce 400 series or later, AMD GCN,
  Intel HD 4000 or later, Mesa 22+). On an RTX 4070 Super it runs comfortably at 1440p.
* The **OpenGL** build of the game. On Windows the default graphics backend is Direct3D, which
  cannot run the compute shaders, so the game must be compiled with the OpenGL backend
  (see below). Linux builds already use OpenGL.
* The map must fit within the GPU's 3D texture limit (16384 on modern GPUs, usually 2048 at least).

Ray tracing is compiled in automatically for Windows and Linux OpenGL builds. Define
`CC_NO_RAYTRACING` to leave it out.

## Building on Windows with the OpenGL backend

##### MinGW-w64 / MSYS2

```
make mingw RELEASE=1 EXTRA_CFLAGS="-DCC_GFX_BACKEND=CC_GFX_BACKEND_GL1"
```

##### Visual Studio (command line)

```
cl.exe /O2 /DCC_GFX_BACKEND=CC_GFX_BACKEND_GL1 src\*.c third_party\bearssl\*.c /link user32.lib gdi32.lib winmm.lib dbghelp.lib shell32.lib comdlg32.lib /out:ClassiCube.exe
```

##### Visual Studio (IDE)

Add `CC_GFX_BACKEND=CC_GFX_BACKEND_GL1` to *Project Properties -> C/C++ -> Preprocessor ->
Preprocessor Definitions*, then build as usual.

`CC_GFX_BACKEND_GL2` (the modern OpenGL backend) also works on Linux; on Windows use `GL1`.

## Enabling it

*Options -> Graphics options -> Ray tracing*:

| Mode      | What you get                                                        |
|-----------|---------------------------------------------------------------------|
| Off       | Normal rasterised rendering (default)                               |
| Shadows   | Ray traced sun shadows                                              |
| GI        | Shadows plus one bounce global illumination and emissive blocks     |
| Full      | GI plus soft shadows and reflections on water                       |

The setting is saved as `gfx-raytracing` in `options.txt`. If the driver or GPU can't run the
shaders, the game falls back to normal rendering and prints why in chat and `client.log`.

Advanced settings (edit `options.txt`, no menu entry):

| Option           | Default | Meaning                                                              |
|------------------|---------|----------------------------------------------------------------------|
| `rt-sun-x`       | 0.35    | Horizontal X component of the direction towards the sun              |
| `rt-sun-z`       | 0.20    | Horizontal Z component of the direction towards the sun              |
| `rt-sun-radius`  | 0.04    | Angular size of the sun for soft shadows (0 = hard shadows)          |
| `rt-ambient`     | 0.25    | Minimum ambient light so caves are not pitch black (0 - 1)           |
| `rt-block-light` | 1.0     | Multiplier for the light cast by full bright blocks (lava, lamps, server defined lights) |
| `rt-cloud-shadow`| 0.6     | How much sunlight clouds block (0 = no cloud shadows, 1 = full)     |
| `rt-gi-distance` | 48      | Maximum length of bounce rays in blocks                              |
| `rt-scale`       | 100     | Render the traced world at this percentage of the window size (25 - 100) and upscale. Also in the menu as *RT resolution* |
| `rt-gi-rate`     | 2       | 2 = trace global illumination for half the pixels each frame (checkerboard), 1 = every pixel. Also in the menu as *RT GI quality* |
| `rt-debug`       | 0       | 1 direct light, 2 indirect, 3 albedo, 4 normals, 5 reflections/behind, 6 depth |

Cost is dominated by resolution and by how far rays travel. If the frame rate drops:

1. *RT resolution* 75% or 50% in the graphics menu. The traced world is upscaled; GUI, text,
   entities and particles stay at full resolution. 75% roughly doubles the frame rate, 50%
   roughly quadruples it, and at 1440p 75% is hard to tell apart from 100%.
2. *RT GI quality* Half (the default) traces bounce/emitter rays for half the pixels per frame.
3. *Shadows* mode instead of GI/Full skips the bounce and emitter rays entirely.
4. Lower the view distance; sky rays walk up to the view distance before giving up.

While ray tracing is active the chunk meshes are not built, which also frees CPU time.

## Automated builds

`.github/workflows/build_raytracing.yml` builds the OpenGL variants of the game on every push to
`main`/`master`, any `claude/*` branch and `raytracing*` branches, or on demand from the
Actions tab (*Build ray tracing (Windows + Linux)* -> *Run workflow*). It also checks that
`src/_RayTracerShaders.h` is in sync with the shader sources.

The workflow uploads these artifacts (download from the run's page on GitHub, then unzip):

| Artifact                            | Contents                                            |
|-------------------------------------|-----------------------------------------------------|
| ClassiCube-Win64-OpenGL-RayTracing  | 64 bit Windows, OpenGL backend (use this one)       |
| ClassiCube-Win32-OpenGL-RayTracing  | 32 bit Windows, OpenGL backend                      |
| ClassiCube-Linux64-OpenGL-RayTracing| 64 bit Linux                                        |

## Limitations / future work

* Where several glowing blocks are in range the strongest one wins (as the game's flood fill
  does), so two lamps side by side are not brighter than one.
* Block light follows the *fancy* lighting model (light level, lamp/lava colours from the
  environment) with real occlusion, but is estimated per pixel and denoised, so it flickers
  slightly while moving.
* Custom block models are traced as their bounding box; sprites as crossed quads.
* Entities are rasterised (not ray traced themselves), so they receive the game's own
  lighting (with *classic* lighting a player next to a lamp stays dark while the blocks around
  are lit; use *fancy* lighting for consistent results) and don't appear in reflections.
  Particles and the held block cast no shadows.
* Only one bounce of indirect light is traced. Deep caves rely on `rt-ambient`.
* The map border/edge water outside the map is still rasterised.
* A variance guided denoiser (SVGF style) would make the indirect light cleaner while moving;
  the current denoiser is temporal accumulation plus a 3 pass a-trous blur.
* Anaglyph 3D renders the world twice per frame, which confuses the temporal history.
