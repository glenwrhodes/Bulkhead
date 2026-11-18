"""Bulkhead kit: corridor modules and the wall details shared with rooms.

A corridor cell is 4 x 4 m; the walkway is 2.4 m wide (walls at +-1.2 m), the
ceiling is at 2.85 m. Canonical orientations (Blender local, Y = north):
  straight  open N,S        corner  open N,E        tee  open N,E,S
  cross     open N,E,S,W    end     open N
The engine rotates pieces in 90 degree steps. Everything that crosses a cell edge
(walls, trim, pipes, floor trough, light strips) is identical on both walls, so
any rotation of any neighbour continues it seamlessly.
"""

import math

from mathutils import Matrix, Vector

from bk_core import V2, V3, Piece, frame_matrix, miters, perp, subdivide, text

W = 1.2       # half walkway width
E = 2.0       # half cell
CEIL = 2.85   # corridor ceiling height

# Wall cross-section: (o = offset toward walkway, z). Lower chamfer, lower panel,
# trim rail (3 faces), upper panel, upper chamfer.
WALL = [(0.30, 0.0), (0.0, 0.45), (0.0, 1.22), (0.035, 1.25), (0.035, 1.33), (0.0, 1.36),
        (0.0, 2.25), (0.45, CEIL)]
K_CHLOW, K_WLOW, K_TRB, K_TRF, K_TRT, K_WUP, K_CHUP = range(7)
WALL_MATS = {K_CHLOW: 'PanelGrey', K_WLOW: 'PanelWhite', K_TRB: 'Gunmetal', K_TRF: 'Gunmetal',
             K_TRT: 'Gunmetal', K_WUP: 'PanelWhite', K_CHUP: 'PanelLight'}

# Corridor interior outline in the (x, z) cross-section, interior on the right.
OUTLINE = [(-0.9, 0.0), (-W, 0.45), (-W, 2.25), (-0.75, CEIL), (0.75, CEIL), (W, 2.25), (W, 0.45), (0.9, 0.0)]

# Pipes riding the upper chamfer: (o, z, radius, material).
_CH_DIR = V2(0.6, 0.8)    # chamfer direction in (o, z)
_CH_NRM = V2(0.8, -0.6)   # chamfer normal (into the corridor)


def _on_chamfer(s, standoff):
    p = V2(0.0, 2.25) + _CH_DIR * (0.75 * s) + _CH_NRM * standoff
    return p.x, p.y


PIPES = [(*_on_chamfer(0.30, 0.062 + 0.02), 0.062, 'PipeBlue'),
         (*_on_chamfer(0.70, 0.044 + 0.02), 0.044, 'Copper'),
         (0.04, 2.13, 0.022, 'DarkSteel')]

TYPES = {
    'straight': ('NS', [[(-W, E), (-W, -E)], [(W, -E), (W, E)]]),
    'corner': ('NE', [[(-W, E), (-W, -W), (E, -W)], [(E, W), (W, W), (W, E)]]),
    'tee': ('NES', [[(-W, E), (-W, -E)], [(E, W), (W, W), (W, E)], [(W, -E), (W, -W), (E, -W)]]),
    'cross': ('NESW', [[(E, W), (W, W), (W, E)], [(-W, E), (-W, W), (-E, W)],
                       [(-E, -W), (-W, -W), (-W, -E)], [(W, -E), (W, -W), (E, -W)]]),
    'end': ('N', [[(-W, E), (-W, -W), (W, -W), (W, E)]]),
}
SIDE_DIR = {'N': V2(0, 1), 'E': V2(1, 0), 'S': V2(0, -1), 'W': V2(-1, 0)}

SIGNS = ['COOLANT', 'AUX PWR', 'MAINT 07', 'LIFE SUPPORT', 'D3-B', 'SECTOR 4', 'O2 LINE', 'HIGH VOLTAGE']


def side_rotation(side):
    """Rotation (about Z) taking canonical north (+Y) to the given side."""
    d = SIDE_DIR[side]
    return Matrix.Rotation(math.atan2(-d.x, d.y), 4, 'Z')


def circle(o, z, r, seg=12):
    return [(o + r * math.cos(2 * math.pi * i / seg), z + r * math.sin(2 * math.pi * i / seg)) for i in range(seg)]


# ---------------------------------------------------------------------------
# Wall frame helper: a straight wall bay
# ---------------------------------------------------------------------------


