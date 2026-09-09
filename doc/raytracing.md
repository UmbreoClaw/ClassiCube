# Ray tracing

ClassiCube can render the world with a GPU path tracer instead of the usual chunk meshes.
It provides:

* ray traced sun shadows (optionally soft)
* one bounce of global illumination (light bouncing off blocks, coloured by them)
* emissive light from full bright blocks such as lava
* mirror reflections on water and other translucent blocks

Everything else (sky, clouds, entities, particles, held block, GUI) is still drawn by the
normal renderer, and composites correctly on top of the traced world because the ray tracer
writes depth.

## How it works

The world is a voxel grid, so the tracer walks rays directly through a 3D texture of block ids
(a 3D DDA) instead of building a triangle acceleration structure. That means there is no
BVH/BLAS to rebuild when blocks change - a block edit is a single voxel upload - and it runs
on any GPU with OpenGL 4.3 compute shaders. RT cores are not required or used: for a blocky
world the voxel walk is already the fast path, and the visual result is the same.

Per frame:

1. `rt_trace.comp` traces one camera ray per pixel, then a shadow ray towards the sun,
   a cosine weighted bounce ray (global illumination), and for water a reflection ray.
   It writes a G-buffer (position, normal, block) and separate lighting terms.
2. `rt_temporal.comp` reprojects each pixel into the previous frame and accumulates the
   noisy terms over time when the same surface was visible there.
3. `rt_atrous.comp` runs an edge aware blur (3 passes) over the indirect light.
4. `rt_composite.frag` combines albedo, direct, indirect and reflection terms, applies the
   same fog as the rasteriser, and writes colour + depth.

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

`CC_GFX_BACKEND_GL2` (the modern OpenGL backend) also works.

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
| `rt-emissive`    | 2.0     | How strongly full bright blocks (lava etc) light their surroundings  |
| `rt-gi-distance` | 48      | Maximum length of bounce rays in blocks                              |
| `rt-debug`       | 0       | 1 direct light, 2 indirect, 3 albedo, 4 normals, 5 reflections/behind, 6 depth |

Larger view distances cost more, since sky rays walk further through empty air before
giving up. If the frame rate drops in very open maps, lower the view distance.

## Limitations / future work

* Lighting from the *fancy* lighting mode (lamp/lava light levels) is not used; full bright
  blocks emit light through the global illumination bounce instead.
* Custom block models are traced as their bounding box; sprites as crossed quads.
* Entities, particles and the held block are rasterised and do not cast ray traced shadows.
* Only one bounce of indirect light is traced. Deep caves rely on `rt-ambient`.
* The map border/edge water outside the map is still rasterised.
* Two-level empty space skipping and a variance guided denoiser would make it faster and
  cleaner still; the current denoiser is temporal accumulation plus a 3 pass a-trous blur.
* Anaglyph 3D renders the world twice per frame, which confuses the temporal history.
