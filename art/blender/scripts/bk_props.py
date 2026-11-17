"""Bulkhead kit: props. Origins sit on the floor at the prop's centre."""

import math

from mathutils import Matrix, Vector

from bk_core import V2, V3, Builder, Piece, frame_matrix, text_at


# ---------------------------------------------------------------------------
# Crates
# ---------------------------------------------------------------------------


def crate(name, parent, size, colour, label, stripes=False):
    sx, sy, sz = size
    pc = Piece(name)
    faces = pc.main.box((-sx / 2, -sy / 2, 0.04), (sx / 2, sy / 2, sz), 'Gunmetal')
    for key, f in faces.items():
        if key == '-z':
            continue
        e = min(0.07, 0.12 * min(sx, sy, sz))
        pc.main.inset(f, e, 0.0)
        pc.main.inset(f, 0.006, -0.02, rim_mat='PanelDark', inner_mat=colour)
        pc.main.inset(f, 0.035, 0.012, rim_mat=colour)
    # corner protectors
    c = 0.11
    for ix in (-1, 1):
        for iy in (-1, 1):
            for iz, z0 in ((0, 0.03), (1, sz - c + 0.012)):
                x0 = ix * sx / 2 - (c if ix > 0 else 0) + (0.012 if ix > 0 else -0.012)
                y0 = iy * sy / 2 - (c if iy > 0 else 0) + (0.012 if iy > 0 else -0.012)
                pc.fine.box((x0, y0, z0), (x0 + c, y0 + c, z0 + c), 'DarkSteel')
    # skids
    for ix in (-1, 1):
        pc.fine.box((ix * sx * 0.3 - 0.05, -sy / 2 + 0.03, 0.0), (ix * sx * 0.3 + 0.05, sy / 2 - 0.03, 0.045), 'Rubber')
    # handles on the short sides
    for ix in (-1, 1):
        x = ix * (sx / 2 + 0.005)
        pts = [V3(x, -0.12, sz * 0.62), V3(x + ix * 0.04, -0.12, sz * 0.62), V3(x + ix * 0.04, 0.12, sz * 0.62),
               V3(x, 0.12, sz * 0.62)]
        pc.fine.tube(pts, 0.012, 'Steel', seg=8)
        for y in (-0.12, 0.12):
            pc.fine.box((x - 0.015 if ix > 0 else x - 0.005, y - 0.025, sz * 0.62 - 0.03),
                        (x + 0.005 if ix > 0 else x + 0.015, y + 0.025, sz * 0.62 + 0.03), 'Gunmetal')
    # label plate with barcode on the front (-Y) and back (+Y)
    for s in (-1, 1):
        y = s * (sy / 2 - 0.02 + 0.012 + 0.003)
        right = V3(-s, 0, 0) * -1 if s < 0 else V3(-1, 0, 0)
        facing = V3(0, s, 0)
        lw, lh = min(0.36, sx * 0.45), 0.16
        zc = sz * 0.55
        pc.flat.face([V3(-lw / 2, y, zc - lh / 2), V3(lw / 2, y, zc - lh / 2), V3(lw / 2, y, zc + lh / 2),
                      V3(-lw / 2, y, zc + lh / 2)], 'Paint', toward=facing)
        text_at(pc.flat, label, 0.05, V3(0, y + s * 0.0006, zc + 0.035), V3(-s, 0, 0), V3(0, 0, 1), 'PaintDark')
        x = -lw / 2 + 0.03
        k = 0
        while x < lw / 2 - 0.04:
            w = (0.004, 0.008, 0.003, 0.006)[k % 4]
            xr = -x if s > 0 else x
            pc.flat.face([V3(xr, y + s * 0.0006, zc - 0.06), V3(xr - s * w, y + s * 0.0006, zc - 0.06),
                          V3(xr - s * w, y + s * 0.0006, zc - 0.005), V3(xr, y + s * 0.0006, zc - 0.005)],
                         'PaintDark', toward=facing)
            x += w + 0.006
            k += 1
        if stripes:
            for j in range(10):
                x0 = -sx / 2 + 0.1 + j * 0.08
                if x0 + 0.1 > sx / 2 - 0.1:
                    break
                m = 'HazardYellow' if j % 2 == 0 else 'HazardBlack'
                pc.flat.face([V3(x0, y, 0.12), V3(x0 + 0.08, y, 0.12), V3(x0 + 0.12, y, 0.2), V3(x0 + 0.04, y, 0.2)], m,
                             toward=facing)
    # stencil on the lid
    text_at(pc.flat, 'THIS SIDE UP' if sx > 0.9 else 'BH', 0.07 if sx > 0.9 else 0.1,
            V3(0, 0, sz - 0.008 + 0.0005), V3(1, 0, 0), V3(0, 1, 0), 'Paint')
    return pc.finish(parent)


# ---------------------------------------------------------------------------
# Barrels and canister
# ---------------------------------------------------------------------------