class Bay:
    """A straight stretch of wall from a to b (2D), walkway on the left."""

    def __init__(self, a, b):
        self.a, self.b = a, b
        self.d = (b - a).normalized()
        self.n = perp(self.d)
        self.L = (b - a).length

    def p(self, s, o, z):
        q = self.a + self.d * s + self.n * o
        return V3(q.x, q.y, z)

    def M(self, s, o, z):
        """Local frame on the wall: X = viewer's right, Y = up, Z = out of the wall."""
        return frame_matrix(self.p(s, o, z), V3(-self.d.x, -self.d.y, 0), V3(0, 0, 1))

    def R(self):
        return self.M(0, 0, 0).to_3x3()


def panelise(b, face, frame=0.04, depth=0.02, plate=0.025, raise_=0.01, inner='PanelLight', recess='PanelGrey',
              bolts=None, frame_mat=None):
    """Frame + recess + raised plate. Inner plate ends at -depth + raise_ from the face.
    If `bolts` (a Builder) is given, adds a bolt in each corner of the frame."""
    if bolts is not None and len(face.verts) == 4:
        face.normal_update()
        n = face.normal.copy()
        vs = [v.co.copy() for v in face.verts]
        for i in range(4):
            c, nx, pv = vs[i], vs[(i + 1) % 4], vs[i - 1]
            p = c + (nx - c).normalized() * (frame * 0.5) + (pv - c).normalized() * (frame * 0.5)
            bolts.cyl(p - n * 0.002, p + n * 0.006, 0.0075, 'Steel', seg=8)
    if frame_mat is not None:
        face.material_index = b.mi(frame_mat)
    b.inset(face, frame, 0.0)
    b.inset(face, 0.006, -depth, rim_mat=recess)
    if plate > 0:
        b.inset(face, plate, raise_, rim_mat=inner, inner_mat=inner)
    else:
        face.material_index = b.mi(inner)
    return face


# ---------------------------------------------------------------------------
# Wall bay details
# ---------------------------------------------------------------------------


def detail_box(pc, bay, rng, conduit_top=2.33):
    s = bay.L * 0.5
    z = 1.78
    R = bay.R()
    pc.fine.obox(bay.p(s, -0.01 + 0.06, z), (0.34, 0.44, 0.12), 'PanelGrey', rot=R)
    pc.fine.obox(bay.p(s, 0.115, z + 0.02), (0.28, 0.32, 0.012), 'Gunmetal', rot=R)
    for dx in (-0.1, 0.1):
        pc.fine.cyl(bay.p(s + dx, 0.05, z + 0.22), bay.p(s + dx, 0.05, conduit_top), 0.02, 'DarkSteel', seg=10)
    led = 'LightGreen' if rng.random() < 0.6 else 'LightAmber'
    pc.flat.obox(bay.p(s - 0.1, 0.123, z - 0.16), (0.04, 0.025, 0.004), led, rot=R)
    pc.flat.obox(bay.p(s - 0.04, 0.123, z - 0.16), (0.04, 0.025, 0.004), 'LightGreen', rot=R)
    text(pc.flat, 'J-%02d' % rng.randint(1, 40), 0.045, bay.M(s + 0.06, 0.1225, z - 0.16), 'Paint')


def detail_hatch(pc, bay, rng):
    s = bay.L * 0.5
    z = 1.8
    R = bay.R()
    pc.fine.obox(bay.p(s, -0.01 + 0.012, z), (0.52, 0.5, 0.024), 'PanelGrey', rot=R)
    pc.fine.obox(bay.p(s, 0.016, z), (0.44, 0.42, 0.012), 'PanelLight', rot=R)
    for dx in (-0.22, 0.22):
        for dz in (-0.21, 0.21):
            pc.fine.cyl(bay.p(s + dx, 0.0, z + dz), bay.p(s + dx, 0.03, z + dz), 0.012, 'Steel', seg=8)
    # handle
    pts = [bay.p(s - 0.1, 0.02, z - 0.12), bay.p(s - 0.1, 0.06, z - 0.12),
           bay.p(s + 0.1, 0.06, z - 0.12), bay.p(s + 0.1, 0.02, z - 0.12)]
    pc.fine.tube(pts, 0.011, 'Gunmetal', seg=8)
    text(pc.flat, 'ACCESS %d' % rng.randint(10, 99), 0.05, bay.M(s, 0.0235, z + 0.12), 'PaintDark')


