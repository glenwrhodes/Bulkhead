"""Bulkhead kit: the starting sidearm, a compact energy pistol.

Real-world scale (about 25 cm long). Forward is +Y, up is +Z, and the origin is
where the hand grips it (top of the grip, at the trigger). Two-tone light/gunmetal
body, polymer grip, glowing energy cell and vents.
"""

import math

from mathutils import Matrix, Vector

from bk_core import V2, V3, Builder, Piece, text_at

W = 0.0175  # half width of the slide


def prism_yz(b, poly, x0, x1, mat, side_mat=None, inset=None):
    """Extrudes a convex (y, z) polygon across x0..x1. Returns (side faces +x, -x)."""
    pts0 = [V3(x0, y, z) for y, z in poly]
    pts1 = [V3(x1, y, z) for y, z in poly]
    fp = b.face(pts1, side_mat or mat, toward=V3(1, 0, 0))
    fn = b.face(pts0, side_mat or mat, toward=V3(-1, 0, 0))
    c = Vector((sum(y for y, _ in poly) / len(poly), sum(z for _, z in poly) / len(poly)))
    for i in range(len(poly)):
        a, bb = V2(*poly[i]), V2(*poly[(i + 1) % len(poly)])
        d = (bb - a).normalized()
        n = V2(d.y, -d.x)
        if n.dot((a + bb) * 0.5 - c) < 0:
            n = -n
        b.face([V3(x0, a.x, a.y), V3(x0, bb.x, bb.y), V3(x1, bb.x, bb.y), V3(x1, a.x, a.y)], mat,
               toward=V3(0, n.x, n.y))
    return fp, fn


