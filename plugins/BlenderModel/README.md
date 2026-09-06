# BlenderModel plugin

A client-side ClassiCube plugin that lets you use a model made in Blender as your
player model, in both third person (the whole mesh) and first person (the arm).

It adds a model named `blender`, loads it from an OBJ + PNG on disk, and can set it
as your model automatically. Nothing is sent to the server: other players still
see whatever model the server has given you.

## Quick start

1. Build the plugin (see below) and put `BlenderModel.so` / `.dll` / `.dylib` in
   the game's `plugins` folder.
2. In Blender, install `blender_export_classicube.py` as an add-on and use
   **File > Export > ClassiCube Model (.obj)**. This writes `model.obj` and
   `model.png` with the right axes, triangulation, part names and pivots.
   (Blender's built-in OBJ exporter with its default settings also works, you just
   lose the pivot metadata.)
3. Put both files in `blendermodel/model.obj` and `blendermodel/model.png` inside
   the game folder (or point `blendermodel-file` in `options.txt` elsewhere).
4. Start the game. The model is applied when a map loads. Press F5 to see it in
   third person.

The `example/` folder has a ready-made blocky robot to try first
(`python3 example/make_example.py` regenerates it).

### Chat commands

| Command | What it does |
| --- | --- |
| `/client blendermodel load [file]` | Loads an OBJ (and `file.png`), remembers the path |
| `/client blendermodel apply` | Switches your model to the loaded one |
| `/client blendermodel reset` | Back to the humanoid model |
| `/client blendermodel info` | Lists parts, how each one animates, triangle counts |
| `/client blendermodel scale <n>` | Blocks per OBJ unit (reloads the file) |
| `/client blendermodel armscale <n>` | Extra scale for the first person arm |
| `/client blendermodel auto on/off` | Apply the model on every map load |
| `/client blendermodel force on/off` | Re-apply it if the server changes your model |

All of these persist in `options.txt` (`blendermodel-*` keys).

## Preparing the model in Blender

* **One object per body part.** Each mesh object becomes an `o` part. Part names
  decide how a part animates, matching is case-insensitive and looks for these
  words: `head`, `arm`/`hand`, `leg`/`foot`. Sides come from `left`/`right` or a
  `.L`/`.R`, `_L`/`_R`, `-L`/`-R` affix (`Arm.L`, `leg_r`, `LeftHand`, ...).
  Everything else is static. An arm with no side counts as the right arm.
* **Pivots.** An animated part rotates about its object origin (the orange dot),
  so put the origin at the shoulder, hip or neck. Without the metadata line the
  plugin guesses: top centre of the part for limbs, bottom centre for heads.
* **Scale and orientation.** 1 Blender unit = 1 block, feet at Y = 0 (Blender Z = 0),
  face -Y in Blender (which becomes -Z in game). The humanoid is 2 blocks tall.
* **First person.** Only the right arm part is drawn in first person. It is moved so
  its pivot sits where the humanoid shoulder is, then posed with the same angles
  the humanoid arm uses. Use `armscale` if it looks too big or small.
* **Texture.** One PNG for the whole model, UV-mapped. Non power-of-two sizes are
  padded automatically. Pixels with alpha 0 are cut out (alpha test); there is no
  translucency.
* **Optional metadata**, on `#` comment lines anywhere in the OBJ:
  `# pivot <part> x y z`, `# eye <y>` (camera height), `# size <w> <h> <l>`
  (collision box, defaults to the humanoid's so you still fit through gaps).
  The exporter fills these from the scene custom properties `cc_eye_height` and
  `cc_size`.

## Constraints you will run into

These come from how ClassiCube renders entities, not from the plugin:

* **No skeletal animation.** ClassiCube has no bones, weights or animation clips.
  Entity models are rigid parts rotated about a pivot by procedural walk/swing
  angles. The plugin maps those angles onto your named parts, that is all the
  animation you get. Blender armature animations, shape keys and IK are not
  exported.
* **Static triangle meshes only.** The GPU layer draws textured, vertex-coloured
  quads through a fixed index buffer, with no normals, shaders or per-pixel
  lighting. Triangles are uploaded as degenerate quads, and the game's face
  shading (top bright, bottom dark, sides in between) is approximated per vertex
  from the normals, then baked into the vertex buffer.
* **File formats.** The game has a PNG decoder and nothing else, so `.blend`,
  glTF or FBX can't be read directly. OBJ is a simple text format that needs no
  library, hence the exporter.
* **One texture.** There is one texture bind per model, so multiple materials must
  be baked to a single atlas in Blender first.
* **Size limits.** 16-bit indices cap each vertex buffer at 65 535 vertices, so the
  mesh is split into chunks of 16 383 triangles (up to 64 chunks). Animated parts
  share one dynamic buffer and are re-uploaded every frame, so keep them under
  16 383 triangles combined; extra animated parts fall back to static. In practice
  keep the whole model in the low thousands of triangles: the game targets very
  old hardware and the software renderer draws every triangle on the CPU.
* **Lighting is per entity.** Static chunks are rebuilt when the entity colour
  changes (walking into shadow). If several entities use the `blender` model with
  different lighting, the chunks are rebuilt every frame.
* **Client-side only.** The server can set your model at any time (`/model` on most
  servers, or the ChangeModel packet); `force on` puts yours back within half a
  second. Server-side custom models (the CustomModels extension) are box-only and
  are a separate, server-driven feature.
* **Native plugin.** Plugins are shared libraries, so this works on the desktop
  builds. The web client and the console ports cannot load plugins.
* **Collision and camera.** Your hitbox and eye height come from the model, so
  a very tall or wide model changes how you fit through gaps and where the camera
  sits. Defaults keep the humanoid's collision size; `# size` overrides it.

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
`make terminal` build of the game (no GPU or display needed) and checks OBJ
parsing, part classification, pivots, texture padding and the rotation maths:

```
cd /path/to/ClassiCube && make terminal
cd plugins/BlenderModel && make test
```