def detail_sign(pc, bay, rng):
    s = bay.L * 0.5
    label = rng.choice(SIGNS)
    # sits on the raised inner plate of the upper panel (o = -0.01)
    text(pc.flat, label, 0.11, bay.M(s, -0.008, 1.95), 'PaintDark')
    pc.flat.obox(bay.p(s, -0.008, 1.84), (min(0.7, bay.L - 0.3), 0.03, 0.002), 'AccentOrange', rot=bay.R())


def detail_screen(pc, bay, rng):
    s = bay.L * 0.5
    z = 1.62
    R = bay.R()
    pc.fine.obox(bay.p(s, 0.045, z), (0.56, 0.42, 0.11), 'Gunmetal', rot=R)
    pc.fine.obox(bay.p(s, 0.1, z + 0.02), (0.48, 0.3, 0.012), 'Recess', rot=R)
    pc.flat.obox(bay.p(s, 0.1085, z + 0.02), (0.46, 0.28, 0.002), 'ScreenDim', rot=R)
    # UI: bars and text glowing on the screen
    for i, w in enumerate((0.3, 0.18, 0.24, 0.12)):
        pc.flat.obox(bay.p(s + 0.19 - w / 2, 0.1105, z + 0.1 - i * 0.05), (w, 0.018, 0.001), 'Screen', rot=R)
    text(pc.flat, 'SYS OK', 0.045, bay.M(s - 0.13, 0.111, z - 0.08), 'LightGreen')
    for k in range(4):
        pc.fine.obox(bay.p(s - 0.18 + k * 0.12, 0.105, z - 0.17), (0.07, 0.035, 0.02), 'PanelLight', rot=R)


def detail_extinguisher(pc, bay, rng):
    s = bay.L * 0.5
    R = bay.R()
    o = 0.1
    pc.fine.obox(bay.p(s, 0.012, 0.86), (0.2, 0.62, 0.024), 'PanelGrey', rot=R)
    pc.fine.cyl(bay.p(s, o, 0.6), bay.p(s, o, 1.02), 0.075, 'PipeRed', seg=20)
    pc.fine.cyl(bay.p(s, o, 1.02), bay.p(s, o, 1.08), 0.03, 'Steel', seg=12)
    pc.fine.obox(bay.p(s, o, 1.1), (0.1, 0.03, 0.03), 'HazardBlack', rot=R)
    pc.fine.tube([bay.p(s + 0.03, o, 1.08), bay.p(s + 0.1, o + 0.03, 1.02),
                  bay.p(s + 0.1, o + 0.07, 0.8), bay.p(s + 0.06, o + 0.08, 0.7)], 0.012, 'Rubber', seg=8)
    for z in (0.7, 0.95):
        pc.fine.obox(bay.p(s, o - 0.02, z), (0.17, 0.03, 0.12), 'Gunmetal', rot=R)
    text(pc.flat, 'FIRE', 0.06, bay.M(s, 0.012 + 0.0125, 1.12), 'PaintYellow')


def detail_cables(pc, bay, rng):
    cols = ['CableBlack', 'CableRed', 'CableBlue', 'CableYellow', 'CableBlack']
    rng.shuffle(cols)
    s0, s1 = 0.12, bay.L - 0.12
    for c in range(3):
        sag = 0.12 + 0.05 * c + rng.random() * 0.04
        o = 0.05 + c * 0.028
        zb = 2.2 - c * 0.012
        pts = []
        for i in range(13):
            t = i / 12
            s = s0 + (s1 - s0) * t
            pts.append(bay.p(s, o, zb - sag * 4 * t * (1 - t)))
        pc.fine.tube(pts, 0.014, cols[c], seg=8)
    for s in (s0, s1):
        pc.fine.obox(bay.p(s, 0.06, 2.2), (0.06, 0.08, 0.12), 'Gunmetal', rot=bay.R())


DETAILS = [(detail_box, 12), (detail_hatch, 10), (detail_sign, 12), (detail_screen, 8),
           (detail_cables, 12), (detail_extinguisher, 5), (None, 41)]


def pick_detail(rng):
    total = sum(w for _, w in DETAILS)
    x = rng.random() * total
    for fn, w in DETAILS:
        x -= w
        if x <= 0:
            return fn
    return None


# ---------------------------------------------------------------------------
# Walls
# ---------------------------------------------------------------------------


