"""Bulkhead kit: room modules. Rooms are made of full 4 x 4 m cells with a 3.4 m
ceiling. Wall pieces are modelled on the SOUTH edge of their cell, facing north;
the engine rotates them onto any edge. Two walls meeting at a room corner overlap
exactly (identical profiles), and a corner post covers the joint."""

import math

import bpy
from mathutils import Matrix, Vector

from bk_core import V2, V3, Piece, frame_matrix, subdivide, text_at
from bk_corridor import (OUTLINE, PIPES, Bay, circle, detail_box, detail_cables, detail_hatch, detail_sign, panelise)

RCEIL = 3.4
RWALL = [(0.30, 0.0), (0.0, 0.45), (0.0, 1.22), (0.035, 1.25), (0.035, 1.33), (0.0, 1.36), (0.0, 2.2),
         (0.0, 3.0), (0.4, RCEIL)]
RK_CHLOW, RK_WLOW, RK_TRB, RK_TRF, RK_TRT, RK_WMID, RK_WTOP, RK_CHUP = range(8)
RWALL_MATS = {RK_CHLOW: 'PanelGrey', RK_WLOW: 'PanelLight', RK_TRB: 'Gunmetal', RK_TRF: 'Gunmetal',
              RK_TRT: 'Gunmetal', RK_WMID: 'PanelWhite', RK_WTOP: 'PanelWhite', RK_CHUP: 'PanelGrey'}
# pipes ride the room's upper chamfer (0,3.0)->(0.4,3.4)
_RD = V2(0.4, 0.4).normalized()
_RN = V2(_RD.y, -_RD.x)


def _rch(s, standoff):
    p = V2(0.0, 3.0) + _RD * (0.566 * s) + _RN * standoff
    return p.x, p.y


RPIPES = [(*_rch(0.35, 0.075), 0.055, 'PipeYellow'), (*_rch(0.75, 0.06), 0.04, 'Steel')]


