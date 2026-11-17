"""Bulkhead kit: door frame and door leaves.

Door pieces are modelled on a cell edge: the edge is the local XZ plane (y = 0) and
the passage runs along local Y. The frame's outside matches the corridor
cross-section exactly, so it plugs the corridor end and the doorway cut in a room
wall. Leaves are separate nodes (LeafL / LeafR / Leaf) so the engine can animate
them: sliding leaves move along local X, the bulkhead leaf moves up along Z.
"""

import math

from mathutils import Matrix, Vector

from bk_core import V2, V3, Builder, Piece, miters, text_at
from bk_corridor import OUTLINE

DEPTH = 0.3  # frame half-depth along Y
DOOR = [(-0.75, 0.0), (-0.75, 0.45), (-0.75, 2.0), (-0.45, 2.3), (0.45, 2.3), (0.75, 2.0), (0.75, 0.45), (0.75, 0.0)]
SLOT = 0.06  # half width of the leaf slot in the jambs


def _outward(a, b, inside):
    """Horizontal-plane outward direction for an edge a-b (x,z) away from `inside`."""
    mid = (a + b) * 0.5
    d = (b - a).normalized()
    n = V2(-d.y, d.x)
    if n.dot(mid - inside) < 0:
        n = -n
    return V3(n.x, 0, n.y)


def resample(outline, step):
    pts = [V2(*outline[0])]
    for a, b in zip(outline, outline[1:]):
        a, b = V2(*a), V2(*b)
        k = max(1, round((b - a).length / step))
        for j in range(1, k + 1):
            pts.append(a.lerp(b, j / k))
    return pts


