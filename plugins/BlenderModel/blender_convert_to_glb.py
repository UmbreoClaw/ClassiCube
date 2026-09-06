# Converts a model file Blender can import (.dae, .fbx, .obj, .x3d, .ply, .stl,
# .abc, .usd*) into a .glb for the BlenderModel plugin, without opening the UI.
#
#   blender -b --python blender_convert_to_glb.py -- input.dae output.glb [--no-anim]
#
# The armature, skin weights and animation actions/clips of the input are kept
# (glTF stores every action as a separate animation, which the plugin picks by
# name: idle / walk / jump / fly). Textures are embedded in the .glb.
#
# Licensed under BSD-3, same as ClassiCube.
import os
import sys

import bpy


def main():
    if "--" not in sys.argv:
        print("usage: blender -b --python blender_convert_to_glb.py -- input output.glb [--no-anim]")
        sys.exit(1)
    args = sys.argv[sys.argv.index("--") + 1:]
    if len(args) < 2:
        print("need an input and an output path")
        sys.exit(1)
    src, dst = os.path.abspath(args[0]), os.path.abspath(args[1])
    export_anim = "--no-anim" not in args

    bpy.ops.wm.read_factory_settings(use_empty=True)
    ext = os.path.splitext(src)[1].lower()
    if ext == ".dae":
        bpy.ops.wm.collada_import(filepath=src)
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=src)
    elif ext == ".obj":
        bpy.ops.wm.obj_import(filepath=src)
    elif ext == ".x3d" or ext == ".wrl":
        bpy.ops.import_scene.x3d(filepath=src)
    elif ext == ".ply":
        bpy.ops.wm.ply_import(filepath=src)
    elif ext == ".stl":
        bpy.ops.wm.stl_import(filepath=src)
    elif ext == ".abc":
        bpy.ops.wm.alembic_import(filepath=src)
    elif ext in (".usd", ".usda", ".usdc", ".usdz"):
        bpy.ops.wm.usd_import(filepath=src)
    else:
        print("don't know how to import %s" % ext)
        sys.exit(1)

    kwargs = dict(filepath=dst, export_format="GLB", export_yup=True, export_apply=True,
                  export_animations=export_anim, export_skins=True, export_extras=True,
                  export_texcoords=True, export_normals=True, export_materials="EXPORT")
    try:
        bpy.ops.export_scene.gltf(**kwargs)
    except TypeError:
        # Older/newer exporters differ in accepted keywords; fall back to the essentials
        bpy.ops.export_scene.gltf(filepath=dst, export_format="GLB", export_animations=export_anim)
    print("wrote %s" % dst)


if __name__ == "__main__":
    main()
