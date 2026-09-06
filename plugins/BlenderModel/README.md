# BlenderModel plugin

A client-side ClassiCube plugin that lets you use a model made in Blender as your
player model, in both third person (the whole mesh) and first person (the arm).

It adds a model named `blender`, loads it from disk, and can set it as your model
automatically. Nothing is sent to the server: other players still see whatever
model the server has given you.

Two formats are supported:

| Format | What you get |
| --- | --- |
| **glTF** (`.glb` or `.gltf`) | Rigged, skinned meshes with real animation clips from Blender (walk, idle, jump, ...). Bones are evaluated on the CPU every frame, the same way the Classic64 plugin draws Mario through libsm64. |
| **OBJ** (`.obj` + `.png`) | Rigid parts. Parts named like head / arm / leg swing with ClassiCube's own humanoid animation. Simplest to make, no rig needed. |

## Quick start

1. Build the plugin (see below) and put `BlenderModel.so` / `.dll` / `.dylib` in
   the game's `plugins` folder.
2. Export from Blender:
   * **glTF (animated):** File > Export > glTF 2.0, format *glTF Binary (.glb)*,
     with *Animation* and *Skinning* enabled and *+Y Up* on (the defaults). Enable
     *Include > Custom Properties* if you set `cc_eye_height` / `cc_size` (see below).
   * **OBJ (rigid parts):** install `blender_export_classicube.py` as an add-on and
     use File > Export > ClassiCube Model (.obj). It writes the OBJ with the right
     axes, one part per object, pivot metadata, and saves the texture PNG.
3. Put the file in `blendermodel/model.glb` (or `model.obj` + `model.png`) inside the
   game folder, or run `/client blendermodel load <path>` in game.
4. Start the game. The model is applied when a map loads. Press F5 for third person.

The `example/` folder has a ready-made blocky OBJ robot to try first
(`python3 example/make_example.py` regenerates it).

### Chat commands

| Command | What it does |
| --- | --- |
| `/client blendermodel load [file]` | Loads an OBJ/GLB/glTF file (and `file.png` for OBJ), remembers the path |
| `/client blendermodel apply` | Switches your model to the loaded one |
| `/client blendermodel reset` | Back to the humanoid model |
| `/client blendermodel info` | Lists parts or bones, clips, triangle counts |
| `/client blendermodel anim <name/auto>` | Forces one glTF clip, or back to automatic selection |
| `/client blendermodel scale <n>` | Blocks per model unit (reloads the file) |
| `/client blendermodel armscale <n>` | Extra scale for the first person arm |
| `/client blendermodel animspeed <n>` | Playback speed of glTF clips |
| `/client blendermodel flip on/off` | Turns the model around 180 degrees, for files that face the wrong way |
| `/client blendermodel headlook on/off/invert` | Whether the head bone follows the camera (glTF) |
| `/client blendermodel auto on/off` | Apply the model on every map load |
| `/client blendermodel force on/off` | Re-apply it if the server changes your model |

All of these persist in `options.txt` (`blendermodel-*` keys).

## glTF models (rigged and animated)

* **Clips are chosen by name.** The plugin looks for clips whose names contain
  `idle`/`stand`, `walk`/`run`, `jump`/`fall` and `fly`/`float`/`swim`, and picks
  one from the entity's state: airborne, flying, moving, or standing still. Each
  Blender action becomes one glTF animation, so name your actions accordingly.
  A model with only one clip just loops it. Use `anim <name>` to force a clip.
* **Head bone** (name contains `head`) is turned to follow the camera pitch and the
  head/body yaw difference, like the humanoid head. If it nods the wrong way use
  `headlook invert`; Blender bone axes vary between rigs.
* **First person arm** is every triangle whose strongest bone is under the topmost
  bone named like a right arm (`upper_arm.R`, `RightArm`, `hand.R`, ...), posed in
  the rest pose, then placed and rotated like the humanoid arm.
* **Orientation.** glTF's front is +Z and the game's is -Z, so glTF files are turned
  around on load. A character built facing Blender's front view comes out right.
  `flip on` undoes the turn if your file was modelled differently.
* **Eye height and hitbox** come from scene custom properties `cc_eye_height`
  (number) and `cc_size` (3 numbers), exported as glTF `extras` when *Custom
  Properties* is enabled. Otherwise the eye is at 81% of the model height and the
  collision box stays the humanoid's.
* **Supported:** `.glb` and `.gltf` (external `.bin`/`.png` or base64 data URIs),
  TRS and matrix nodes, several meshes and primitives, up to 4 bones per vertex,
  LINEAR and STEP animation (CUBICSPLINE is played linearly through its keys),
  embedded or external PNG base colour texture from the first textured material.
  Not supported: morph targets, sparse accessors, non-PNG textures (convert to PNG),
  Draco/meshopt compression, KHR extensions.

## OBJ models (rigid parts)

