# Ray tracing - developer notes

Working notes for whoever continues the ray tracer (including a future hardware ray tracing
backend). `doc/raytracing.md` is the user facing description; this file records the design
decisions, data layouts, hooks into the game, the pitfalls hit so far, how it was tested
without a GPU, and the plan for moving the trace pass onto RT cores.

Branch: `claude/classicube-ray-tracing-s0dbg2`. CI: `.github/workflows/build_raytracing.yml`.

## 1. What exists

| Piece | File(s) | Notes |
|-------|---------|-------|
| C module | `src/RayTracer.c`, `src/RayTracer.h` | ~1300 lines. Loads GL 4.3 functions itself (no GL headers), owns all GPU resources, dispatches the passes |
| Shaders | `misc/raytracing/*.glsl,*.comp,*.vert,*.frag` | Edited here, embedded into `src/_RayTracerShaders.h` by `misc/raytracing/embed_shaders.py` (CI fails if out of date) |
| Game hooks | `src/Game.c` | `Render3DFrame`: replaces `MapRenderer_Update/RenderNormal/RenderTranslucent` with `RayTracer_Render` + `RayTracer_RenderTranslucent`, keeps weather drawn, skips blob entity shadows. `Game_UpdateBlock` -> `RayTracer_OnBlockChanged` |
| Model hooks | `src/Model.c` | `Model_Render` -> `RayTracer_BeginEntity`; `Model_UpdateVB` and `Model_UnlockVB` -> `RayTracer_AddEntityVertices`; `Model_RenderArm` -> `BeginEntity(NULL)` so the held arm is ignored |
| Backend hook | `src/Graphics_GL1.c`, `src/Graphics_GL2.c` | `GLBackend_RestoreProgram()` rebinds the backend's shader program after our draws (GL2 caches the active program) |
| Clouds | `src/EnvRenderer.c/.h` | `EnvRenderer_CloudsTexture()` exposes the cloud texture for cloud shadows |
| Options | `src/Options.h`, `src/MenuOptions.c` | `gfx-raytracing` enum (Off/Shadows/GI/Full) in Graphics options; `rt-*` keys read once at init |
| Build | `src/Core.h` | `CC_BUILD_RAYTRACING` defined for Windows/Linux + GL1/GL2 backends unless `CC_NO_RAYTRACING` |
| Docs | `doc/raytracing.md` | user docs |
| Testing | `misc/raytracing/testing/` | headless test harness, see section 6 |

The Visual Studio project files list sources explicitly; `RayTracer.c/.h` and the generated
header were added to `src/ClassiCube.vcxproj(.filters)`.

## 2. Frame pipeline

All passes run inside `RayTracer_Render`, called where the opaque chunk pass used to be
(after sky/clouds/entities are drawn, before map edges). Water is blended in
`RayTracer_RenderTranslucent` where the translucent chunk pass used to be (after entities,
particles, selection).

1. **Entities**: quads recorded since the last frame are uploaded (`RT_UploadEntities`).
   If the local player was not drawn (first person), it is rendered once with colour and
   depth writes off just to capture its geometry.
2. **Emitters**: nearest `RT_MAX_EMITTERS` (128) light emitting blocks to the camera are
   uploaded, reselected when the camera moves > 2 blocks or the list changes.
3. **Params UBO** (`struct RTParams` <-> `Params` block in `rt_common.glsl`, std140, vec4/mat4
   only, keep both in sync field for field).
4. `rt_trace.comp` (8x8 groups): per pixel primary ray, then shading:
   - water/ice hit -> recorded as an overlay (colour premultiplied by alpha + reflection,
     distance along the ray in `albedo.a`), continuation ray behind it becomes the primary hit
   - `direct` = sun shadow ray (hard, or cone jittered in Full) + cloud shadow + entity
     shadow test + ambient floor
   - `indirect` = one cosine weighted bounce (`secondaryRadiance`, no emissive) + explicit
     emitter sample (`emitterLight`) + sky ambient on miss
   - writes: gbuf (pos, t), normal (n, block id), albedo (rgb, waterT), direct, indirect,
     water overlay. Six images: GeForce has exactly 8 image units per shader.