def wall_run(pc, corners, profile=WALL, wall_mats=WALL_MATS, k_low=K_CHLOW, k_wlow=K_WLOW,
             k_wup=K_WUP, k_chup=K_CHUP, details=True, pipes=PIPES, bay_len=1.0, skip_detail=None):
    """Sweeps a wall along a polyline and dresses every bay."""
    rng = pc.rng
    path = subdivide([V2(*p) for p in corners], bay_len)
    faces = pc.main.sweep(path, profile, lambda i, k: wall_mats.get(k, 'PanelWhite'))
    ms = miters(path)
    n = len(path)

    for i in range(n - 1):
        bay = Bay(path[i], path[i + 1])
        straight = ((ms[i] - bay.n).length < 1e-6) and ((ms[i + 1] - bay.n).length < 1e-6)
        alt = (i % 2 == 0)
        bolts = pc.fine if straight else None
        panelise(pc.main, faces[i][k_wlow], inner='PanelGrey' if alt else 'PanelDark', recess='PanelDark',
                 frame_mat='PanelLight', bolts=bolts)
        panelise(pc.main, faces[i][k_wup], inner='PanelWhite' if alt else 'PanelLight', bolts=bolts)
        panelise(pc.main, faces[i][k_chup], frame=0.035, depth=0.012, plate=0.0, inner='PanelGrey')
        low = faces[i][k_low]
        if straight and alt and bay.L > 0.6:
            # louvred vent in the lower chamfer
            pc.main.inset(low, 0.05, 0.0)
            pc.main.inset(low, 0.005, -0.05, rim_mat='DarkSteel', inner_mat='Recess')
            o0, z0 = profile[k_low]
            o1, z1 = profile[k_low + 1]
            for j in range(5):
                t = 0.17 + j * 0.165
                o = o0 + (o1 - o0) * t - 0.03
                z = z0 + (z1 - z0) * t
                rot = bay.R() @ Matrix.Rotation(math.radians(-35), 3, 'X')
                pc.fine.obox(bay.p(bay.L / 2, o, z), (bay.L - 0.14, 0.012, 0.07), 'DarkSteel', rot=rot)
        else:
            panelise(pc.main, low, frame=0.04, depth=0.012, plate=0.0, inner='PanelDark')
        if details and straight and bay.L >= 0.75 and not (skip_detail and skip_detail(bay)):
            fn = pick_detail(rng)
            if fn:
                fn(pc, bay, rng)

    # floor glow strip in front of the lower chamfer
    pc.flat.sweep(path, [(0.335, 0.003), (0.305, 0.003)], 'LightTeal')

    # pipes along the upper chamfer, with brackets at straight bay joints
    if pipes:
        corner_pts = [V2(*p) for p in corners]
        for o, z, r, m in pipes:
            pc.round.sweep(corner_pts, circle(o, z, r), m, closed_profile=True)
        for i in range(1, n - 1):
            if (ms[i] - perp((path[i + 1] - path[i]).normalized())).length > 1e-6:
                continue
            bay = Bay(path[i], path[i + 1])
            R = bay.R()
            mid = V2(*_on_chamfer(0.5, 0.012))
            rot = R @ Matrix.Rotation(math.atan2(0.6, 0.8), 3, 'X')
            pc.fine.obox(bay.p(0, mid.x, mid.y), (0.06, 0.6, 0.024), 'Gunmetal', rot=rot)
            for o, z, r, m in pipes[:2]:
                pc.fine.cyl(bay.p(-0.025, o, z), bay.p(0.025, o, z), r + 0.012, 'DarkSteel', seg=16)
    return path


# ---------------------------------------------------------------------------
# Floor, ceiling, ribs
# ---------------------------------------------------------------------------

_XS = [-E, -0.5, 0.5, E]
_ARM_TILE = {'N': (1, 2), 'S': (1, 0), 'E': (2, 1), 'W': (0, 1)}
TROUGH = -0.32


