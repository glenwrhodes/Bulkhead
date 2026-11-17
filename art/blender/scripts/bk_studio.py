"""Bulkhead kit: render setup, cameras and visibility helpers for checking pieces
and making the video shots."""

import math

import bpy
from mathutils import Vector

KIT_ROOT = 'KIT'


def setup_render(width=1280, height=720, samples=48):
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_EEVEE'
    sc.render.resolution_x = width
    sc.render.resolution_y = height
    sc.render.resolution_percentage = 100
    sc.render.image_settings.file_format = 'PNG'
    ee = sc.eevee
    for attr, val in (('taa_render_samples', samples), ('use_raytracing', True), ('use_shadows', True),
                      ('shadow_ray_count', 2), ('shadow_step_count', 8), ('use_fast_gi', True),
                      ('fast_gi_method', 'GLOBAL_ILLUMINATION'), ('ray_tracing_method', 'SCREEN'),
                      ('use_volumetric_shadows', False)):
        if hasattr(ee, attr):
            try:
                setattr(ee, attr, val)
            except Exception:  # noqa: BLE001
                pass
    if hasattr(ee, 'ray_tracing_options'):
        o = ee.ray_tracing_options
        for attr, val in (('resolution_scale', '1'), ('use_denoise', True)):
            if hasattr(o, attr):
                try:
                    setattr(o, attr, val)
                except Exception:  # noqa: BLE001
                    pass
    vs = sc.view_settings
    vs.view_transform = 'AgX'
    for look in ('AgX - Medium High Contrast', 'AgX - Base Contrast', 'None'):
        try:
            vs.look = look
            break
        except Exception:  # noqa: BLE001
            continue
    vs.exposure = 0.0
    world = sc.world or bpy.data.worlds.new('World')
    sc.world = world
    world.use_nodes = True
    bg = next((n for n in world.node_tree.nodes if n.type == 'BACKGROUND'), None)
    if bg:
        bg.inputs['Color'].default_value = (0.012, 0.016, 0.022, 1.0)
        bg.inputs['Strength'].default_value = 1.0


def camera(name='BH_Camera'):
    cam = bpy.data.objects.get(name)
    if cam is None:
        data = bpy.data.cameras.new(name)
        cam = bpy.data.objects.new(name, data)
        bpy.context.scene.collection.objects.link(cam)
    bpy.context.scene.camera = cam
    return cam


def look_at(eye, target, lens=30.0, name='BH_Camera', clip_start=0.05):
    cam = camera(name)
    cam.location = Vector(eye)
    d = Vector(target) - Vector(eye)
    cam.rotation_mode = 'QUATERNION'
    cam.rotation_quaternion = d.to_track_quat('-Z', 'Y')
    cam.data.lens = lens
    cam.data.clip_start = clip_start
    cam.data.clip_end = 400.0
    return cam


def studio_light_small(target, dist=0.8, energy=40.0):
    """Key + rim scaled for small objects such as the handgun."""
    studio_light(True, energy)
    t = Vector(target)
    key = bpy.data.objects['BH_Key']
    key.data.size = dist * 0.8
    key.location = t + Vector((0.5, -0.8, 0.7)) * dist
    key.rotation_mode = 'QUATERNION'
    key.rotation_quaternion = (t - key.location).to_track_quat('-Z', 'Y')
    rim = bpy.data.objects['BH_Rim']
    rim.data.size = dist * 0.6
    rim.data.energy = energy * 1.2
    rim.location = t + Vector((-0.7, 0.7, 0.5)) * dist
    rim.rotation_mode = 'QUATERNION'
    rim.rotation_quaternion = (t - rim.location).to_track_quat('-Z', 'Y')


def studio_light(on=True, energy=400.0):
    """Soft overhead key + rim for exterior 'dollhouse' piece shots."""
    names = ('BH_Key', 'BH_Rim')
    for nm in names:
        ob = bpy.data.objects.get(nm)
        if ob is None:
            ld = bpy.data.lights.new(nm, 'AREA')
            ob = bpy.data.objects.new(nm, ld)
            bpy.context.scene.collection.objects.link(ob)
        ob.hide_render = not on
        ob.hide_viewport = not on
    key = bpy.data.objects['BH_Key']
    key.data.energy = energy
    key.data.size = 6.0
    key.data.color = (1.0, 0.97, 0.92)
    key.location = (4.0, -5.0, 9.0)
    key.rotation_mode = 'XYZ'
    key.rotation_euler = (math.radians(35), 0, math.radians(35))
    rim = bpy.data.objects['BH_Rim']
    rim.data.energy = energy * 0.5
    rim.data.size = 4.0
    rim.data.color = (0.6, 0.85, 1.0)
    rim.location = (-6.0, 6.0, 6.0)
    rim.rotation_mode = 'XYZ'
    rim.rotation_euler = (math.radians(-45), 0, math.radians(-135))


def hide_default_objects():
    for nm in ('Cube', 'Light', 'Camera'):
        ob = bpy.data.objects.get(nm)
        if ob:
            bpy.data.objects.remove(ob, do_unlink=True)


def layer_collection(name, lc=None):
    lc = lc or bpy.context.view_layer.layer_collection
    if lc.name == name:
        return lc
    for child in lc.children:
        r = layer_collection(name, child)
        if r:
            return r
    return None


def show_only(top_names, kit_names=()):
    """Shows the given top-level collections (excluding the rest), and inside KIT shows
    only kit_names."""
    root = bpy.context.view_layer.layer_collection
    for lc in root.children:
        lc.exclude = lc.name not in top_names
    kit = layer_collection(KIT_ROOT)
    if kit and not kit.exclude:
        for lc in kit.children:
            lc.exclude = lc.name not in kit_names


def render(path):
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    return path