* **One object per body part.** Each mesh object becomes an `o` part. Part names
  decide how a part animates, matching is case-insensitive and looks for these
  words: `head`, `arm`/`hand`, `leg`/`foot`. Sides come from `left`/`right` or a
  `.L`/`.R`, `_L`/`_R`, `-L`/`-R` affix (`Arm.L`, `leg_r`, `LeftHand`, ...).
  Everything else is static. An arm with no side counts as the right arm.
* **Pivots.** An animated part rotates about its object origin (the orange dot),
  so put the origin at the shoulder, hip or neck. Without the metadata line the
  plugin guesses: top centre of the part for limbs, bottom centre for heads.
* **Scale and orientation.** 1 Blender unit = 1 block, feet at Z = 0 in Blender,
  character facing -Y (Blender's front view). The add-on maps this to the game.
  If you use Blender's stock OBJ exporter instead, set *Forward: Z, Up: Y* in the
  export dialog, or leave the defaults and turn `flip on` in game.
* **Optional metadata**, on `#` comment lines anywhere in the OBJ:
  `# pivot <part> x y z`, `# eye <y>` (camera height), `# size <w> <h> <l>`
  (collision box). The add-on fills these from the scene custom properties
  `cc_eye_height` and `cc_size`.

Both formats: one PNG texture for the whole model (bake multiple materials to an
atlas in Blender). Non power-of-two sizes are padded automatically. Alpha 0 pixels
are cut out; there is no translucency.

## What about .dae (COLLADA), .fbx, .smd, ...?

Not read directly. COLLADA is a large XML schema (and every exporter writes a
different dialect of it), so a robust parser would be several times the size of
the glTF one for the same information: mesh, skin weights, bone hierarchy and
animation clips. glTF carries all of that in a compact, well-specified form that
Blender exports by default, which is why it is the animated format here.

Converting is two clicks in Blender (File > Import > Collada, then File > Export >
glTF 2.0), or one command without opening the UI:

```
blender -b --python blender_convert_to_glb.py -- model.dae model.glb
```

`blender_convert_to_glb.py` accepts `.dae`, `.fbx`, `.obj`, `.x3d`, `.ply`, `.stl`,
`.abc` and `.usd*`, keeps the armature, weights and actions, and embeds the
textures in the `.glb`.

## Constraints you will run into

These come from how ClassiCube renders entities, not from the plugin:

* **Animation is CPU work.** ClassiCube has no bones or animation clips of its own;
  entity models are rigid parts rotated about a pivot. Skinned glTF models are
  posed and re-uploaded every frame by the plugin, so the cost scales with
  triangles times bones-per-vertex. A few thousand triangles is comfortable; tens of
  thousands will hurt on weak machines and the software renderer.
* **Static triangle meshes only on the GPU side.** The renderer draws textured,
  vertex-coloured quads through a fixed index buffer, with no normals, shaders or
  per-pixel lighting. Triangles are uploaded as degenerate quads, and the game's
  face shading (top bright, bottom dark, sides in between) is approximated per
  vertex from the normals. Normal maps, PBR materials, transparency and multiple
  textures do not exist.
* **Size limits.** 16-bit indices cap each vertex buffer at 65 535 vertices, so
  meshes are split into chunks of 16 383 triangles (up to 64 chunks). For OBJ files
  the animated parts share one chunk; extra animated parts fall back to static.
* **Lighting is per entity.** For OBJ files static chunks are rebuilt when the entity
  colour changes (walking into shadow). If several entities use the `blender` model
  with different lighting, those chunks are rebuilt every frame. glTF models are
  re-uploaded every frame anyway.
* **Client-side only.** The server can set your model at any time (`/model` on most
  servers, or the ChangeModel packet); `force on` puts yours back within half a
  second. Server-side custom models (the CustomModels extension) are box-only and
  are a separate, server-driven feature. Other players need the plugin and your
  file to see the same thing, and even then nothing syncs it yet.
* **Native plugin.** Plugins are shared libraries, so this works on the desktop
  builds. The web client and the console ports cannot load plugins.
* **Collision and camera.** Your hitbox and eye height come from the model, so a
  very tall or wide model changes how you fit through gaps and where the camera
  sits. Defaults keep the humanoid's collision size; `cc_size` / `# size` override it.

## Building

The game's headers are needed. From this folder, with the repository root two
levels up:

```
make            # Linux/BSD: BlenderModel.so
make macos      # BlenderModel.dylib
make mingw64    # Windows 64 bit, needs libClassiCube.a next to the Makefile
make mingw32    # Windows 32 bit
```

`libClassiCube.a` for Windows is generated from `ClassiCube.exe` with `gendef` and
`dlltool` as described in `doc/plugin-dev.md`. Set `GAME_ROOT=/path/to/ClassiCube`
if this folder lives elsewhere.

### Tests

`make test` builds a loader test against the object files of a headless
`make terminal` build of the game (no GPU or display needed). It generates a small
skinned glTF model and checks OBJ and glTF parsing, JSON, bone classification,
pivots, texture padding, skinning, animation sampling and clip selection:

```
cd /path/to/ClassiCube && make terminal
cd plugins/BlenderModel && make test
```