def build_gun(parent):
    pc = Piece('handgun')
    body = Builder()    # 1.5 mm bevels
    small = Builder()   # 0.6 mm bevels
    glow = Builder()    # emissive and decals
    grip_b = Builder()

    # --- slide -------------------------------------------------------------
    # Cross-section (x, z) with chamfered top edges; each vertex gets its own back and
    # front y so the nose sweeps back and the rear is undercut.
    C = 0.007
    xs = [(-W, 0.004), (W, 0.004), (W, 0.018), (W, 0.036), (W - C, 0.044), (-W + C, 0.044), (-W, 0.036),
          (-W, 0.018)]

    def front_y(z):
        return {0.004: 0.125, 0.018: 0.136, 0.036: 0.127, 0.044: 0.110}[z]

    def back_y(z):
        return {0.004: -0.072, 0.018: -0.076, 0.036: -0.074, 0.044: -0.062}[z]
    back = [body.bm.verts.new(V3(x, back_y(z), z)) for x, z in xs]
    front = [body.bm.verts.new(V3(x, front_y(z), z)) for x, z in xs]
    side_faces = {}
    n = len(xs)
    for k in range(n):
        k2 = (k + 1) % n
        (xa, za), (xb, zb) = xs[k], xs[k2]
        mid = V3((xa + xb) / 2, 0, (za + zb) / 2 - 0.024)
        f = body.face_verts([back[k], back[k2], front[k2], front[k]], 'PanelLight')
        f.normal_update()
        if f.normal.dot(V3(mid.x, 0, mid.z)) < 0:
            f.normal_flip()
        side_faces[k] = f
    for ring, d in ((front, V3(0, 1, 0)), (back, V3(0, -1, 0))):
        f = body.face_verts(list(ring), 'Gunmetal')
        f.normal_update()
        if f.normal.dot(d) < 0:
            f.normal_flip()
    for k in (2, 6):  # the main side panels
        f = side_faces[k]
        body.inset(f, 0.0025, 0.0)
        body.inset(f, 0.0006, -0.0009, rim_mat='Gunmetal', inner_mat='PanelWhite')
    for k in (4, 0):
        side_faces[k].material_index = body.mi('Gunmetal')
    # vents along the front of the slide, glowing inside
    for s in (-1, 1):
        x = s * (W + 0.0002)
        for k in range(5):
            y = 0.045 + k * 0.014
            frame = [V3(x, y - 0.0015, 0.0155), V3(x, y + 0.0075, 0.0155),
                     V3(x, y + 0.0125, 0.0365), V3(x, y + 0.0035, 0.0365)]
            glow.face(frame, 'Recess', toward=V3(s, 0, 0))
            xg = x + s * 0.00015
            quad = [V3(xg, y, 0.017), V3(xg, y + 0.006, 0.017), V3(xg, y + 0.011, 0.035), V3(xg, y + 0.005, 0.035)]
            glow.face(quad, 'LightTeal', toward=V3(s, 0, 0))
        # rear serrations
        for k in range(6):
            y = -0.066 + k * 0.006
            small.box((x - s * 0.0005 if s > 0 else x - 0.0012, y, 0.012), (x + 0.0012 if s > 0 else x + 0.0005, y + 0.003,
                                                                             0.038), 'Gunmetal')
        # orange accent and model stencil
        small.box((min(x, x + s * 0.0008), -0.03, 0.006), (max(x, x + s * 0.0008), 0.03, 0.0085), 'AccentOrange')
        text_at(glow, 'BH-9 ARC', 0.0065, V3(x + s * 0.0009, -0.012, 0.026), V3(0, s, 0), V3(0, 0, 1), 'PaintDark')

    # top rail
    for k in range(9):
        y = -0.04 + k * 0.012
        small.box((-0.008, y, 0.044), (0.008, y + 0.007, 0.0485), 'Gunmetal')
    small.box((-0.0055, -0.045, 0.044), (0.0055, 0.068, 0.046), 'Gunmetal')
    # sights
    small.box((-0.009, -0.06, 0.044), (-0.003, -0.05, 0.053), 'Gunmetal')
    small.box((0.003, -0.06, 0.044), (0.009, -0.05, 0.053), 'Gunmetal')
    small.box((-0.0025, 0.104, 0.044), (0.0025, 0.112, 0.054), 'Gunmetal')
    glow.box((-0.0012, 0.1035, 0.0505), (0.0012, 0.1045, 0.0525), 'LightGreen')
    for x in (-0.006, 0.006):
        glow.box((x - 0.001, -0.0505, 0.049), (x + 0.001, -0.0495, 0.051), 'LightGreen')

    # barrel and emitter
    bm_ = Matrix.Translation(V3(0, 0.12, 0.022)) @ Matrix.Rotation(math.radians(-90), 4, 'X')
    pc.round.lathe([(0.0, 0.0), (0.0105, 0.0), (0.0105, 0.03), (0.0125, 0.032), (0.0125, 0.04), (0.009, 0.042),
                    (0.0, 0.042)], 'Gunmetal', seg=24, M=bm_)
    glow.ring(V3(0, 0.1625, 0.022), V3(0, 1, 0), 0.0088, 0.0012, 'LightTeal', seg=24, tseg=6)
    glow.cyl(V3(0, 0.161, 0.022), V3(0, 0.1622, 0.022), 0.0065, 'LightTeal', seg=16)

    # --- frame (under the slide) -------------------------------------------
    frame = [(-0.062, 0.004), (0.1, 0.004), (0.11, -0.004), (0.104, -0.02), (0.032, -0.024), (-0.036, -0.024),
             (-0.062, -0.012)]
    ffp, ffn = prism_yz(body, frame, -W + 0.0015, W - 0.0015, 'Gunmetal')
    # energy channel along the frame
    for s in (-1, 1):
        x = s * (W - 0.0015 + 0.0003)
        glow.face([V3(x, 0.0, -0.013), V3(x, 0.088, -0.013), V3(x, 0.092, -0.009), V3(x, 0.0, -0.009)], 'LightTeal',
                  toward=V3(s, 0, 0))
        small.box((min(x, x + s * 0.0012), -0.004, -0.0165), (max(x, x + s * 0.0012), 0.096, -0.0145), 'DarkSteel')
        small.box((min(x, x + s * 0.0012), -0.004, -0.0075), (max(x, x + s * 0.0012), 0.096, -0.0055), 'DarkSteel')
    # accessory rail under the front
    for k in range(4):
        y = 0.05 + k * 0.012
        small.box((-0.009, y, -0.0275), (0.009, y + 0.007, -0.022), 'Gunmetal')

    # trigger guard and trigger
    small.tube([V3(0, 0.034, -0.022), V3(0, 0.034, -0.04), V3(0, 0.026, -0.05), V3(0, -0.012, -0.052),
                V3(0, -0.026, -0.044)], 0.0032, 'Gunmetal', seg=10)
    small.tube([V3(0, 0.008, -0.022), V3(0, 0.0095, -0.03), V3(0, 0.006, -0.039), V3(0, 0.0, -0.043)], 0.0028,
               'Steel', seg=10)

    # --- grip, raked back 16 degrees ---------------------------------------
    rake = math.radians(16)
    top_y0, top_y1, z_top, z_bot = -0.062, -0.026, -0.012, -0.112
    dy = (z_top - z_bot) * math.tan(rake)
    # rounded-rectangle cross-section swept down the raked axis, flaring at the base
    def rrect(cx, cy, hx, hy, r, seg=4):
        pts = []
        for qx, qy, a0 in ((1, 1, 0.0), (-1, 1, 90.0), (-1, -1, 180.0), (1, -1, 270.0)):
            ccx, ccy = cx + qx * (hx - r), cy + qy * (hy - r)
            for i in range(seg + 1):
                a = math.radians(a0 + 90.0 * i / seg)
                pts.append((ccx + r * math.cos(a), ccy + r * math.sin(a)))
        return pts
    rings = []
    for t, z in ((0.0, z_top + 0.004), (0.5, (z_top + z_bot) / 2), (1.0, z_bot)):
        yc = (top_y0 + top_y1) / 2 - dy * t - 0.002 * t
        hy = 0.018 + 0.003 * t
        rings.append([grip_b.bm.verts.new(V3(x, y, z)) for x, y in rrect(0.0, yc, W - 0.002, hy, 0.008)])
    m = len(rings[0])
    for a, b2 in zip(rings, rings[1:]):
        for i in range(m):
            i2 = (i + 1) % m
            f = grip_b.face_verts([a[i], a[i2], b2[i2], b2[i]], 'Polymer')
            f.normal_update()
            c = (a[i].co + b2[i2].co) * 0.5
            axis_pt = sum((v.co for v in a), V3(0, 0, 0)) / m
            out = c - axis_pt
            out.z = 0
            if f.normal.dot(out) < 0:
                f.normal_flip()
    f = grip_b.face_verts(list(rings[-1]), 'DarkSteel')
    f.normal_update()
    if f.normal.z > 0:
        f.normal_flip()
    # finger grooves on the front strap
    for k in range(3):
        t = 0.22 + k * 0.24
        z = z_top + (z_bot - z_top) * t
        y = top_y1 - dy * t - 0.0025 * t - 0.0028
        small.cyl(V3(-W + 0.005, y, z), V3(W - 0.005, y, z), 0.0045, 'Polymer', seg=12)
    # grip texture: raised ribs following the rake
    for s in (-1, 1):
        x = s * (W - 0.0022)
        for k in range(10):
            t = 0.12 + k * 0.075
            z = z_top + (z_bot - z_top) * t
            yc = (top_y0 + top_y1) / 2 - dy * t - 0.002 * t
            small.box((min(x, x + s * 0.0014), yc - 0.009, z - 0.0016), (max(x, x + s * 0.0014), yc + 0.009, z + 0.0016),
                      'Rubber')
    # back strap and energy cell at the base of the grip
    small.box((-0.012, top_y0 - dy - 0.009, z_bot - 0.012), (0.012, top_y1 - dy + 0.002, z_bot + 0.002), 'DarkSteel')
    glow.box((-0.0122, top_y0 - dy - 0.006, z_bot - 0.008), (0.0122, top_y1 - dy - 0.002, z_bot - 0.004), 'LightTeal')
    # charge display on the back of the slide (what the player sees): 5 segments
    small.box((-0.0135, -0.0795, 0.021), (0.0135, -0.073, 0.035), 'Gunmetal')
    glow.face([V3(-0.0115, -0.0797, 0.023), V3(0.0115, -0.0797, 0.023), V3(0.0115, -0.0797, 0.033),
               V3(-0.0115, -0.0797, 0.033)], 'Recess', toward=V3(0, -1, 0))
    for k in range(5):
        x0 = -0.0105 + k * 0.0044
        glow.face([V3(x0, -0.0799, 0.0245), V3(x0 + 0.0034, -0.0799, 0.0245), V3(x0 + 0.0034, -0.0799, 0.0315),
                   V3(x0, -0.0799, 0.0315)], 'LightTeal' if k < 4 else 'LightAmber', toward=V3(0, -1, 0))

    pc.extra_objects.append(('body', body, 0.0028))
    pc.extra_objects.append(('grip', grip_b, 0.002))
    pc.extra_objects.append(('detail', small, 0.0005))
    pc.extra_objects.append(('glow', glow, 0.0))
    return pc.finish(parent)