def floor(pc, open_sides):
    grating = {(1, 1)} | {_ARM_TILE[s] for s in open_sides}
    up = V3(0, 0, 1)
    for ix in range(3):
        for iy in range(3):
            x0, x1, y0, y1 = _XS[ix], _XS[ix + 1], _XS[iy], _XS[iy + 1]
            if (ix, iy) not in grating:
                f = pc.main.face([V3(x0, y0, 0), V3(x1, y0, 0), V3(x1, y1, 0), V3(x0, y1, 0)], 'DarkSteel', toward=up)
                pc.main.inset(f, 0.03, 0.0)
                pc.main.inset(f, 0.004, -0.006, rim_mat='Recess')
                continue
            # trough
            pc.main.face([V3(x0, y0, TROUGH), V3(x1, y0, TROUGH), V3(x1, y1, TROUGH), V3(x0, y1, TROUGH)],
                         'Recess', toward=up)
            along_y = ix == 1  # bars run across the direction of travel
            for (nx, ny), edge, into in (((ix - 1, iy), ((x0, y0), (x0, y1)), V3(1, 0, 0)),
                                         ((ix + 1, iy), ((x1, y0), (x1, y1)), V3(-1, 0, 0)),
                                         ((ix, iy - 1), ((x0, y0), (x1, y0)), V3(0, 1, 0)),
                                         ((ix, iy + 1), ((x0, y1), (x1, y1)), V3(0, -1, 0))):
                if not (0 <= nx < 3 and 0 <= ny < 3) or (nx, ny) in grating:
                    continue
                (ax, ay), (bx, by) = edge
                pc.main.face([V3(ax, ay, 0), V3(bx, by, 0), V3(bx, by, TROUGH), V3(ax, ay, TROUGH)],
                             'DarkSteel', toward=into)
                # rail supporting the bars
                cx, cy = (ax + bx) / 2 + into.x * 0.02, (ay + by) / 2 + into.y * 0.02
                lx, ly = abs(bx - ax) or 0.04, abs(by - ay) or 0.04
                pc.fine.box((cx - lx / 2, cy - ly / 2, -0.06), (cx + lx / 2, cy + ly / 2, -0.004), 'Gunmetal')
                # underglow strip on the trough floor along that wall
                gx, gy = (ax + bx) / 2 + into.x * 0.07, (ay + by) / 2 + into.y * 0.07
                gl = (abs(bx - ax) or 0.025, abs(by - ay) or 0.025)
                pc.flat.box((gx - gl[0] / 2, gy - gl[1] / 2, TROUGH), (gx + gl[0] / 2, gy + gl[1] / 2, TROUGH + 0.004),
                            'LightTeal')
            # bars
            pitch, bw = 0.07, 0.022
            if along_y:
                cnt = int(round((y1 - y0) / pitch))
                for j in range(cnt):
                    yc = y0 + (j + 0.5) * (y1 - y0) / cnt
                    pc.fine.box((x0 + 0.01, yc - bw / 2, -0.045), (x1 - 0.01, yc + bw / 2, -0.004), 'DarkSteel')
            else:
                cnt = int(round((x1 - x0) / pitch))
                for j in range(cnt):
                    xc = x0 + (j + 0.5) * (x1 - x0) / cnt
                    pc.fine.box((xc - bw / 2, y0 + 0.01, -0.045), (xc + bw / 2, y1 - 0.01, -0.004), 'DarkSteel')
            # longitudinal stringers under the bars
            if along_y:
                for xs in (-0.17, 0.17):
                    pc.fine.box((xs - 0.012, y0, -0.09), (xs + 0.012, y1, -0.045), 'Gunmetal')
            else:
                for ys in (-0.17, 0.17):
                    pc.fine.box((x0, ys - 0.012, -0.09), (x1, ys + 0.012, -0.045), 'Gunmetal')

    # under-floor manifold and pipes to every open edge
    pc.fine.box((-0.26, -0.26, TROUGH), (0.26, 0.26, TROUGH + 0.15), 'Gunmetal')
    pc.flat.box((-0.06, -0.262, TROUGH + 0.09), (0.06, -0.258, TROUGH + 0.11), 'LightAmber')
    for s in open_sides:
        R = side_rotation(s).to_3x3()
        for off, r, m, z in ((-0.15, 0.045, 'PipeBlue', TROUGH + 0.07), (0.15, 0.045, 'PipeBlue', TROUGH + 0.07),
                             (0.0, 0.025, 'Copper', TROUGH + 0.05)):
            p0 = R @ V3(off, 0.26, z)
            p1 = R @ V3(off, E, z)
            pc.round.cyl(p0, p1, r, m, seg=12, caps=(False, False))


HOLE = (-0.62, 0.82, 0.62, 1.72)   # broken ceiling: the open hole (x0, y0, x1, y1)
CAVITY = 0.48                      # how deep the space above the ceiling is


