# Blender add-on: File > Export > ClassiCube Model (.obj)
#
# Writes an OBJ in exactly the form the BlenderModel plugin expects, plus the
# extra metadata it can use, and optionally saves the model's texture next to it:
#   - one "o <name>" part per mesh object (modifiers applied, triangulated)
#   - Y up, -Z forward, 1 Blender unit = 1 block (the plugin can rescale later)
#   - "# pivot <name> x y z" using each object's origin, so animated parts
#     (Head, Arm.L, Arm.R, Leg.L, Leg.R ...) rotate about the point you chose
#   - "# eye <y>" and "# size <w> <h> <l>" from the scene custom properties
#     cc_eye_height and cc_size (add them in Scene Properties > Custom Properties)
#
# Install: Edit > Preferences > Add-ons > Install..., pick this file, enable it.
# Tested against the Blender 3.x / 4.x Python API (both normal APIs are handled).
#
# Licensed under BSD-3, same as ClassiCube.

bl_info = {
    "name": "ClassiCube Model Export",
    "author": "BlenderModel plugin",
    "version": (1, 0, 0),
    "blender": (3, 0, 0),
    "location": "File > Export > ClassiCube Model (.obj)",
    "description": "Exports mesh objects as an OBJ + PNG for the ClassiCube BlenderModel plugin",
    "category": "Import-Export",
}

import os

import bpy
import bmesh
from bpy.props import BoolProperty, FloatProperty, StringProperty
from bpy_extras.io_utils import ExportHelper
from mathutils import Matrix, Vector

# Blender is Z up with -Y forward; ClassiCube (and OBJ convention) is Y up with -Z forward.
# This rotation has determinant +1, so face winding is preserved.
AXIS_CONVERSION = Matrix(((1, 0, 0, 0),
                          (0, 0, 1, 0),
                          (0, -1, 0, 0),
                          (0, 0, 0, 1)))


def clean_name(name):
    return "".join(c if c.isalnum() or c in "._-" else "_" for c in name) or "part"


def fmt(value):
    return ("%.6f" % value).rstrip("0").rstrip(".") or "0"


def loop_normals(mesh):
    """Per-corner normals, working around the 4.1 API change."""
    if hasattr(mesh, "corner_normals"):
        return [tuple(n.vector) for n in mesh.corner_normals]
    if hasattr(mesh, "calc_normals_split"):
        mesh.calc_normals_split()
    return [tuple(loop.normal) for loop in mesh.loops]


def find_texture_image(objects):
    """First image texture used by any material of the exported objects."""
    for obj in objects:
        for slot in obj.material_slots:
            mat = slot.material
            if not mat or not mat.use_nodes:
                continue
            for node in mat.node_tree.nodes:
                if node.type == "TEX_IMAGE" and node.image:
                    return node.image
    return None


