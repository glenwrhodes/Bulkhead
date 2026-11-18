"""Bulkhead kit: one-shot build. Run inside Blender (the MCP session or
`blender --python art/blender/scripts/bk_build.py`):

    import bk_build; bk_build.main()

Rebuilds every piece from the generator scripts, lays out the showcase and test
corridor, exports assets/*.glb plus assets/kit_manifest.json, and saves
art/blender/bulkhead_kit.blend.
"""

import importlib
import json
import os
import sys

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
ASSETS = os.path.join(ROOT, 'assets')
BLEND = os.path.join(ROOT, 'art', 'blender', 'bulkhead_kit.blend')
if HERE not in sys.path:
    sys.path.insert(0, HERE)

MODULES = ['bk_core', 'bk_corridor', 'bk_doors', 'bk_room', 'bk_props', 'bk_gun', 'bk_derelict', 'bk_arms',
           'bk_creature', 'bk_layout', 'bk_studio']

# How the engine uses each piece (documented in the manifest).
CATEGORY = {
    'corridor_straight': ('corridor', 'cell centre; open N+S (Blender +Y/-Y)'),
    'corridor_corner': ('corridor', 'cell centre; open N+E'),
    'corridor_tee': ('corridor', 'cell centre; open N+E+S'),
    'corridor_cross': ('corridor', 'cell centre; open N+E+S+W'),
    'corridor_end': ('corridor', 'cell centre; open N'),
    'door_frame': ('door', 'edge midpoint; passage along local Y'),
    'door_sliding': ('door', 'edge midpoint; animate LeafL along -X and LeafR along +X (0.8 m)'),
    'door_bulkhead': ('door', 'edge midpoint; animate Leaf along +Z (2.4 m)'),
    'door_airlock': ('door', 'edge midpoint; animate Leaf along +Z (2.4 m); exit door'),
    'room_floor': ('room', 'room cell centre'),
    'room_ceiling': ('room', 'room cell centre'),
    'room_wall_a': ('room', 'room cell centre; wall on the cell south edge facing north'),
    'room_wall_b': ('room', 'room cell centre; wall on the cell south edge facing north'),
    'room_wall_door': ('room', 'room cell centre; south edge, doorway for door_frame'),
    'room_corner': ('room', 'room cell centre; post at the south-west corner'),
    'crate_small': ('prop', 'floor, prop centre'),
    'crate_medium': ('prop', 'floor, prop centre'),
    'crate_long': ('prop', 'floor, prop centre'),
    'barrel_blue': ('prop', 'floor, prop centre'),
    'barrel_red': ('prop', 'floor, prop centre'),
    'canister': ('prop', 'floor, prop centre'),
    'console': ('prop', 'floor, prop centre; screen faces -Y'),
    'handgun': ('weapon', 'grip origin; forward +Y, up +Z; fx_muzzle marker'),
    'corridor_straight_broken': ('corridor', 'derelict variant of corridor_straight: north ceiling tile torn open; '
                                             'fx_spark* markers'),
    'pipes_low': ('derelict', 'straight corridor cell; crosses the walkway at y 0.88, underside 1.30 m (crouch)'),
    'pipe_burst': ('derelict', 'straight corridor cell; east wall; fx_steam marker'),
    'debris': ('derelict', 'straight corridor cell; along the east wall'),
    'beacon': ('derelict', 'straight corridor cell; east wall; own red light'),
    'arms': ('weapon', 'gun space (same transform as handgun)'),
    'muzzle_flash': ('fx', 'gun space, at the muzzle'),
    'bolt': ('fx', 'origin at centre, along +Y, 0.5 m'),
    'spark': ('fx', 'origin at centre, along +Y, 7 cm'),
    'scorch': ('fx', 'decal in the XY plane facing +Z'),
    'creature': ('creature', 'rigid parts creature_<bone> in the rest pose; fx_bone_<bone> pivots, fx_foot_<leg> claw '
                             'tips; forward +Y (glTF -Z), origin on the ground under the waist'),
}


def reload_modules():
    mods = {}
    for name in MODULES:
        if name in sys.modules:
            mods[name] = importlib.reload(sys.modules[name])
        else:
            mods[name] = importlib.import_module(name)
    return mods


def build_all(m):
    core = m['bk_core']
    kit = core.collection('KIT')
    for k in ('straight', 'corner', 'tee', 'cross', 'end'):
        m['bk_corridor'].build_corridor(k, kit)
    m['bk_doors'].build_frame(kit)
    m['bk_doors'].build_sliding(kit)
    m['bk_doors'].build_bulkhead(kit)
    m['bk_doors'].build_bulkhead(kit, airlock=True)
    m['bk_room'].build_rooms(kit)
    m['bk_props'].build_props(kit)
    m['bk_gun'].build_gun(kit)
    m['bk_corridor'].build_corridor('straight', kit, broken=True)
    m['bk_derelict'].build_derelict(kit)
    m['bk_arms'].build_all_fx(kit)
    m['bk_creature'].build_creature(kit)
    m['bk_layout'].test_corridor()
    return kit


def triangles(obj):
    dg = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(dg)
    me = ev.to_mesh()
    me.calc_loop_triangles()
    n = len(me.loop_triangles)
    ev.to_mesh_clear()
    return n


def export_all(m, kit):
    os.makedirs(ASSETS, exist_ok=True)
    studio = m['bk_studio']
    manifest = {'units': 'metres', 'up': 'glTF +Y (Blender +Z)', 'north': 'glTF -Z (Blender +Y)',
                'cell_size': 4.0, 'corridor_width': 2.4, 'corridor_ceiling': 2.85, 'room_ceiling': 3.4,
                'pieces': {}}
    for coll in kit.children:
        name = coll.name[len('KIT_'):]
        studio.show_only(['KIT'], [coll.name])
        bpy.ops.object.select_all(action='DESELECT')
        objs = [o for o in coll.objects]
        for o in objs:
            o.select_set(True)
        meshes = [o for o in objs if o.type == 'MESH']
        if meshes:
            bpy.context.view_layer.objects.active = meshes[0]
        path = os.path.join(ASSETS, name + '.glb')
        bpy.ops.export_scene.gltf(filepath=path, export_format='GLB', use_selection=True, export_apply=True,
                                  export_lights=True, export_yup=True, export_cameras=False, export_extras=False,
                                  export_materials='EXPORT', export_normals=True, export_tangents=False)
        mats = sorted({ms.material.name for o in meshes for ms in o.material_slots if ms.material})
        tris = sum(triangles(o) for o in meshes)
        cat, place = CATEGORY.get(name, ('other', ''))
        manifest['pieces'][name] = {
            'file': 'assets/%s.glb' % name, 'category': cat, 'placement': place,
            'nodes': [o.name for o in objs], 'triangles': tris, 'materials': mats,
            'lights': sum(1 for o in objs if o.type == 'LIGHT'),
            'bytes': os.path.getsize(path)}
    with open(os.path.join(ASSETS, 'kit_manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=2)
    bpy.ops.object.select_all(action='DESELECT')
    return manifest


def main(export=True, save=True):
    m = reload_modules()
    m['bk_studio'].hide_default_objects()
    kit = build_all(m)
    manifest = export_all(m, kit) if export else None
    # the creature's armature and crawl cycle (Blender only; not exported)
    m['bk_creature'].build_rig(bpy.data.collections['KIT_creature'])
    if save:
        bpy.ops.wm.save_as_mainfile(filepath=BLEND, compress=True)
    return manifest


if __name__ == '__main__':
    main()