def broken_ceiling(pc):
    """The north ceiling tile of a straight corridor torn open: a cavity with ducts above,
    one panel hanging from its hinge, the light fixture dangling on its cables, and loose
    cables with bare ends (spark markers)."""
    down = V3(0, 0, -1)
    a = 0.75
    x0, y0, x1, y1 = HOLE
    # what is left of the tile around the hole
    for sx0, sy0, sx1, sy1 in ((-a, a, a, y0), (-a, y1, a, E), (-a, y0, x0, y1), (x1, y0, a, y1)):
        pc.main.face([V3(sx0, sy0, CEIL), V3(sx1, sy0, CEIL), V3(sx1, sy1, CEIL), V3(sx0, sy1, CEIL)], 'PanelLight',
                     toward=down)
    # torn edge: the panel's thickness shows round the hole
    t = 0.03
    for p0, p1, inward in (((x0, y0), (x1, y0), V3(0, 1, 0)), ((x1, y0), (x1, y1), V3(-1, 0, 0)),
                           ((x1, y1), (x0, y1), V3(0, -1, 0)), ((x0, y1), (x0, y0), V3(1, 0, 0))):
        pc.main.face([V3(*p0, CEIL), V3(*p1, CEIL), V3(*p1, CEIL + t), V3(*p0, CEIL + t)], 'PanelGrey', toward=inward)
    # the cavity: dark walls and a lid, seen from below
    zt = CEIL + CAVITY
    for p0, p1, inward in (((x0, y0), (x1, y0), V3(0, 1, 0)), ((x1, y0), (x1, y1), V3(-1, 0, 0)),
                           ((x1, y1), (x0, y1), V3(0, -1, 0)), ((x0, y1), (x0, y0), V3(1, 0, 0))):
        pc.main.face([V3(*p0, CEIL + t), V3(*p1, CEIL + t), V3(*p1, zt), V3(*p0, zt)], 'Recess', toward=inward)
    pc.main.face([V3(x0, y0, zt), V3(x1, y0, zt), V3(x1, y1, zt), V3(x0, y1, zt)], 'Recess', toward=down)
    # ducts and a beam running through it
    pc.round.cyl(V3(-0.3, y0, CEIL + 0.27), V3(-0.3, y1, CEIL + 0.27), 0.11, 'DarkSteel', seg=20, caps=(False, False))
    pc.round.cyl(V3(0.22, y0, CEIL + 0.33), V3(0.22, y1, CEIL + 0.33), 0.065, 'Copper', seg=16, caps=(False, False))
    pc.round.cyl(V3(0.42, y0, CEIL + 0.2), V3(0.42, y1, CEIL + 0.2), 0.035, 'PipeRed', seg=12, caps=(False, False))
    pc.fine.box((x0, 1.22, CEIL + 0.4), (x1, 1.3, zt), 'Gunmetal')
    # one panel hanging down from its hinge on the south edge, a little twisted
    from mathutils import Matrix as _M
    hinge = V3(-0.02, y0 + 0.02, CEIL - 0.012)
    R = _M.Rotation(math.radians(-62), 3, 'X') @ _M.Rotation(math.radians(9), 3, 'Y')
    c = hinge + R @ V3(0, 0.42, 0)
    # the panel's underside (what you see) is a proper ceiling panel: framed, recessed
    # middle, a stencil code, a soot burn and a torn corner; its back is bare grey metal
    pf = pc.main.obox(c, (0.58, 0.84, 0.024), 'PanelGrey', rot=R, mats={'+z': 'DarkSteel'})
    under = pf['-z']
    pc.main.inset(under, 0.04, 0.0, inner_mat='PanelGrey')
    pc.main.inset(under, 0.012, 0.008, rim_mat='Gunmetal', inner_mat='PanelDark')
    n_under = R @ V3(0, 0, -1)
    along = R @ V3(0, 1, 0)
    across = R @ V3(1, 0, 0)
    face_c = c + n_under * 0.0125
    text(pc.flat, 'C-14', 0.06, frame_matrix(face_c + n_under * 0.0015 + along * 0.22, -across, along), 'PaintDark')
    # a scorch where the fixture shorted, and rivets round the frame
    burn = [face_c + n_under * 0.0016 + across * (0.12 * math.cos(a)) + along * (-0.12 + 0.09 * math.sin(a))
            for a in [k * 2 * math.pi / 12 for k in range(12)]]
    pc.flat.face(burn, 'Soot', toward=n_under)
    for sx in (-0.25, 0.25):
        for sy in (-0.38, 0.0, 0.38):
            pc.fine.cyl(face_c + across * sx + along * sy, face_c + across * sx + along * sy + n_under * 0.006, 0.008,
                        'Steel', seg=8)
    # the light fixture, torn loose, hanging at an angle on two cables
    top, bot = V3(0.36, 1.58, 2.62), V3(0.43, 0.98, 2.05)
    axis = (bot - top)
    L = axis.length
    # twisted on its cables so the lit face turns toward the walkway (west, a little down)
    yax = axis.normalized()
    want = V3(-0.85, -0.15, -0.5)
    lit_n = (want - yax * want.dot(yax)).normalized()
    zax = -lit_n
    Rf = _M((yax.cross(zax).normalized(), yax, zax)).transposed()
    fc = (top + bot) * 0.5
    faces = pc.fine.obox(fc, (0.34, L, 0.075), 'Gunmetal', rot=Rf)
    lit = faces['-z']
    pc.fine.inset(lit, 0.03, 0.0)
    pc.fine.inset(lit, 0.004, 0.01, rim_mat='DarkSteel', inner_mat='LightWhite')
    # weaker than a mounted fixture: it hangs right next to the dropped panel
    pc.light((fc.x - 0.05, fc.y - 0.2, fc.z - 0.25), power=60.0, radius=0.15)
    for end in (top + Rf @ V3(0.12, 0, 0.03), bot + Rf @ V3(-0.12, 0, 0.03)):
        anchor = V3(end.x * 0.6, (end.y + 1.27) * 0.5, CEIL + 0.3)
        mid = (anchor + end) * 0.5 + V3(0.03, 0, -0.06)
        pc.round.tube([anchor, mid, end], 0.008, 'CableBlack', seg=6)
    pc.marker('spark0', bot + V3(0, -0.04, -0.03), (0, -0.3, -1))
    # loose cables: two sagging loops and three bare ends hanging down
    rng = pc.rng
    for k, (xa, xb, low, mat) in enumerate(((-0.5, -0.1, 2.25, 'CableRed'), (-0.45, 0.05, 2.32, 'CableYellow'))):
        ya, yb = 0.95 + 0.25 * k, 1.55 - 0.15 * k
        pts = []
        for i in range(9):
            s = i / 8
            x = xa + (xb - xa) * s
            y = ya + (yb - ya) * s
            z = CEIL + 0.15 - (CEIL + 0.15 - low) * math.sin(math.pi * s)
            pts.append(V3(x, y, z))
        pc.round.tube(pts, 0.011, mat, seg=8)
    for k, (x, y, z_end, mat) in enumerate(((-0.25, 1.1, 2.02, 'CableBlue'), (-0.05, 1.42, 1.96, 'CableBlack'),
                                            (-0.38, 1.6, 2.18, 'CableRed'))):
        pts = [V3(x, y, CEIL + 0.2)]
        for i in range(1, 6):
            s = i / 5
            pts.append(V3(x + 0.03 * math.sin(s * 3 + k) + rng.uniform(-0.01, 0.01), y + 0.04 * s,
                          CEIL + 0.2 - (CEIL + 0.2 - z_end) * s))
        pc.round.tube(pts, 0.012, mat, seg=8)
        tip = pts[-1]
        pc.fine.cyl(tip, tip + V3(0, 0, -0.035), 0.006, 'Copper', seg=8)
        pc.marker(f'spark{k + 1}', tip + V3(0, 0, -0.04), (0, 0, -1))