5. `rt_temporal.comp`: reprojects the hit position with `prevViewProj`, validates against the
   previous frame's gbuf/normal/block id, blends `indirect` (and `direct` only in soft shadow
   mode) with history (max 48 frames). Inputs are samplers, only outputs are images.
6. `rt_atrous.comp` x3 (steps 1, 2, 4): edge aware blur of indirect, weights from normal
   equality, plane distance, block id, and less blur with longer history.
7. `rt_composite.frag`: full screen triangle (`gl_VertexID`, own empty VAO), colour =
   albedo * (direct + indirect), fog identical to `EnvRenderer` (linear or exp, block fog when
   camera is inside a fogging block), writes `gl_FragDepth` from the hit position. Sky pixels
   are discarded so the rasterised sky/clouds/horizon show through.
8. `rt_water.frag` (later in the frame): reconstructs the water surface from `albedo.a`, fogs,
   blends with `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` (premultiplied), writes depth.

Ping-pong: `gbuf[2]`, `normal[2]`, `accumDir[2]`, `accumInd[2]` indexed by frame parity so the
previous frame stays readable. All screen textures are created zero filled (history is read
before it is first written).

## 3. World representation

- `worldTex`: `usampler3D`, R8UI (or R16UI when `World.Blocks2` is in use), dimensions
  (Width, Length, Height) so `blockAt(p)` fetches `(p.x, p.z, p.y)`. This matches
  `World.Blocks` layout `[y][z][x]`, so the whole array uploads with one `glTexImage3D`.
- `coarseTex`: R8UI, one byte per 8x8x8 region, 0 = all air. The DDA jumps across empty
  regions (`traceRay`). Updated per block change: set to 1 on placement; on removal the region
  is rescanned on the CPU (512 blocks).
- Block table SSBO (`struct RTBlockInfo` <-> `BlockInfo`, std430, 80 bytes): bounds, draw
  type, blocks-light flag, six tile ids, tint, full bright flag. Rebuilt on
  `BlockEvents.BlockDefChanged`. Uses `Blocks.MinBB/MaxBB` NOT `RenderMinBB/MaxBB` (see 5.7),
  lowered by 1.5/16 for liquids.
- Textures: the game's own `Atlas1D.TexIds` are bound to units 2..5 (`RT_MAX_ATLASES` = 4;
  more than that falls back to the rasteriser until the atlas changes). Tile lookup: index =
  texLoc >> Atlas1D.Shift, row = texLoc & Atlas1D.Mask, v = (row + uv.y) * InvTileSize.
- Entities: SSBO 2 = `RTEntity` (bbox + quad range), SSBO 3 = quads as 4 x vec4 world space.
  Up to 64 entities / 8192 quads per frame. Only entities in `Entities.List` are recorded.
- Emitters: SSBO 4 = ivec4 (x, y, z, block | level << 16). Full list kept on the CPU
  (`rt_em.indices`, grown with realloc), built by scanning the world, updated per block change.

Texture unit map (compute): 1 world, 2-5 atlases, 6-13 pass inputs, 14 coarse, 15 clouds.
Image units: 0-5. SSBO bindings: 1 blocks, 2 entities, 3 quads, 4 emitters. UBO binding 0.

## 4. Lighting model (keep the classic look)

- Lit face colour = `Env.SunCol` / `SunXSide` / `SunZSide` / `SunYMin`, shadowed face =
  `Env.ShadowCol` etc. `directSun()` returns `max(lit - shadow, 0) * f` where `f` normalises
  so an upward face gets the full sun colour; total for an open, lit surface equals the
  rasteriser exactly (`sun`), fully shadowed equals `shadow`.
- `rt-ambient` (0.25): part of the shadow colour that is always present so caves are not
  black; the sky bounce contributes `(1 - ambient) * shadow`.
- Water blocks light (`BlocksLight` true in the block table) so pool floors are shadowed like
  the classic lighting does - this made the water brightness match the rasteriser.
- Emitters: treated as omnidirectional point lights at the cell centre; colour = alpha
  weighted average of the face texture that faces the receiver, scaled by `rt-emissive` and
  the block's light level (larger nibble of `Blocks.Brightness`, 0..15). One emitter per pixel
  is chosen with weight 1/d^2 (weighted reservoir sampling), visibility ray stops at the
  emitter's cell so sprites/translucent/thin models work. Bounce rays return 0 on full
  bright hits to avoid double counting.
