"""Bulkhead kit: corridor_straight_window, a straight corridor with two viewports in its
east wall looking out into space (part 3). The engine puts it on straight corridor
cells along the edge of the ship, turned so the windows face outward.

Built in stages so each modelling step can be shown (video/blender/window/step_NN.png):

  1  the straight corridor it starts from
  2  the east wall's panels cut away where the viewports go (trim rail capped)
  3  a flat wall plate with two rounded openings
  4  the reveals: the hull's thickness, splayed toward the glass
  5  raised bezels round the openings, bolted on
  6  rubber gaskets and the glass (BH_ViewGlass: clear, slightly tinted, glossy)
  7  the sill with a light strip underneath, a handrail and stencils

The windows sit between the cell's central rib (which becomes the mullion) and the
ends of the cell, so the walls where cells meet are the normal profile.
"""

import math

from mathutils import Matrix, Vector

import bk_corridor as cor
from bk_core import V2, V3, Piece, frame_matrix, text

W, E, CEIL = cor.W, cor.E, cor.CEIL
SECTION = 1.6                 # the cut runs from y = -1.6 to 1.6
CENTRES = (-0.86, 0.86)       # viewport centres along the wall (y)
HW, HH, RAD = 0.58, 0.55, 0.22  # opening half width / half height / corner radius at the wall
ZC = 1.42                     # opening centre height (z 0.87 .. 1.97; the bezel clears the pipe at 2.13)
BEZEL = 0.12                  # bezel width round the opening
X_GLASS = 1.50                # glass plane (the wall face is at x = W = 1.2)
X_OUT = 1.64                  # outer end of the reveal (open to space)
SPLAY = 0.08                  # the reveal narrows by this much toward the glass
SILL_Z = 0.68
CUT_KS = (cor.K_WLOW, cor.K_TRB, cor.K_TRF, cor.K_TRT, cor.K_WUP)


def rrect(yc, zc, hw, hh, r, K=10):
    """A rounded rectangle in the (y, z) plane, counter-clockwise from the (+y, +z)
    corner. Returns points and, per point, which corner arc (0..3) it is on."""
    r = max(1e-4, min(r, hw, hh))
    centres = ((hw - r, hh - r, 0.0), (-(hw - r), hh - r, 90.0), (-(hw - r), -(hh - r), 180.0),
               (hw - r, -(hh - r), 270.0))
    pts, corner = [], []
    for ci, (cy, cz, a0) in enumerate(centres):
        for k in range(K + 1):
            a = math.radians(a0 + 90.0 * k / K)
            pts.append((yc + cy + r * math.cos(a), zc + cz + r * math.sin(a)))
            corner.append(ci)
    return pts, corner


def loop3(x, pts):
    return [V3(x, y, z) for y, z in pts]


def strip(b, la, lb, mat, axis):
    """Quads between two loops of equal length; faces point toward the opening axis
    (yc, zc) given as axis."""
    n = len(la)
    for i in range(n):
        j = (i + 1) % n
        mid = (la[i] + la[j] + lb[i] + lb[j]) * 0.25
        toward = V3(0, axis[0] - mid.y, axis[1] - mid.z)
        b.face([la[i], la[j], lb[j], lb[i]], mat, toward=toward)


def ring_out(b, la, lb, mat, axis):
    """Like strip, but facing away from the axis (the outside slope of a bezel)."""
    n = len(la)
    for i in range(n):
        j = (i + 1) % n
        mid = (la[i] + la[j] + lb[i] + lb[j]) * 0.25
        b.face([la[i], la[j], lb[j], lb[i]], mat, toward=V3(-0.4, mid.y - axis[0], mid.z - axis[1]))


def flat_ring(b, la, lb, mat, toward):
    n = len(la)
    for i in range(n):
        j = (i + 1) % n
        b.face([la[i], la[j], lb[j], lb[i]], mat, toward=toward)


def east_wall(pc, cut):
    """The east wall (x = W), with the panels between the trim and the upper chamfer cut
    away over the viewport section when `cut`."""
    corners = [(W, -E), (W, -SECTION), (W, SECTION), (W, E)]

    def omit(bay, k):
        mid = (bay.a + bay.b) * 0.5
        return cut and abs(mid.y) < SECTION and k in CUT_KS

    cor.wall_run(pc, corners, details=False, omit=omit)
    if cut:
        # cap the trim rail where it stops
        rail = [(0.0, 1.22), (0.035, 1.25), (0.035, 1.33), (0.0, 1.36)]
        for y in (-SECTION, SECTION):
            pc.main.face([V3(W - o, y, z) for o, z in rail], 'Gunmetal', toward=V3(0, -math.copysign(1, y), 0))


def wall_plate(pc, yc):
    """The flat plate over half the section with a rounded opening (x = W, facing -x)."""
    y0, y1 = (0.0, SECTION) if yc > 0 else (-SECTION, 0.0)
    z0, z1 = 0.45, 2.25
    oc = [V3(W, y1, z1), V3(W, y0, z1), V3(W, y0, z0), V3(W, y1, z0)]
    pts, corner = rrect(yc, ZC, HW, HH, RAD)
    inner = loop3(W, pts)
    n = len(inner)
    into = V3(-1, 0, 0)
    for i in range(n):
        j = (i + 1) % n
        ci, cj = corner[i], corner[j]
        if ci == cj:
            pc.main.face([oc[ci], inner[j], inner[i]], 'PanelLight', toward=into)
        else:
            pc.main.face([oc[ci], oc[cj], inner[j], inner[i]], 'PanelLight', toward=into)