def ceiling(pc, open_sides, kind, broken=False):
    down = V3(0, 0, -1)
    a = 0.75
    tiles = [(-a, -a, a, a)]
    for s in open_sides:
        d = SIDE_DIR[s]
        if d.y > 0:
            if broken:
                continue
            tiles.append((-a, a, a, E))
        elif d.y < 0:
            tiles.append((-a, -E, a, -a))
        elif d.x > 0:
            tiles.append((a, -a, E, a))
        else:
            tiles.append((-E, -a, -a, a))
    for x0, y0, x1, y1 in tiles:
        f = pc.main.face([V3(x0, y0, CEIL), V3(x1, y0, CEIL), V3(x1, y1, CEIL), V3(x0, y1, CEIL)], 'PanelLight',
                         toward=down)
        panelise(pc.main, f, frame=0.04, depth=0.03, plate=0.0, inner='PanelGrey')

    def fixture(cx, cy, sx, sy):
        z0 = CEIL - 0.075
        faces = pc.fine.box((cx - sx / 2, cy - sy / 2, z0), (cx + sx / 2, cy + sy / 2, CEIL + 0.01), 'Gunmetal')
        pc.fine.inset(faces['-z'], 0.035, 0.0)
        pc.fine.inset(faces['-z'], 0.004, 0.012, rim_mat='DarkSteel', inner_mat='LightWhite')
        pc.light((cx, cy, CEIL - 0.3), power=160.0, radius=0.18)

    if kind == 'straight':
        if broken:
            broken_ceiling(pc)
        else:
            fixture(0, 1.0, 0.34, 1.1)
        fixture(0, -1.0, 0.34, 1.1)
    else:
        fixture(0, 0, 0.62, 0.62)