def hazard_band(b, r, z0, z1, seg=160, rows=4, stripe=8, shear=6, colours=('HazardYellow', 'HazardBlack')):
    """Diagonal stripes wrapped around a cylinder of radius r: a fine grid whose rows
    are rotated progressively, so each cell stays nearly flat on the surface."""
    def P(i, j):
        a = 2 * math.pi * (j + shear * i / rows) / seg
        return V3(r * math.cos(a), r * math.sin(a), z0 + (z1 - z0) * i / rows)
    for i in range(rows):
        for j in range(seg):
            pts = [P(i, j), P(i, j + 1), P(i + 1, j + 1), P(i + 1, j)]
            mid = (pts[0] + pts[2]) * 0.5
            b.face(pts, colours[(j // stripe) % 2], toward=V3(mid.x, mid.y, 0))


def barrel(name, parent, colour, band=('HazardYellow', 'HazardBlack')):
    pc = Piece(name)
    R, H = 0.29, 0.88
    prof = [(0.0, 0.0), (R - 0.02, 0.0), (R, 0.02), (R, H - 0.02), (R - 0.02, H), (R - 0.035, H),
            (R - 0.045, H - 0.015), (0.0, H - 0.015)]
    pc.main.lathe(prof, colour, seg=40)
    for z in (0.012, H - 0.012):
        pc.fine.ring(V3(0, 0, z), V3(0, 0, 1), R - 0.006, 0.014, 'DarkSteel', seg=40, tseg=8)
    for z in (0.29, 0.59):
        pc.fine.ring(V3(0, 0, z), V3(0, 0, 1), R + 0.002, 0.012, colour, seg=40, tseg=8)
    hazard_band(pc.flat, R + 0.003, 0.36, 0.52, colours=band)
    # bungs
    for (x, y, rr) in ((0.15, 0.0, 0.035), (-0.16, 0.04, 0.025)):
        pc.fine.cyl(V3(x, y, H - 0.016), V3(x, y, H + 0.006), rr + 0.012, 'DarkSteel', seg=16)
        pc.fine.cyl(V3(x, y, H + 0.006), V3(x, y, H + 0.018), rr, 'Steel', seg=6)
    text_at(pc.flat, 'BH', 0.08, V3(0, 0, H - 0.0145), V3(1, 0, 0), V3(0, 1, 0), 'Paint')
    return pc.finish(parent)


def canister(name, parent):
    """Pressurised gas tank in a cage, with a glowing level window."""
    pc = Piece(name)
    R = 0.2
    prof = [(0.0, 0.1)]
    for k in range(1, 7):
        a = math.pi / 2 * (k / 6)
        prof.append((R * math.sin(a), 0.1 + R * (1 - math.cos(a))))
    for k in range(0, 6):
        a = math.pi / 2 * (k / 6)
        prof.append((R * math.cos(a), 1.05 + R * math.sin(a)))
    prof.append((0.0, 1.05 + R))
    pc.round.lathe(prof, 'PanelWhite', seg=36)
    hazard_band(pc.flat, R + 0.003, 0.42, 0.5, stripe=6, colours=('AccentOrange', 'PanelDark'))
    # level window
    for k, (z0, z1) in enumerate(((0.55, 1.0),)):
        a0, a1 = -0.18, 0.18
        pts = []
        for i in range(7):
            a = a0 + (a1 - a0) * i / 6
            pts.append((math.cos(a), math.sin(a)))
        for i in range(6):
            (c0, s0), (c1, s1) = pts[i], pts[i + 1]
            r = R + 0.002
            quad = [V3(r * c0, r * s0, z0), V3(r * c1, r * s1, z0), V3(r * c1, r * s1, z1), V3(r * c0, r * s0, z1)]
            pc.flat.face(quad, 'LightTeal' if i in (2, 3) else 'Recess', toward=V3(1, 0, 0))
    # cage: base ring, top ring, four posts
    pc.fine.lathe([(0.0, 0.0), (0.27, 0.0), (0.27, 0.1), (0.0, 0.1)], 'Gunmetal', seg=32)
    for z in (0.6, 1.2):
        pc.fine.ring(V3(0, 0, z), V3(0, 0, 1), 0.245, 0.016, 'Gunmetal', seg=32, tseg=8)
    for k in range(4):
        a = math.pi / 4 + k * math.pi / 2
        x, y = 0.245 * math.cos(a), 0.245 * math.sin(a)
        pc.fine.box((x - 0.018, y - 0.018, 0.1), (x + 0.018, y + 0.018, 1.25), 'DarkSteel')
    # valve on top
    pc.fine.cyl(V3(0, 0, 1.24), V3(0, 0, 1.33), 0.035, 'Steel', seg=16)
    pc.fine.ring(V3(0, 0, 1.34), V3(0, 0, 1), 0.07, 0.009, 'PipeRed', seg=24, tseg=8)
    pc.fine.cyl(V3(0, 0, 1.29), V3(0.12, 0, 1.29), 0.014, 'Steel', seg=10)
    pc.fine.cyl(V3(0.12, 0, 1.29), V3(0.12, 0, 1.25), 0.03, 'Gunmetal', seg=16)
    text_at(pc.flat, 'O2', 0.09, V3(0, -(R + 0.0025), 0.78), V3(1, 0, 0), V3(0, 0, 1), 'PaintDark')
    return pc.finish(parent)


# ---------------------------------------------------------------------------
# Console
# ---------------------------------------------------------------------------


def console(name, parent):
    pc = Piece(name)
    # pedestal
    pc.main.box((-0.36, -0.22, 0.0), (0.36, 0.22, 0.08), 'Gunmetal')
    f = pc.main.box((-0.24, -0.14, 0.08), (0.24, 0.14, 0.86), 'PanelGrey')
    for key in ('-y', '+y'):
        pc.main.inset(f[key], 0.03, 0.0)
        pc.main.inset(f[key], 0.004, -0.012, rim_mat='PanelDark', inner_mat='PanelDark')
    for j in range(8):
        pc.fine.box((-0.16, -0.152, 0.25 + j * 0.035), (0.16, -0.14, 0.262 + j * 0.035), 'Recess')
    # slanted desk (tilted 28 degrees toward the user at -Y)
    tilt = Matrix.Rotation(math.radians(28), 4, 'X')
    M = Matrix.Translation(V3(0, -0.05, 0.98)) @ tilt
    top = pc.main.box((-0.55, -0.3, -0.05), (0.55, 0.3, 0.03), 'PanelLight', M=M, mats={'+z': 'Gunmetal'})
    pc.main.inset(top['+z'], 0.03, 0.0)
    pc.main.inset(top['+z'], 0.004, -0.008, rim_mat='DarkSteel', inner_mat='PanelDark')
    up = (M.to_3x3() @ V3(0, 0, 1)).normalized()
    ax = V3(1, 0, 0)
    ay = (M.to_3x3() @ V3(0, 1, 0)).normalized()
    origin = M @ V3(0, 0, 0.03 - 0.008)
    # screen (upper half) and keyboard (lower half)
    sc = origin + ay * 0.12 + up * 0.002
    pc.flat.face([sc - ax * 0.42 - ay * 0.13, sc + ax * 0.42 - ay * 0.13, sc + ax * 0.42 + ay * 0.13,
                  sc - ax * 0.42 + ay * 0.13], 'ScreenDim', toward=up)
    for i, (dx, dy, w, h) in enumerate(((-0.25, 0.06, 0.25, 0.02), (-0.25, 0.02, 0.18, 0.02), (-0.25, -0.02, 0.22, 0.02),
                                        (0.2, 0.0, 0.3, 0.16))):
        c = sc + ax * dx + ay * dy + up * 0.001
        mat = 'Screen' if i < 3 else 'Recess'
        pc.flat.face([c - ax * w / 2 - ay * h / 2, c + ax * w / 2 - ay * h / 2, c + ax * w / 2 + ay * h / 2,
                      c - ax * w / 2 + ay * h / 2], mat, toward=up)
    for k in range(9):
        x = 0.07 + k * 0.03
        h = 0.02 + 0.1 * abs(math.sin(k * 1.7))
        c = sc + ax * x + ay * (-0.07 + h / 2) + up * 0.002
        pc.flat.face([c - ax * 0.01 - ay * h / 2, c + ax * 0.01 - ay * h / 2, c + ax * 0.01 + ay * h / 2,
                      c - ax * 0.01 + ay * h / 2], 'LightGreen' if k % 3 else 'LightAmber', toward=up)
    kb = origin - ay * 0.14
    for r in range(4):
        for c in range(12):
            p = kb + ax * (-0.33 + c * 0.06) + ay * (-0.06 + r * 0.045)
            Mk = frame_matrix(p, ax, ay)
            pc.fine.box((-0.024, -0.017, 0.0), (0.024, 0.017, 0.014), 'PanelLight' if (r + c) % 7 else 'AccentOrange',
                        M=Mk)
    text_at(pc.flat, 'NAV-2', 0.035, origin + ay * 0.26 - ax * 0.35 + up * 0.001, ax, ay, 'Paint')
    return pc.finish(parent)


def build_props(parent):
    crate('crate_small', parent, (0.6, 0.6, 0.6), 'CrateOlive', 'BH-07')
    crate('crate_medium', parent, (1.2, 0.8, 0.8), 'CrateOrange', 'CARGO 3', stripes=True)
    crate('crate_long', parent, (1.6, 0.7, 0.55), 'CrateGrey', 'AMMO', stripes=True)
    barrel('barrel_blue', parent, 'BarrelBlue', band=('Paint', 'PanelDark'))
    barrel('barrel_red', parent, 'BarrelRed')
    canister('canister', parent)
    console('console', parent)