def reveal(pc, yc):
    """The hull's thickness: a short lip, then a splayed tunnel to the glass and beyond."""
    a = rrect(yc, ZC, HW, HH, RAD)[0]
    b = rrect(yc, ZC, HW - SPLAY, HH - SPLAY, RAD - 0.05)[0]
    l0, l1 = loop3(W, a), loop3(W + 0.04, a)
    l2, l3 = loop3(X_GLASS, b), loop3(X_OUT, b)
    axis = (yc, ZC)
    strip(pc.main, l0, l1, 'Gunmetal', axis)
    strip(pc.main, l1, l2, 'PanelDark', axis)
    strip(pc.main, l2, l3, 'DarkSteel', axis)


def bezel(pc, yc):
    """A raised frame round the opening, standing 45 mm proud, with 18 bolts."""
    axis = (yc, ZC)
    o = loop3(W, rrect(yc, ZC, HW + BEZEL, HH + BEZEL, RAD + BEZEL)[0])
    o1 = loop3(W - 0.045, rrect(yc, ZC, HW + BEZEL - 0.03, HH + BEZEL - 0.03, RAD + BEZEL - 0.03)[0])
    i1 = loop3(W - 0.045, rrect(yc, ZC, HW + 0.025, HH + 0.025, RAD + 0.025)[0])
    i0 = loop3(W - 0.002, rrect(yc, ZC, HW, HH, RAD)[0])
    ring_out(pc.fine, o, o1, 'Gunmetal', axis)
    flat_ring(pc.fine, o1, i1, 'Gunmetal', V3(-1, 0, 0))
    strip(pc.fine, i1, i0, 'DarkSteel', axis)
    # bolts, evenly spaced round the middle of the bezel face
    mid = rrect(yc, ZC, HW + BEZEL * 0.5, HH + BEZEL * 0.5, RAD + BEZEL * 0.5, K=12)[0]
    seg = [((Vector(mid[(i + 1) % len(mid)]) - Vector(mid[i])).length) for i in range(len(mid))]
    total = sum(seg)
    count = 18
    acc, k, i = 0.0, 0, 0
    while k < count and i < len(mid):
        target = (k + 0.5) * total / count
        if acc + seg[i] >= target:
            t = (target - acc) / seg[i]
            p = Vector(mid[i]).lerp(Vector(mid[(i + 1) % len(mid)]), t)
            pc.fine.cyl(V3(W - 0.044, p.x, p.y), V3(W - 0.056, p.x, p.y), 0.013, 'Steel', seg=6)
            k += 1
        else:
            acc += seg[i]
            i += 1


def glass(pc, yc):
    """A rubber gasket and the pane: one face toward the corridor, one toward space."""
    g0 = loop3(X_GLASS - 0.004, rrect(yc, ZC, HW - SPLAY, HH - SPLAY, RAD - 0.05)[0])
    g1 = loop3(X_GLASS - 0.004, rrect(yc, ZC, HW - SPLAY - 0.035, HH - SPLAY - 0.035, RAD - 0.08)[0])
    flat_ring(pc.fine, g0, g1, 'Rubber', V3(-1, 0, 0))
    pane = rrect(yc, ZC, HW - SPLAY - 0.03, HH - SPLAY - 0.03, RAD - 0.075)[0]
    pc.flat.face(loop3(X_GLASS, pane), 'ViewGlass', toward=V3(-1, 0, 0))
    pc.flat.face(loop3(X_GLASS + 0.006, pane), 'ViewGlass', toward=V3(1, 0, 0))


def sill(pc):
    """A sill across the section with a light strip under it, and stencils."""
    y0, y1 = -SECTION + 0.06, SECTION - 0.06
    pc.main.box((W - 0.16, y0, SILL_Z - 0.035), (W, y1, SILL_Z + 0.015), 'Gunmetal')
    pc.fine.box((W - 0.17, y0, SILL_Z + 0.015), (W - 0.15, y1, SILL_Z + 0.03), 'DarkSteel')  # front lip
    pc.flat.box((W - 0.12, y0 + 0.05, SILL_Z - 0.042), (W - 0.03, y1 - 0.05, SILL_Z - 0.036), 'LightTeal')
    for yc in CENTRES:
        # stencil and a pressure light under each viewport (viewer faces +x: right is -y)
        M = frame_matrix(V3(W - 0.003, yc, 0.58), V3(0, -1, 0), V3(0, 0, 1))
        text(pc.flat, 'VIEWPORT %s' % ('7-A' if yc < 0 else '7-B'), 0.055, M, 'PaintYellow')
        pc.flat.obox(V3(W - 0.004, yc + math.copysign(0.36, yc), 0.58), (0.004, 0.04, 0.04), 'LightGreen')
        M2 = frame_matrix(V3(W - 0.003, yc, 0.5), V3(0, -1, 0), V3(0, 0, 1))
        text(pc.flat, 'PRESSURE HULL - DO NOT STRIKE', 0.026, M2, 'Paint')


def build_window(parent, upto=99, name='corridor_straight_window'):
    """The full piece (upto=99) or the first `upto` modelling stages of it."""
    open_sides, polys = cor.TYPES['straight']
    pc = Piece(name, seed='corridor_straight')
    cor.wall_run(pc, polys[0])                     # west wall, dressed as usual
    east_wall(pc, cut=upto >= 2)
    cor.floor(pc, open_sides)
    cor.ceiling(pc, open_sides, 'straight')
    cor.ribs(pc, open_sides, 'straight')
    for yc in CENTRES:
        if upto >= 3:
            wall_plate(pc, yc)
        if upto >= 4:
            reveal(pc, yc)
        if upto >= 5:
            bezel(pc, yc)
        if upto >= 6:
            glass(pc, yc)
    if upto >= 7:
        sill(pc)
    return pc.finish(parent)