def ribs(pc, open_sides, kind):
    def rib(M, y0, y1, faces_both):
        faces = pc.main.band(OUTLINE, 0.09, y0, y1, 'PanelDark', M=M, front=faces_both, back=True,
                             inner_mat='Gunmetal')
        per = 3 if faces_both else 2
        for k in (1, 5):  # vertical sides of the outline
            back = faces[k * per + (1 if faces_both else 0)]
            pc.main.inset(back, 0.02, -0.008, inner_mat='Gunmetal')
        # running lights on the rib, both sides of the walkway
        for x in (-W + 0.045, W - 0.045):
            for y, fwd in ((y0 - 0.002, -1),) + (((y1 + 0.002, 1),) if faces_both else ()):
                c = M @ V3(x, y, 0.9)
                pc.flat.obox(c, (0.03, 0.004, 0.2), 'LightTeal', rot=M.to_3x3())

    for s in open_sides:
        M = side_rotation(s)
        rib(M, E - 0.1, E, False)
    if kind == 'straight':
        rib(Matrix.Identity(4), -0.1, 0.1, True)


def end_feature(pc):
    """Sealed emergency hatch on the dead-end wall."""
    bay = Bay(V2(-W, -W), V2(W, -W))
    R = bay.R()
    s = bay.L / 2
    pc.fine.obox(bay.p(s, 0.03, 1.2), (1.5, 1.9, 0.06), 'PanelGrey', rot=R)
    # hazard stripes border (diagonal blocks)
    for j in range(14):
        x = -0.7 + j * 0.1
        m = 'HazardYellow' if j % 2 == 0 else 'HazardBlack'
        for z in (0.32, 2.08):
            rot = R @ Matrix.Rotation(math.radians(35), 3, 'Z')
            pc.fine.obox(bay.p(s + x + 0.05, 0.065, z), (0.07, 0.12, 0.012), m, rot=rot)
    pc.fine.obox(bay.p(s, 0.075, 1.2), (1.1, 1.5, 0.04), 'Gunmetal', rot=R)
    pc.fine.cyl(bay.p(s, 0.09, 1.25), bay.p(s, 0.16, 1.25), 0.22, 'DarkSteel', seg=24)
    for k in range(6):
        a = k * math.pi / 3
        pc.fine.obox(bay.p(s + math.cos(a) * 0.17, 0.17, 1.25 + math.sin(a) * 0.17), (0.05, 0.05, 0.03), 'Steel',
                     rot=R)
    pc.fine.obox(bay.p(s, 0.19, 1.25), (0.5, 0.05, 0.03), 'Steel', rot=R)
    text(pc.flat, 'SEALED', 0.12, bay.M(s, 0.0955, 1.78), 'PaintYellow')
    pc.flat.obox(bay.p(s - 0.42, 0.0955, 1.78), (0.06, 0.06, 0.002), 'LightRed', rot=R)
    pc.flat.obox(bay.p(s + 0.42, 0.0955, 1.78), (0.06, 0.06, 0.002), 'LightRed', rot=R)


def build_corridor(kind, parent, broken=False):
    """broken (straight only): the derelict variant corridor_straight_broken, with the
    north ceiling tile torn open (see broken_ceiling)."""
    open_sides, polys = TYPES[kind]
    pc = Piece('corridor_' + kind + ('_broken' if broken else ''), seed='corridor_' + kind)
    # keep the dead-end wall clear for the sealed hatch
    skip = (lambda bay: abs(bay.d.x) > 0.5) if kind == 'end' else None
    for poly in polys:
        wall_run(pc, poly, skip_detail=skip)
    floor(pc, open_sides)
    ceiling(pc, open_sides, kind, broken)
    ribs(pc, open_sides, kind)
    if kind == 'end':
        end_feature(pc)
    return pc.finish(parent)
