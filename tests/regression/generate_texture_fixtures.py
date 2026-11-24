"""Optional fixture authoring: blender --background --factory-startup --python this_file.

Tests consume the checked-in FBX/GLB files; running the tests does not require Blender.
"""
from pathlib import Path
import bpy

root = Path(__file__).resolve().parent / "fixtures/model-textures"
root.mkdir(parents=True, exist_ok=True)
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
bpy.ops.mesh.primitive_cube_add()
obj = bpy.context.object
image = bpy.data.images.new("diffuse", width=2, height=2)
image.pixels.foreach_set([.8, .2, .1, 1]*4)
image.filepath_raw = str(root / "diffuse.png")
image.file_format = 'PNG'
image.save()
material = bpy.data.materials.new("Textured")
material.use_nodes = True
texture = material.node_tree.nodes.new("ShaderNodeTexImage")
texture.image = image
material.node_tree.links.new(texture.outputs["Color"], material.node_tree.nodes["Principled BSDF"].inputs["Base Color"])
obj.data.materials.append(material)
bpy.ops.export_scene.fbx(filepath=str(root / "external.fbx"), use_selection=True, path_mode='RELATIVE', bake_anim=False)
bpy.ops.export_scene.fbx(filepath=str(root / "embedded.fbx"), use_selection=True, path_mode='COPY', embed_textures=True, bake_anim=False)
bpy.ops.export_scene.gltf(filepath=str(root / "embedded.glb"), export_format='GLB', use_selection=True, export_animations=False)