- Full bright primary hits: direct = 1, indirect = 0 (texture shown as is).

## 5. Pitfalls hit (don't repeat)

1. **NVIDIA: 8 image units per shader.** Bind pass inputs as samplers, images only for outputs.
   Mesa allows more and hides the bug.
2. **NVIDIA: constant out of range vector index is a link error** even in a dead branch
   (`texB[face - 4u]` with constant face 0). Keep every index provably in range (`& 3u`).
3. **GLSL function order**: `rand()` and friends live at the end of `rt_common.glsl`; any
   function using them must come after (`emitterLight` bit this).
4. **Windows logging**: `Platform_Log` only reaches the debugger. Use `Logger_Log` for
   `client.log` (`RT_LogToFile`).
5. **GL2 backend caches the bound program** - call `GLBackend_RestoreProgram()` after using our
   programs. Blending is cached in `_GraphicsBase.h` (`gfx_alphaBlend`); we enable/disable
   `GL_BLEND` ourselves around the water pass and restore the blend func.
6. **DDA precision**: recompute cell boundary distances from the cell coordinates every step
   (no accumulated `tMax += tDelta`), or cracks appear between blocks.
7. **Liquid render bounds are shifted** (`Block_CalcRenderBounds`: +0.1/16 in x/z, -1.5/16 in
   y). Using them in the tracer produced speckles on water (rays slipping between cells).
   Use `MinBB/MaxBB` and lower liquids by 1.5/16 only. Faces shared with an identical
   neighbour are skipped in `hitCell` (except `DRAW_TRANSPARENT_THICK`).
8. **Water as opaque depth cut off submerged entities**: water must be a blended layer drawn
   after entities, like the rasteriser's translucent pass.
9. **Weather is drawn by the translucent chunk pass**, so replacing that pass silently removed
   rain; `Game.c` now draws it in the ray traced path.
10. **CRLF**: `.c/.h` in this repo are CRLF; the shader embedder writes CRLF. Python patches
    must not normalise line endings (the first attempt turned every touched file into a
    whole-file diff).
11. **Singleplayer lava physics**: an open sided lava block floods flat ground within a minute.
    Looked exactly like a renderer bug. Test with `singleplayerphysics=false`.
12. **Windows GL2 backend does not link** (needs `-lopengl32`, upstream never ships it), so the
    Windows artifact is the GL1 build only.
13. `make ... EXTRA_CFLAGS='-DCC_COMMIT_SHA="x"'` loses the quotes; CI calls gcc directly.

## 6. Testing without a GPU (what worked)

Mesa's llvmpipe in this environment exposes OpenGL 4.5 compatibility profile with compute
shaders, so the whole pipeline runs (slowly, 3-10 fps at 800x600) under Xvfb. The harness is
in `misc/raytracing/testing/`:

- `make_scene.py OUT.cw x z yaw pitch y` - writes a 64x32x64 ClassicWorld test map (pool,
  lava, glass, slab, sprites, tree, wall, pillar, cave, recessed lava) with the spawn on a
  pillar at the given position/orientation. Yaw: 0 faces -z, 90 faces +x.
- `capture.sh MODE NAME [W H SECS]` - writes `options.txt`, starts the game on the map under
  Xvfb (`DISPLAY=:99`), waits, dumps the Xvfb framebuffer (`-fbdir`) and converts it with
  `xwd2png.py`. Extra options via `EXTRA='key=value\n'` (e.g. `rt-debug=1`,
  `singleplayerphysics=false`, `rt-sun-x=2.0` for long shadows).
- `xwd2png.py`, `sidebyside.py`, `px.py` - image helpers (no PIL needed).
- `xsend.c` - XTest input injector (`click 1` breaks the targeted block, `click 3` places,
  `key F12` screenshot) for block change tests.
- The game needs `texpacks/default.zip` next to the binary (downloaded from
  classicube.net/static/default.zip) and the Linux build needs `-L` symlinks for
  `libGL.so`/`libXi.so` if the dev packages are missing.
- `rt-debug` views (1 direct, 2 indirect, 3 albedo, 4 normals, 5 water, 6 depth, 7 block id)
  were essential; compare pixel values numerically with `px.py` rather than by eye.