def room_wall(name, parent, variant='a', doorway=False):
    pc = Piece(name)
    corners = [(-2.0, -2.0), (2.0, -2.0)]
    path = subdivide([V2(*p) for p in corners], 1.0)
    faces = pc.main.sweep(path, RWALL, lambda i, k: RWALL_MATS.get(k, 'PanelWhite'))
    for i in range(len(path) - 1):
        bay = Bay(path[i], path[i + 1])
        alt = i % 2 == 0
        if doorway and i in (1, 2):
            continue  # cut away by the doorway; dressed by the door frame
        panelise(pc.main, faces[i][RK_WLOW], inner='PanelGrey' if alt else 'PanelDark', recess='PanelDark',
                 frame_mat='PanelLight', bolts=pc.fine)
        panelise(pc.main, faces[i][RK_WMID], inner='PanelWhite' if alt else 'PanelLight', bolts=pc.fine)
        panelise(pc.main, faces[i][RK_WTOP], inner='PanelLight', plate=0.0)
        panelise(pc.main, faces[i][RK_CHLOW], frame=0.04, depth=0.012, plate=0.0, inner='PanelDark')
        panelise(pc.main, faces[i][RK_CHUP], frame=0.035, depth=0.012, plate=0.0, inner='PanelDark')
    # glow strips: at the floor and a cove light under the upper chamfer
    spans = [(-2.0, -1.25), (1.25, 2.0)] if doorway else [(-2.0, 2.0)]
    for a, b in spans:
        p2 = [V2(a, -2.0), V2(b, -2.0)]
        pc.flat.sweep(p2, [(0.335, 0.003), (0.305, 0.003)], 'LightTeal')
    pc.flat.sweep([V2(-2.0, -2.0), V2(2.0, -2.0)], [(0.012, 2.985), (0.06, 3.03)], 'LightTeal')
    pc.fine.sweep([V2(-2.0, -2.0), V2(2.0, -2.0)], [(0.0, 2.94), (0.03, 2.95), (0.03, 2.99), (0.0, 3.0)], 'Gunmetal')
    for o, z, r, m in RPIPES:
        pc.round.sweep([V2(-2.0, -2.0), V2(2.0, -2.0)], circle(o, z, r), m, closed_profile=True)

    # pilasters: full at the middle, halves at both ends (neighbours complete them)
    outline = [(0.30, 0.0), (0.0, 0.45), (0.0, 3.0), (0.4, RCEIL), (0.75, RCEIL)]
    # band's local x = offset into the room (+Y world), local y = along the wall (world x = xc - y)
    for xc, y0, y1, front, back in ((0.0, -0.16, 0.16, True, True), (-2.0, -0.16, 0.0, False, True),
                                    (2.0, 0.0, 0.16, True, False)):
        if doorway and xc == 0.0:
            continue
        M = Matrix.Translation(V3(xc, -2.0, 0)) @ Matrix.Rotation(math.radians(90), 4, 'Z')
        pc.main.band(outline, 0.13, y0, y1, 'PanelDark', M=M, front=front, back=back, inner_mat='Gunmetal')
        # a vertical light strip on the pilaster face
        if xc == 0.0:
            pc.flat.obox(V3(0, -2.0 + 0.131, 1.9), (0.05, 0.004, 1.0), 'LightTeal')

    if not doorway:
        if variant == 'a':
            wall_screen(pc, Bay(V2(0.15, -2.0), V2(1.85, -2.0)))
            lockers(pc, Bay(V2(-1.85, -2.0), V2(-0.15, -2.0)))
        else:
            pipe_cluster(pc, Bay(V2(-1.85, -2.0), V2(-0.15, -2.0)))
            detail_box(pc, Bay(V2(0.15, -2.0), V2(1.0, -2.0)), pc.rng, conduit_top=3.05)
            detail_sign(pc, Bay(V2(1.0, -2.0), V2(1.85, -2.0)), pc.rng)
    else:
        text_at(pc.flat, 'CARGO 2', 0.16, V3(0.0, -2.0 + 0.004, 3.15), V3(1, 0, 0), V3(0, 0.0, 1), 'PaintDark')
        build_cutter_and_cut(pc)
        return None
    return pc.finish(parent)


def wall_screen(pc, bay):
    s = bay.L / 2
    z = 2.05
    R = bay.R()
    pc.fine.obox(bay.p(s, 0.05, z), (1.25, 0.75, 0.1), 'Gunmetal', rot=R)
    pc.fine.obox(bay.p(s, 0.1, z), (1.15, 0.65, 0.01), 'Recess', rot=R)
    pc.flat.obox(bay.p(s, 0.1062, z), (1.12, 0.62, 0.002), 'ScreenDim', rot=R)
    # a little ship schematic on the screen
    for (dx, dz, w, h) in ((-0.35, 0.12, 0.3, 0.03), (-0.35, -0.12, 0.3, 0.03), (-0.2, 0.0, 0.03, 0.27),
                           (0.05, 0.0, 0.5, 0.03), (0.3, 0.1, 0.03, 0.2), (0.3, -0.12, 0.2, 0.03)):
        pc.flat.obox(bay.p(s - dx, 0.1085, z + dz), (w, h, 0.001), 'Screen', rot=R)
    text_at(pc.flat, 'DECK 3  STATUS', 0.045, bay.p(s, 0.1088, z + 0.25), V3(-bay.d.x, -bay.d.y, 0), V3(0, 0, 1),
            'Screen')
    text_at(pc.flat, 'HULL OK', 0.04, bay.p(s + 0.38, 0.1088, z - 0.25), V3(-bay.d.x, -bay.d.y, 0), V3(0, 0, 1),
            'LightGreen')
    text_at(pc.flat, 'O2 98%', 0.04, bay.p(s - 0.38, 0.1088, z - 0.25), V3(-bay.d.x, -bay.d.y, 0), V3(0, 0, 1),
            'LightAmber')