def hazard_ring(b, y, facing, width=0.1, step=0.05):
    """Diagonal hazard stripes around the doorway on the frame face at depth y."""
    inner = resample(DOOR[1:-1], step)   # skip the floor-level ends
    ms = miters(inner)                    # left normals = outward from the doorway
    outer = [p + m * width for p, m in zip(inner, ms)]
    n = len(inner)
    for i in range(n - 1):
        j0, j1 = min(i + 1, n - 1), min(i + 2, n - 1)
        quad = [V3(inner[i].x, y, inner[i].y), V3(inner[i + 1].x, y, inner[i + 1].y),
                V3(outer[j1].x, y, outer[j1].y), V3(outer[j0].x, y, outer[j0].y)]
        m = 'HazardYellow' if (i // 2) % 2 == 0 else 'HazardBlack'
        b.face(quad, m, toward=facing)


def keypad(pc, x, y, facing):
    s = 1.0 if facing.y > 0 else -1.0
    pc.fine.box((x - 0.085, y, 1.18), (x + 0.085, y + s * 0.045, 1.46), 'Gunmetal') if s > 0 else \
        pc.fine.box((x - 0.085, y - 0.045, 1.18), (x + 0.085, y, 1.46), 'Gunmetal')
    yf = y + s * 0.047
    pc.flat.face([V3(x - 0.06, yf, 1.36), V3(x + 0.06, yf, 1.36), V3(x + 0.06, yf, 1.43), V3(x - 0.06, yf, 1.43)],
                 'Screen', toward=facing)
    for r in range(3):
        for c in range(3):
            bx, bz = x - 0.05 + c * 0.05, 1.215 + r * 0.045
            y0, y1 = sorted((y + s * 0.045, y + s * 0.056))
            pc.fine.box((bx - 0.017, y0, bz - 0.015), (bx + 0.017, y1, bz + 0.015),
                        'LightAmber' if (r, c) == (0, 2) else 'PanelLight')


def build_frame(parent):
    pc = Piece('door_frame')
    out2 = [V2(*p) for p in OUTLINE]
    in2 = [V2(*p) for p in DOOR]
    centre = V2(0, 1.2)
    fy, by = V3(0, 1, 0), V3(0, -1, 0)
    front_faces = []
    for k in range(len(out2) - 1):
        oa, ob, ia, ib = out2[k], out2[k + 1], in2[k], in2[k + 1]
        for y, facing in ((DEPTH, fy), (-DEPTH, by)):
            f = pc.main.face([V3(oa.x, y, oa.y), V3(ob.x, y, ob.y), V3(ib.x, y, ib.y), V3(ia.x, y, ia.y)],
                             'PanelGrey', toward=facing)
            front_faces.append((k, f))
        # outside skin (visible where the frame stands proud of a room wall)
        pc.main.face([V3(oa.x, -DEPTH, oa.y), V3(ob.x, -DEPTH, ob.y), V3(ob.x, DEPTH, ob.y), V3(oa.x, DEPTH, oa.y)],
                     'PanelGrey', toward=_outward(oa, ob, centre))
        # jamb, split around the leaf slot
        into = -_outward(ia, ib, centre)
        for y0, y1, m in ((-DEPTH, -SLOT, 'Gunmetal'), (-SLOT, SLOT, 'Recess'), (SLOT, DEPTH, 'Gunmetal')):
            pc.main.face([V3(ia.x, y0, ia.y), V3(ib.x, y0, ib.y), V3(ib.x, y1, ib.y), V3(ia.x, y1, ia.y)], m,
                         toward=into)
    for k, f in front_faces:
        if k in (1, 2, 6, 5):   # tall side pillars and the angled upper parts
            pc.main.inset(f, 0.03, 0.0)
            pc.main.inset(f, 0.004, -0.015, rim_mat='PanelDark', inner_mat='PanelDark')
        elif k == 3:            # header
            pc.main.inset(f, 0.03, 0.0)
            pc.main.inset(f, 0.004, -0.01, rim_mat='PanelDark', inner_mat='PanelLight')

    # flange around the outside on both faces (hides the cut in a room wall)
    ms = miters(out2)
    flange = [p + m * 0.14 for p, m in zip(out2, ms)]  # left of travel = outward
    for y0, y1, facing in ((DEPTH, DEPTH + 0.035, fy), (-DEPTH - 0.035, -DEPTH, by)):
        yf = y1 if facing.y > 0 else y0
        for k in range(len(out2) - 1):
            oa, ob, fa, fb = out2[k], out2[k + 1], flange[k], flange[k + 1]
            pc.main.face([V3(oa.x, yf, oa.y), V3(ob.x, yf, ob.y), V3(fb.x, yf, fb.y), V3(fa.x, yf, fa.y)],
                         'Gunmetal', toward=facing)
            pc.main.face([V3(fa.x, y0, fa.y), V3(fb.x, y0, fb.y), V3(fb.x, y1, fb.y), V3(fa.x, y1, fa.y)],
                         'Gunmetal', toward=_outward(fa, fb, centre))

    # hazard stripes, status light, keypad, label on both faces
    for y, facing in ((DEPTH + 0.002, fy), (-DEPTH - 0.002, by)):
        hazard_ring(pc.flat, y, facing)
        s = facing.y
        pc.fine.box((-0.32, min(y, y + s * 0.03), 2.43), (0.32, max(y, y + s * 0.03), 2.55), 'Gunmetal')
        pc.flat.face([V3(-0.28, y + s * 0.032, 2.455), V3(0.28, y + s * 0.032, 2.455),
                      V3(0.28, y + s * 0.032, 2.525), V3(-0.28, y + s * 0.032, 2.525)], 'LightGreen', toward=facing)
        keypad(pc, -0.97 * s, y, facing)
        text_at(pc.flat, 'D-03', 0.1, V3(0.0, y - s * 0.0085, 2.68), V3(-s, 0, 0), V3(0, 0, 1), 'PaintDark')
        # bolt columns on the pillars
        for x in (-1.08, 1.08):
            for z in (0.7, 1.0, 1.6, 1.9):
                p = V3(x, y, z)
                pc.fine.cyl(p - fy * s * 0.002, p + fy * s * 0.012, 0.014, 'Steel', seg=8)

    # threshold plate with the leaf groove
    pc.fine.box((-0.9, -DEPTH, 0.0), (0.9, -SLOT, 0.025), 'Gunmetal')
    pc.fine.box((-0.9, SLOT, 0.0), (0.9, DEPTH, 0.025), 'Gunmetal')
    pc.fine.box((-0.9, -SLOT, 0.0), (0.9, SLOT, 0.008), 'Recess')
    for y0 in (-DEPTH + 0.03, SLOT + 0.03):
        for j in range(16):
            x = -0.8 + j * 0.1
            m = 'HazardYellow' if j % 2 == 0 else 'HazardBlack'
            pc.flat.face([V3(x, y0, 0.0255), V3(x + 0.1, y0, 0.0255), V3(x + 0.16, y0 + 0.15, 0.0255),
                          V3(x + 0.06, y0 + 0.15, 0.0255)], m, toward=V3(0, 0, 1))
    return pc.finish(parent)


# ---------------------------------------------------------------------------
# Leaves
# ---------------------------------------------------------------------------


class Leaf:
    """Flat prism leaf: front at +t, back at -t, built from convex 2D cells (x, z)
    that tile the leaf face; cells listed in `holes` are left open (windows)."""

    def __init__(self, name, t):
        self.body = Builder()
        self.fine = Builder()
        self.flat = Builder()
        self.name = name
        self.t = t

    def prism(self, cells, outline, centre, mat='PanelLight', side_mat='Gunmetal'):
        fy, by = V3(0, 1, 0), V3(0, -1, 0)
        faces = []
        for cell in cells:
            for y, facing in ((self.t, fy), (-self.t, by)):
                f = self.body.face([V3(x, y, z) for x, z in cell], mat, toward=facing)
                faces.append((cell, facing, f))
        o2 = [V2(*p) for p in outline]
        for a, b in zip(o2, o2[1:] + o2[:1]):
            self.body.face([V3(a.x, -self.t, a.y), V3(b.x, -self.t, b.y), V3(b.x, self.t, b.y),
                            V3(a.x, self.t, a.y)], side_mat, toward=_outward(a, b, centre))
        return faces

    def hole_reveal(self, rect, mat='Gunmetal'):
        x0, z0, x1, z1 = rect
        c = V2((x0 + x1) / 2, (z0 + z1) / 2)
        loop = [V2(x0, z0), V2(x1, z0), V2(x1, z1), V2(x0, z1)]
        for a, b in zip(loop, loop[1:] + loop[:1]):
            into = -_outward(a, b, c)
            self.body.face([V3(a.x, -self.t, a.y), V3(b.x, -self.t, b.y), V3(b.x, self.t, b.y),
                            V3(a.x, self.t, a.y)], mat, toward=into)

    def objects(self, pc):
        pc.extra_objects.append((self.name, self.body, 0.006))
        pc.extra_objects.append((self.name + '_detail', self.fine, 0.002))
        pc.extra_objects.append((self.name + '_glass', self.flat, 0.0))


def sliding_leaf(sign, name):
    """Half of the sliding door. sign = -1 for the left leaf, +1 for the right.
    The leaf runs 0.05 m into the jamb slot and 0.03 m into the header."""
    t = 0.04
    X = lambda x: x * sign  # noqa: E731
    xo, xm = 0.80, 0.0          # outer edge (in the jamb), meeting edge
    win = (0.16, 1.38, 0.30, 1.86)  # window x0, z0, x1, z1 (distance from centre line)
    zb, zt = 0.026, 2.33
    cells = [
        [(X(xm), zb), (X(xo), zb), (X(xo), win[1]), (X(xm), win[1])],
        [(X(xm), win[1]), (X(win[0]), win[1]), (X(win[0]), win[3]), (X(xm), win[3])],
        [(X(win[2]), win[1]), (X(xo), win[1]), (X(xo), win[3]), (X(win[2]), win[3])],
        [(X(xm), win[3]), (X(xo), win[3]), (X(xo), 2.0), (X(xm), 2.0)],
        [(X(xm), 2.0), (X(xo), 2.0), (X(0.47), zt), (X(xm), zt)],
    ]
    outline = [(X(xm), zb), (X(xo), zb), (X(xo), 2.0), (X(0.47), zt), (X(xm), zt)]
    leaf = Leaf(name, t)
    centre = V2(X(0.4), 1.2)
    faces = leaf.prism(cells, outline, centre, mat='PanelLight', side_mat='Gunmetal')
    leaf.hole_reveal((min(X(win[0]), X(win[2])), win[1], max(X(win[0]), X(win[2])), win[3]))
    for cell, facing, f in faces:
        if cell is cells[0]:
            leaf.body.inset(f, 0.05, 0.0)
            leaf.body.inset(f, 0.005, -0.012, rim_mat='PanelGrey', inner_mat='PanelGrey')
        elif cell is cells[3] or cell is cells[4]:
            leaf.body.inset(f, 0.03, 0.0)
            leaf.body.inset(f, 0.004, -0.008, rim_mat='PanelGrey', inner_mat='PanelWhite')
    fy = V3(0, 1, 0)
    wx0, wx1 = sorted((X(win[0]), X(win[2])))
    for y, facing in ((t, fy), (-t, -fy)):
        s = facing.y
        # window frame ring
        fr = 0.03
        rects = [((wx0 - fr, win[1] - fr), (wx1 + fr, win[1])),
                 ((wx0 - fr, win[3]), (wx1 + fr, win[3] + fr)),
                 ((wx0 - fr, win[1]), (wx0, win[3])),
                 ((wx1, win[1]), (wx1 + fr, win[3]))]
        for (a0, b0), (a1, b1) in rects:
            y0, y1 = sorted((y, y + s * 0.012))
            leaf.fine.box((a0, y0, b0), (a1, y1, b1), 'Gunmetal')
        # orange accent band and a recessed pull handle near the meeting edge
        y0, y1 = sorted((y, y + s * 0.008))
        leaf.fine.box((min(X(0.06), X(0.72)), y0, 0.96), (max(X(0.06), X(0.72)), y1, 1.04), 'AccentOrange')
        hx = X(0.1)
        leaf.fine.box((hx - 0.02, y0, 1.1), (hx + 0.02, y1, 1.32), 'Recess')
        # stencil on the recessed lower panel
        text_at(leaf.flat, '07', 0.16, V3(X(0.42), y - s * 0.0105, 0.75), V3(-s, 0, 0), V3(0, 0, 1), 'PaintDark')
    # glass pane and rubber seal on the meeting edge
    leaf.flat.face([V3(wx0, 0, win[1]), V3(wx1, 0, win[1]), V3(wx1, 0, win[3]), V3(wx0, 0, win[3])], 'Glass',
                   toward=fy)
    leaf.flat.face([V3(wx0, 0, win[1]), V3(wx1, 0, win[1]), V3(wx1, 0, win[3]), V3(wx0, 0, win[3])], 'Glass',
                   toward=-fy)
    leaf.fine.box((min(X(-0.012), X(0.0)), -t * 0.7, zb), (max(X(-0.012), X(0.0)), t * 0.7, zt), 'Rubber')
    return leaf


def build_sliding(parent):
    pc = Piece('door_sliding')
    sliding_leaf(-1, 'LeafL').objects(pc)
    sliding_leaf(+1, 'LeafR').objects(pc)
    return pc.finish(parent)


def build_bulkhead(parent, airlock=False):
    """Heavy single leaf that slides up into the header."""
    name = 'door_airlock' if airlock else 'door_bulkhead'
    pc = Piece(name)
    t = 0.07
    zb, zt, xo = 0.026, 2.33, 0.80
    outline = [(-xo, zb), (xo, zb), (xo, 2.0), (0.47, zt), (-0.47, zt), (-xo, 2.0)]
    cells = [[(-xo, zb), (xo, zb), (xo, 0.9), (-xo, 0.9)],
             [(-xo, 0.9), (xo, 0.9), (xo, 2.0), (-xo, 2.0)],
             [(-xo, 2.0), (xo, 2.0), (0.47, zt), (-0.47, zt)]]
    leaf = Leaf('Leaf', t)
    faces = leaf.prism(cells, outline, V2(0, 1.2), mat='PanelGrey', side_mat='Gunmetal')
    for cell, facing, f in faces:
        if cell is cells[1]:
            leaf.body.inset(f, 0.06, 0.0)
            leaf.body.inset(f, 0.006, -0.02, rim_mat='Gunmetal', inner_mat='PanelLight' if not airlock else 'PanelGrey')
    fy = V3(0, 1, 0)
    for y, facing in ((t, fy), (-t, -fy)):
        s = facing.y
        y0, y1 = sorted((y, y + s * 0.006))
        # diagonal hazard stripes over the lower part
        for j in range(-2, 19):
            x = -0.8 + j * 0.1
            m = ('PipeRed' if airlock else 'HazardYellow') if j % 2 == 0 else 'HazardBlack'
            quad = [(x, zb + 0.06), (x + 0.1, zb + 0.06), (x + 0.1 + 0.36, 0.84), (x + 0.36, 0.84)]
            # clip to the leaf width
            quad = [(max(-0.74, min(0.74, qx)), qz) for qx, qz in quad]
            if abs(quad[0][0] - quad[1][0]) + abs(quad[2][0] - quad[3][0]) < 1e-4:
                continue
            leaf.flat.face([V3(qx, y + s * 0.001, qz) for qx, qz in quad], m, toward=facing)
        # reinforcement: two horizontal bars and a vertical spine
        for zc in (1.15, 1.75):
            leaf.fine.box((-0.7, y0 if s > 0 else y1 - 0.03, zc - 0.05), (0.7, y0 + 0.03 if s > 0 else y1, zc + 0.05),
                          'Gunmetal')
        # locking wheel
        c0 = V3(0, y, 1.45)
        leaf.fine.cyl(c0, c0 + fy * s * 0.05, 0.2, 'DarkSteel', seg=28)
        leaf.fine.ring(c0 + fy * s * 0.075, fy, 0.17, 0.018, 'Steel', seg=28, tseg=8)
        for k in range(3):
            a = k * math.pi / 3
            d = V3(math.cos(a), 0, math.sin(a))
            leaf.fine.cyl(c0 + fy * s * 0.075 - d * 0.17, c0 + fy * s * 0.075 + d * 0.17, 0.012, 'Steel', seg=8)
        leaf.fine.cyl(c0 + fy * s * 0.05, c0 + fy * s * 0.1, 0.045, 'Gunmetal', seg=16)
        for k in range(12):
            a = k * math.pi / 6
            p = c0 + V3(math.cos(a) * 0.24, 0, math.sin(a) * 0.24)
            leaf.fine.cyl(p, p + fy * s * 0.018, 0.016, 'Steel', seg=8)
        if airlock:
            leaf.flat.face([V3(-0.6, y + s * 0.003, 2.07), V3(0.6, y + s * 0.003, 2.07), V3(0.6, y + s * 0.003, 2.12),
                            V3(-0.6, y + s * 0.003, 2.12)], 'LightRed', toward=facing)
        label = 'AIRLOCK' if airlock else 'BULKHEAD 7'
        text_at(leaf.flat, label, 0.12, V3(0, y - s * 0.0185, 1.885), V3(-s, 0, 0), V3(0, 0, 1),
                'PaintDark' if not airlock else 'Paint')
    leaf.objects(pc)
    return pc.finish(parent)