Real hardware results so far (RTX 4070 Super, ~2000x1125, Full mode, multiplayer server):
60-90 fps. NVIDIA driver was the only source of shader compile differences.

## 7. Performance notes and remaining ideas

Done: coarse empty space skipping, `rt-scale`, no chunk mesh building while tracing, block
info fetched once per cell test, entity bbox culling, emitter reservoir sampling.

Not done, in rough order of value:
- Variance guided (SVGF style) denoiser; the current temporal + a-trous is fine when still,
  visibly noisy for emitter light while moving. Firefly clamp on `indirect` would help too.
- Separate accumulation for emitter light (it has different noise statistics from sky GI).
- Second bounce for caves, or an irradiance cache per coarse cell.
- Entity reflections/tinting: entities are still rasterised, so they don't appear in water
  reflections and get the rasteriser's flat lighting.
- Held block/arm and particles cast no shadows.
- Anaglyph renders twice per frame and confuses the temporal history.
- 16-bit block ids force a 2 bytes/voxel world texture (512 MB for 1024x256x1024 maps).

## 8. Hardware ray tracing (RT cores) plan

OpenGL cannot reach RT cores; Vulkan (ray query / ray tracing pipeline) or D3D12 DXR can.
Recommended route: keep the game on OpenGL and run only the trace pass in Vulkan, sharing
memory and semaphores (`GL_EXT_memory_object[_win32/_fd]`, `GL_EXT_semaphore[_win32/_fd]`
on the GL side, `VK_KHR_external_memory[_win32/_fd]`, `VK_KHR_external_semaphore[...]`).
A full Vulkan `Graphics.h` backend is 3-5x more work and touches every draw path.

What carries over unchanged: temporal, a-trous, composite and water passes (they only consume
the G-buffer and lighting images), world/coarse/block/emitter/entity data structures, the
lighting model, all game hooks, options, docs and CI. Only `rt_trace.comp` moves.

Steps:
1. Vulkan bootstrap in a new `src/RayTracer_Vulkan.c` (guarded by `CC_BUILD_RAYTRACING_VK`):
   instance, physical device matching the GL context's GPU (compare `VK_KHR_device_group`
   / device UUID with `GL_EXT_memory_object`'s device UUID), device with
   `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`,
   external memory/semaphore extensions. Load `vulkan-1.dll` / `libvulkan.so.1` dynamically
   like the GL functions are loaded (no link time dependency, keep the game buildable
   without SDKs). Fall back to the compute tracer if anything is missing.
2. Shared images: create the G-buffer/lighting images in Vulkan with exportable memory, import
   them into GL textures (`glCreateMemoryObjectsEXT`, `glTexStorageMem2DEXT`). Two
   semaphores (GL->VK "inputs ready", VK->GL "trace done") signalled/waited each frame with
   `glSignalSemaphoreEXT` / `glWaitSemaphoreEXT`.
3. Acceleration structure: one BLAS per 16x16x16 chunk from AABBs (one per exposed block face
   or one per block; per block is simplest and still cheap), procedural geometry with an
   intersection shader (ray query returns the AABB candidate, do the same slab/sprite test as
   `hitCell` in the shader). Rebuild a chunk's BLAS on block change (batched per frame), TLAS
   refit every frame. Alternative: triangles from the existing chunk mesh builder
   (`Builder.c`), which gives hardware triangle intersection but couples the tracer to the
   mesh builder that is currently skipped while tracing.
4. Port `rt_trace.comp` to a Vulkan compute shader using `rayQueryEXT` in place of
   `traceRay()`. `hitCell`, UVs, alpha test, water overlay, entity test, emitter sampling and
   all shading code stay as they are. Emitters/entities can become instances in the TLAS
   (entities as triangle BLASes) so the bespoke entity loop disappears.
5. SPIR-V: compile with `glslangValidator` at development time and embed the binaries
   (extend `embed_shaders.py`), so building the game still needs no Vulkan SDK.
6. Verify with Mesa lavapipe (`mesa-vulkan-drivers`), which supports ray queries, using the
   same Xvfb harness; performance only measurable on real hardware.

Expected gain is not primarily frames per second - the voxel DDA with empty space skipping
is already close to hardware traversal for this content - but headroom for more rays per
pixel (multi bounce, noise free soft shadows, entity reflections) and proper entity/model
geometry in reflections and shadows.