def lockers(pc, bay):
    """Two lockers standing on the floor in front of the lower chamfer."""
    R = bay.R()
    for k in range(2):
        s = bay.L * (0.25 + 0.5 * k)
        o = 0.32 + 0.22
        pc.fine.obox(bay.p(s, o, 1.0), (0.78, 2.0, 0.44), 'CrateGrey', rot=R)
        # door panel, vents and handle on the front (+Z local = toward the room)
        pc.fine.obox(bay.p(s, o + 0.225, 1.02), (0.7, 1.86, 0.012), 'PanelLight', rot=R)
        for j in range(6):
            pc.fine.obox(bay.p(s, o + 0.233, 1.7 - j * 0.035), (0.4, 0.014, 0.006), 'Recess', rot=R)
        pc.fine.obox(bay.p(s - 0.28, o + 0.24, 1.0), (0.03, 0.22, 0.03), 'Gunmetal', rot=R)
        pc.flat.obox(bay.p(s, o + 0.2317, 1.32), (0.7, 0.04, 0.001), 'AccentOrange', rot=R)
        text_at(pc.flat, 'L-%d' % (k + 4), 0.07, bay.p(s, o + 0.2318, 1.45), V3(-bay.d.x, -bay.d.y, 0), V3(0, 0, 1),
                'PaintDark')
        pc.fine.obox(bay.p(s, o, 0.03), (0.74, 0.06, 0.4), 'Rubber', rot=R)


def pipe_cluster(pc, bay):
    """Vertical pipes with valve wheels and a gauge."""
    mats = ['PipeBlue', 'PipeYellow', 'PipeRed']
    for k in range(3):
        s = bay.L * (0.25 + 0.25 * k)
        r = 0.06 if k != 1 else 0.075
        o = 0.12 + r
        pc.round.cyl(bay.p(s, o + 0.18, 0.0), bay.p(s, o + 0.18, 0.5), r, mats[k], seg=16, caps=(False, False))
        pc.round.tube([bay.p(s, o + 0.18, 0.5), bay.p(s, o + 0.12, 0.62), bay.p(s, o, 0.75)], r, mats[k], seg=16,
                      caps=False)
        pc.round.cyl(bay.p(s, o, 0.75), bay.p(s, o, RCEIL), r, mats[k], seg=16, caps=(False, False))
        for z in (1.1, 2.6):
            pc.fine.cyl(bay.p(s, o, z - 0.03), bay.p(s, o, z + 0.03), r + 0.018, 'DarkSteel', seg=16)
        # valve
        vz = 1.55 + 0.12 * k
        pc.fine.cyl(bay.p(s, o, vz - 0.06), bay.p(s, o, vz + 0.06), r + 0.025, 'Gunmetal', seg=16)
        hub = bay.p(s, o + r + 0.06, vz)
        out = V3(bay.n.x, bay.n.y, 0)
        pc.fine.cyl(bay.p(s, o + r, vz), hub, 0.015, 'Steel', seg=8)
        pc.fine.ring(hub, out, 0.11, 0.013, 'PipeRed' if k != 2 else 'HazardYellow', seg=24, tseg=8)
        for a in range(3):
            ang = a * 2 * math.pi / 3
            u = V3(-bay.d.x, -bay.d.y, 0) * math.cos(ang) + V3(0, 0, 1) * math.sin(ang)
            pc.fine.cyl(hub, hub + u * 0.11, 0.008, 'Steel', seg=6)
    # gauge
    s = bay.L * 0.62
    g = bay.p(s, 0.06, 2.1)
    pc.fine.cyl(g, g + V3(bay.n.x, bay.n.y, 0) * 0.05, 0.08, 'Steel', seg=24)
    pc.flat.cyl(g + V3(bay.n.x, bay.n.y, 0) * 0.051, g + V3(bay.n.x, bay.n.y, 0) * 0.052, 0.068, 'Paint', seg=24)
    pc.flat.obox(bay.p(s, 0.1125, 2.11), (0.006, 0.055, 0.001), 'PipeRed',
                 rot=bay.R() @ Matrix.Rotation(math.radians(-35), 3, 'Z'))
    text_at(pc.flat, 'PRESSURE', 0.05, bay.p(bay.L * 0.5, -0.008, 2.55), V3(-bay.d.x, -bay.d.y, 0), V3(0, 0, 1),
            'PaintDark')