def export_model(context, filepath, selected_only, scale, export_texture, apply_modifiers):
    scene = context.scene
    depsgraph = context.evaluated_depsgraph_get()
    candidates = context.selected_objects if selected_only else scene.objects
    objects = [o for o in candidates if o.type == "MESH" and o.visible_get()]
    if not objects:
        return "No visible mesh objects to export"

    lines = ["# ClassiCube BlenderModel export from Blender %s" % bpy.app.version_string,
             "# Units: 1 unit = 1 block, Y up, model faces -Z"]

    eye = scene.get("cc_eye_height")
    if eye is not None:
        lines.append("# eye %s" % fmt(float(eye) * scale))
    size = scene.get("cc_size")
    if size is not None and len(size) == 3:
        lines.append("# size %s %s %s" % tuple(fmt(float(v) * scale) for v in size))

    names = {}
    for obj in objects:
        name = clean_name(obj.name)
        # Keep part names unique, the plugin matches pivots by name
        if name in names:
            names[name] += 1
            name = "%s_%d" % (name, names[name])
        else:
            names[name] = 1
        obj["_cc_export_name"] = name

        pivot = AXIS_CONVERSION @ obj.matrix_world.translation
        lines.append("# pivot %s %s %s %s" % (name, fmt(pivot.x * scale), fmt(pivot.y * scale), fmt(pivot.z * scale)))

    v_offset = vt_offset = vn_offset = 0
    total_tris = 0
    for obj in objects:
        name = obj["_cc_export_name"]
        del obj["_cc_export_name"]

        source = obj.evaluated_get(depsgraph) if apply_modifiers else obj
        mesh = source.to_mesh()
        try:
            mesh.transform(AXIS_CONVERSION @ obj.matrix_world)

            bm = bmesh.new()
            bm.from_mesh(mesh)
            bmesh.ops.triangulate(bm, faces=bm.faces[:])
            bm.to_mesh(mesh)
            bm.free()

            normals = loop_normals(mesh)
            uv_layer = mesh.uv_layers.active
            lines.append("o %s" % name)

            for vert in mesh.vertices:
                co = vert.co * scale
                lines.append("v %s %s %s" % (fmt(co.x), fmt(co.y), fmt(co.z)))

            uv_index = {}
            uv_ids = []
            for loop in mesh.loops:
                uv = tuple(round(c, 6) for c in uv_layer.data[loop.index].uv) if uv_layer else (0.0, 0.0)
                if uv not in uv_index:
                    uv_index[uv] = len(uv_index) + 1
                    lines.append("vt %s %s" % (fmt(uv[0]), fmt(uv[1])))
                uv_ids.append(uv_index[uv])

            n_index = {}
            n_ids = []
            for n in normals:
                key = tuple(round(c, 4) for c in n)
                if key not in n_index:
                    n_index[key] = len(n_index) + 1
                    lines.append("vn %s %s %s" % (fmt(key[0]), fmt(key[1]), fmt(key[2])))
                n_ids.append(n_index[key])

            for poly in mesh.polygons:
                corners = []
                for li in poly.loop_indices:
                    vi = mesh.loops[li].vertex_index
                    corners.append("%d/%d/%d" % (v_offset + vi + 1, vt_offset + uv_ids[li], vn_offset + n_ids[li]))
                lines.append("f " + " ".join(corners))
                total_tris += 1

            v_offset += len(mesh.vertices)
            vt_offset += len(uv_index)
            vn_offset += len(n_index)
        finally:
            source.to_mesh_clear()

    with open(filepath, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    texture_note = ""
    if export_texture:
        image = find_texture_image(objects)
        png_path = os.path.splitext(filepath)[0] + ".png"
        if image is None:
            texture_note = ", no image texture found so no PNG was written"
        else:
            copy = image.copy()
            try:
                copy.filepath_raw = png_path
                copy.file_format = "PNG"
                copy.save()
                texture_note = ", texture saved to %s" % os.path.basename(png_path)
            finally:
                bpy.data.images.remove(copy)

    return "Exported %d parts, %d triangles%s" % (len(objects), total_tris, texture_note)


class EXPORT_OT_classicube_model(bpy.types.Operator, ExportHelper):
    bl_idname = "export_scene.classicube_model"
    bl_label = "Export ClassiCube Model"
    bl_options = {"PRESET"}
    filename_ext = ".obj"
    filter_glob: StringProperty(default="*.obj", options={"HIDDEN"})

    selected_only: BoolProperty(name="Selected objects only", default=False)
    apply_modifiers: BoolProperty(name="Apply modifiers", default=True)
    export_texture: BoolProperty(name="Save texture PNG next to the OBJ", default=True)
    scale: FloatProperty(name="Scale (blocks per unit)", default=1.0, min=0.001, max=1000.0)

    def execute(self, context):
        message = export_model(context, self.filepath, self.selected_only, self.scale,
                               self.export_texture, self.apply_modifiers)
        if message.startswith("No "):
            self.report({"ERROR"}, message)
            return {"CANCELLED"}
        self.report({"INFO"}, message)
        return {"FINISHED"}


def menu_func_export(self, context):
    self.layout.operator(EXPORT_OT_classicube_model.bl_idname, text="ClassiCube Model (.obj)")


def register():
    bpy.utils.register_class(EXPORT_OT_classicube_model)
    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)
    bpy.utils.unregister_class(EXPORT_OT_classicube_model)


if __name__ == "__main__":
    register()