def build_cutter_and_cut(pc):
    """Finishes the doorway wall: builds it, then cuts the corridor-shaped opening."""
    parent = bpy.data.collections['KIT']
    coll = pc.finish(parent)
    old = bpy.data.objects.get('_doorway_cutter')
    if old is not None:
        me_old = old.data
        bpy.data.objects.remove(old, do_unlink=True)
        bpy.data.meshes.remove(me_old)
    cutter_me = bpy.data.meshes.new('_doorway_cutter')
    import bmesh
    bm = bmesh.new()
    out = [V2(x, z) for x, z in OUTLINE]
    # close the outline along the floor (slightly below) to make a prism
    loop = [V2(out[0].x, -0.2)] + out + [V2(out[-1].x, -0.2)]
    front = [bm.verts.new(V3(p.x, -1.55, p.y)) for p in loop]
    back = [bm.verts.new(V3(p.x, -2.45, p.y)) for p in loop]
    bm.faces.new(front)
    bm.faces.new(list(reversed(back)))
    n = len(loop)
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new((front[i], back[i], back[j], front[j]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(cutter_me)
    bm.free()
    cutter = bpy.data.objects.new('_doorway_cutter', cutter_me)
    bpy.context.scene.collection.objects.link(cutter)
    for ob in coll.objects:
        if ob.type != 'MESH':
            continue
        mod = ob.modifiers.new('Doorway', 'BOOLEAN')
        mod.operation = 'DIFFERENCE'
        mod.solver = 'EXACT'
        if hasattr(mod, 'use_hole_tolerant'):
            mod.use_hole_tolerant = True
        mod.object = cutter
        # boolean first, then bevel/normals
        ob.modifiers.move(len(ob.modifiers) - 1, 0)
    cutter.hide_render = True
    cutter.hide_viewport = True
    cutter.display_type = 'WIRE'
    return coll


def room_floor(parent):
    pc = Piece('room_floor')
    up = V3(0, 0, 1)
    for ix in range(4):
        for iy in range(4):
            x0, y0 = -2 + ix, -2 + iy
            m = 'DarkSteel' if (ix + iy) % 2 == 0 else 'Gunmetal'
            f = pc.main.face([V3(x0, y0, 0), V3(x0 + 1, y0, 0), V3(x0 + 1, y0 + 1, 0), V3(x0, y0 + 1, 0)], m,
                             toward=up)
            pc.main.inset(f, 0.012, 0.0, rim_mat='Recess')
            pc.main.inset(f, 0.06, 0.004)
            pc.main.inset(f, 0.006, -0.004, rim_mat='DarkSteel')
            for cx, cy in ((x0 + 0.04, y0 + 0.04), (x0 + 0.96, y0 + 0.04), (x0 + 0.96, y0 + 0.96), (x0 + 0.04, y0 + 0.96)):
                pc.fine.cyl(V3(cx, cy, -0.002), V3(cx, cy, 0.007), 0.009, 'Steel', seg=8)
    return pc.finish(parent)


def room_ceiling(parent):
    pc = Piece('room_ceiling')
    down = V3(0, 0, -1)
    h = 1.85
    f = pc.main.face([V3(-h, -h, RCEIL), V3(h, -h, RCEIL), V3(h, h, RCEIL), V3(-h, h, RCEIL)], 'PanelGrey', toward=down)
    panelise(pc.main, f, frame=0.05, depth=0.04, plate=0.0, inner='PanelDark')
    # half beams along the four cell edges (neighbouring cells complete them)
    for (x0, y0, x1, y1) in ((-2, -2, 2, -h), (-2, h, 2, 2), (-2, -2, -h, 2), (h, -2, 2, 2)):
        pc.main.box((x0, y0, RCEIL - 0.22), (x1, y1, RCEIL + 0.01), 'PanelLight',
                    mats={'-z': 'Gunmetal'})
    # big light panel with a louvre grid
    fl = pc.fine.box((-0.65, -0.65, RCEIL - 0.1), (0.65, 0.65, RCEIL + 0.005), 'Gunmetal')
    pc.fine.inset(fl['-z'], 0.05, 0.0)
    pc.fine.inset(fl['-z'], 0.004, 0.01, rim_mat='DarkSteel', inner_mat='LightWhite')
    for k in range(1, 5):
        c = -0.6 + k * 0.24
        pc.fine.box((c - 0.008, -0.6, RCEIL - 0.125), (c + 0.008, 0.6, RCEIL - 0.1), 'Gunmetal')
        pc.fine.box((-0.6, c - 0.008, RCEIL - 0.125), (0.6, c + 0.008, RCEIL - 0.1), 'Gunmetal')
    # sprinklers / sensors in the corner panels
    for sx in (-1.25, 1.25):
        for sy in (-1.25, 1.25):
            pc.fine.cyl(V3(sx, sy, RCEIL - 0.04), V3(sx, sy, RCEIL - 0.0), 0.05, 'PanelLight', seg=16)
            pc.fine.cyl(V3(sx, sy, RCEIL - 0.06), V3(sx, sy, RCEIL - 0.04), 0.018, 'Steel', seg=10)
    pc.light((0, 0, RCEIL - 0.45), power=320.0, radius=0.45)
    return pc.finish(parent)


def room_corner(parent):
    """Corner post at the cell's south-west corner covering where two walls meet."""
    pc = Piece('room_corner')
    poly = [V2(-2.0, -2.0), V2(-1.48, -2.0), V2(-1.48, -1.78), V2(-1.78, -1.48), V2(-2.0, -1.48)]
    c = V2(-1.8, -1.8)
    faces = []
    for a, b in zip(poly, poly[1:] + poly[:1]):
        d = (b - a).normalized()
        n = V3(d.y, -d.x, 0)
        mid = (a + b) * 0.5
        if n.x * (mid.x - c.x) + n.y * (mid.y - c.y) < 0:
            n = -n
        if abs(mid.x + 2.0) < 1e-6 or abs(mid.y + 2.0) < 1e-6:
            continue  # faces buried in the walls
        f = pc.main.face([V3(a.x, a.y, 0), V3(b.x, b.y, 0), V3(b.x, b.y, RCEIL), V3(a.x, a.y, RCEIL)], 'PanelDark',
                         toward=n)
        faces.append((f, a, b, n))
    for f, a, b, n in faces:
        diag = abs((b - a).x) > 1e-6 and abs((b - a).y) > 1e-6
        if diag:
            pc.main.inset(f, 0.04, 0.0)
            pc.main.inset(f, 0.004, -0.03, rim_mat='Gunmetal', inner_mat='Gunmetal')
            mid = (a + b) * 0.5
            p = V3(mid.x, mid.y, 1.7) + n * -0.026
            rot = frame_matrix(p, V3(-n.y, n.x, 0), V3(0, 0, 1)).to_3x3()
            pc.flat.obox(p, (0.06, 2.6, 0.004), 'LightTeal', rot=rot)
        else:
            pc.main.inset(f, 0.03, 0.0)
            pc.main.inset(f, 0.004, -0.012, rim_mat='Gunmetal', inner_mat='PanelGrey')
    # foot and cap
    pc.fine.box((-2.0, -2.0, 0.0), (-1.42, -1.42, 0.12), 'Gunmetal')
    pc.fine.box((-2.0, -2.0, RCEIL - 0.18), (-1.42, -1.42, RCEIL), 'Gunmetal')
    return pc.finish(parent)


def build_rooms(parent):
    room_floor(parent)
    room_ceiling(parent)
    room_wall('room_wall_a', parent, 'a')
    room_wall('room_wall_b', parent, 'b')
    room_wall('room_wall_door', parent, doorway=True)
    room_corner(parent)
