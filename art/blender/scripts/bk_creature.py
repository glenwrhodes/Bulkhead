"""Bulkhead kit: the creature (round 8). Our own design, biomechanical:

  A long, low body (2.4 m nose to tail hook) of rigid graphite-black armour plates on
  a ribbed bone frame. Six limbs: four spindly walking legs that rise to high knees and
  end in hooked claws that grip panels, and two long forelimbs held folded under the
  head like a mantis, each ending in a scythe hook. The head is an eyeless, flat wedge of
  overlapping plates; its snout is four petal jaws (top, bottom, left, right) lined with
  needle teeth, which split open in a cross. An exposed spine ridge of bone fins runs from
  the neck to the tail; tendons like armoured cable conduit run along the flanks. A
  faint amber glow lives in the throat and in the seams between plates.

Rigid segments (no skinning): every bone is one mesh object named creature_<bone>,
modelled in the rest pose. Markers tell the engine the rest pivots:
  creature_fx_bone_<bone>   at the joint, +Y along the bone
  creature_fx_foot_<leg>    at each claw tip (leg = ML, MR, HL, HR)
Blender axes: +Y is the creature's forward, +Z up, origin on the ground under the waist.

build_creature() makes the static kit piece (KIT_creature, exported) and
build_rig() makes CREATURE_RIG: an armature with the same parts parented to bones,
IK legs, and a looping crawl cycle, for the Blender renders.
"""

import math

import bpy
from mathutils import Matrix, Vector

from bk_core import V3, Builder, Piece, collection, clear_collection, material, sphere

N = 20  # ring resolution

# ---------------------------------------------------------------------------
# Skeleton (rest pose)
# ---------------------------------------------------------------------------

BODY = {  # bone: pivot
    'pelvis': V3(0, -0.30, 0.47),
    'waist': V3(0, 0.02, 0.47),
    'thorax': V3(0, 0.38, 0.49),
    'neck': V3(0, 0.66, 0.52),
    'head': V3(0, 0.76, 0.54),
}
TAIL_Y = [-0.58, -0.75, -0.92, -1.08, -1.23, -1.37]
TAIL_Z = [0.47, 0.49, 0.52, 0.56, 0.60, 0.64]
LEGS = {  # leg: (hip, knee, foot)
    'MR': (V3(0.15, 0.30, 0.44), V3(0.43, 0.42, 0.86), V3(0.80, 0.62, 0.0)),
    'HR': (V3(0.16, -0.36, 0.43), V3(0.45, -0.47, 0.84), V3(0.80, -0.72, 0.0)),
}
LEGS['ML'] = tuple(V3(-p.x, p.y, p.z) for p in LEGS['MR'])
LEGS['HL'] = tuple(V3(-p.x, p.y, p.z) for p in LEGS['HR'])
ARMS = {  # side: (shoulder, elbow, wrist, hook tip)
    'R': (V3(0.11, 0.56, 0.48), V3(0.19, 0.82, 0.74), V3(0.16, 0.64, 0.36), V3(0.11, 1.06, 0.26)),
}
ARMS['L'] = tuple(V3(-p.x, p.y, p.z) for p in ARMS['R'])
JAW_BASE_Y = 1.06
JAW_TIP = V3(0, 1.27, 0.51)
JAWS = {'t': V3(0, 0, 1), 'b': V3(0, 0, -1), 'l': V3(-1, 0, 0), 'r': V3(1, 0, 0)}


def head_axis_z(y):
    """Height of the head's centre line (it slopes down to the snout)."""
    return 0.56 - (y - 0.76) * 0.14


# ---------------------------------------------------------------------------
# Geometry helpers
# ---------------------------------------------------------------------------

def sgnpow(x, e):
    return math.copysign(abs(x) ** e, x)


def ring(center, u, v, ru, rv, e=1.0, n=N, a0=0.0, a1=2 * math.pi, closed=True):
    pts = []
    count = n if closed else n + 1
    for i in range(count):
        a = a0 + (a1 - a0) * i / n
        pts.append(center + u * (ru * sgnpow(math.cos(a), e)) + v * (rv * sgnpow(math.sin(a), e)))
    return pts


def loft(b, rings, mat, caps=(True, True), mat_fn=None):
    """Quads between consecutive rings (same point count, closed loops), facing away
    from the rings' centre line; optional fan caps."""
    vs = [[b.bm.verts.new(p) for p in r] for r in rings]
    cs = [sum(r, Vector()) / len(r) for r in rings]
    n = len(rings[0])
    for i in range(len(rings) - 1):
        axis_c = (cs[i] + cs[i + 1]) * 0.5
        for k in range(n):
            k2 = (k + 1) % n
            m = mat_fn(i, k) if mat_fn else mat
            f = b.face_verts([vs[i][k], vs[i][k2], vs[i + 1][k2], vs[i + 1][k]], m)
            f.normal_update()
            mid = (vs[i][k].co + vs[i][k2].co + vs[i + 1][k].co + vs[i + 1][k2].co) * 0.25
            if f.normal.dot(mid - axis_c) < 0:
                f.normal_flip()
    for end, idx, nb in ((caps[0], 0, 1), (caps[1], len(rings) - 1, len(rings) - 2)):
        if not end:
            continue
        c = b.bm.verts.new(cs[idx])
        out = cs[idx] - cs[nb]
        for k in range(n):
            f = b.face_verts([c, vs[idx][k], vs[idx][(k + 1) % n]], mat)
            f.normal_update()
            if f.normal.dot(out) < 0:
                f.normal_flip()


def tube_loft(b, a, c, profile, mat, e=1.0, up=V3(0, 0, 1), n=14, caps=(True, True), flat=1.0):
    """A limb segment from a to c; profile = [(t, r)], cross-section flattened by `flat`
    along the side axis."""
    axis = (c - a)
    d = axis.normalized()
    side = d.cross(up)
    if side.length < 1e-4:
        side = d.cross(V3(1, 0, 0))
    side.normalize()
    top = side.cross(d).normalized()
    rings = [ring(a + axis * t, side, top, r * flat, r, e=e, n=n) for t, r in profile]
    loft(b, rings, mat, caps=caps)
    return side, top


def shell(b, y0, y1, prof, z_of, rim_mat, mat, thick=0.012, a0=0.15, a1=math.pi - 0.15, e=0.7, lift=0.0, n=14):
    """An armour shingle over the top of a body section between y0 and y1: a thick arc
    (outer and inner surface, rims). prof(y) -> (half width, half height) of the body
    under it; lift raises the plate off the body."""
    steps = 6
    rings = []
    for i in range(steps + 1):
        y = y0 + (y1 - y0) * i / steps
        w, h = prof(y)
        c = V3(0, y, z_of(y))
        outer = ring(c, V3(1, 0, 0), V3(0, 0, 1), w + thick + lift, h + thick + lift, e=e, n=n, a0=a0, a1=a1,
                     closed=False)
        inner = ring(c, V3(1, 0, 0), V3(0, 0, 1), w + lift * 0.3, h + lift * 0.3, e=e, n=n, a0=a0, a1=a1,
                     closed=False)
        rings.append(outer + list(reversed(inner)))
    m = len(rings[0])
    vs = [[b.bm.verts.new(p) for p in r] for r in rings]
    half = n + 1
    for i in range(steps):
        for k in range(m):
            k2 = (k + 1) % m
            is_rim = k == half - 1 or k == m - 1
            f = b.face_verts([vs[i][k], vs[i][k2], vs[i + 1][k2], vs[i + 1][k]], rim_mat if is_rim else mat)
            f.normal_update()
            # outer arc faces out, inner faces in, rims face sideways
            yy = y0 + (y1 - y0) * (i + 0.5) / steps
            c = V3(0, yy, z_of(yy))
            mid = (vs[i][k].co + vs[i + 1][k2].co) * 0.5
            want = (mid - c) if k < half - 1 else (c - mid)
            if is_rim:
                want = (mid - c)
            if f.normal.dot(want) < 0:
                f.normal_flip()
    for idx, out in ((0, V3(0, -1, 0)), (steps, V3(0, 1, 0))):
        f = b.face_verts(list(vs[idx]), rim_mat)
        f.normal_update()
        if f.normal.dot(out) < 0:
            f.normal_flip()


def curve_loft(b, pts, radii, mat, up, flat=1.0, e=0.75, n=12, caps=(True, True)):
    """A tapering limb along a curve: rings perpendicular to the curve's tangent."""
    rings = []
    for i, (p, r) in enumerate(zip(pts, radii)):
        a = pts[max(0, i - 1)]
        c = pts[min(len(pts) - 1, i + 1)]
        d = (c - a).normalized()
        side = d.cross(up)
        if side.length < 1e-4:
            side = d.cross(V3(1, 0, 0))
        side.normalize()
        top = side.cross(d).normalized()
        rings.append(ring(p, side, top, r * flat, r, e=e, n=n))
    loft(b, rings, mat, caps=caps)


def bezier(a, c, ctrl, steps):
    return [a * (1 - t) ** 2 + ctrl * (2 * t * (1 - t)) + c * t * t for t in [i / steps for i in range(steps + 1)]]


def spike(b, base, tip, r, mat, seg=8):
    b.cyl(base, tip, r, mat, seg=seg, caps=(True, False), r1=0.0005)


def hook(b, base, d, up, length, r, mat, curl=1.1, steps=8):
    """A curved claw/hook: a tapering tube bending toward `up` as it goes."""
    pts, radii = [], []
    p = Vector(base)
    dirv = Vector(d).normalized()
    upv = Vector(up).normalized()
    for i in range(steps + 1):
        t = i / steps
        pts.append(p.copy())
        radii.append(max(r * (1.0 - t) ** 0.8, 0.0012))
        ang = curl * t
        cur = (dirv * math.cos(ang) + upv * math.sin(ang)).normalized()
        p = p + cur * (length / steps)
    for i in range(steps):
        b.cyl(pts[i], pts[i + 1], radii[i], mat, seg=8, caps=(i == 0, False), r1=radii[i + 1])
    sphere(b, pts[-1], radii[-1], mat, seg=6, rings=3)
    return pts[-1]


# ---------------------------------------------------------------------------
# Body parts
# ---------------------------------------------------------------------------

def body_prof(points):
    """Interpolated (half width, half height) along y from [(y, w, h)]."""
    def f(y):
        if y <= points[0][0]:
            return points[0][1], points[0][2]
        for (ya, wa, ha), (yb, wb, hb) in zip(points, points[1:]):
            if ya <= y <= yb:
                t = (y - ya) / (yb - ya)
                t = t * t * (3 - 2 * t)
                return wa + (wb - wa) * t, ha + (hb - ha) * t
        return points[-1][1], points[-1][2]
    return f


def body_segment(b, glow, prof_pts, z_of, plates, mat='Chitin', e=0.75, seam_sides=True):
    """A lofted body section plus overlapping top plates with glowing seams."""
    prof = body_prof(prof_pts)
    ys = [prof_pts[0][0] + (prof_pts[-1][0] - prof_pts[0][0]) * i / 14 for i in range(15)]
    rings = []
    for y in ys:
        w, h = prof(y)
        rings.append(ring(V3(0, y, z_of(y)), V3(1, 0, 0), V3(0, 0, 1), w, h, e=e))
    loft(b, rings, 'ChitinMatte')
    # shingles: each starts a little in front of the previous one's end, raised slightly
    for k, (y0, y1) in enumerate(plates):
        shell(b, y0, y1, prof, z_of, 'Bone', mat, thick=0.014, lift=0.006 + 0.004 * (k % 2))
        # the glowing seam under the front edge of each plate
        w, h = prof(y1)
        seam = ring(V3(0, y1 - 0.004, z_of(y1)), V3(1, 0, 0), V3(0, 0, 1), w + 0.004, h + 0.004, e=e, n=14,
                    a0=0.35, a1=math.pi - 0.35, closed=False)
        seam_in = [p + V3(0, -0.012, 0) for p in seam]
        for i in range(len(seam) - 1):
            glow.face([seam[i], seam[i + 1], seam_in[i + 1], seam_in[i]], 'CreatureGlow', toward=seam[i] - V3(0, seam[i].y, z_of(y1)))
    # exposed ribs curving down each flank between the plates and the belly
    for y in ys[2:-2:2]:
        w, h = prof(y)
        for a0, a1 in ((-1.05, 0.3), (math.pi - 0.3, math.pi + 1.05)):
            pts = ring(V3(0, y, z_of(y)), V3(1, 0, 0), V3(0, 0, 1), w * 1.06, h * 1.06, e=e, n=8, a0=a0, a1=a1,
                       closed=False)
            b.tube(pts, 0.0075, 'Bone', seg=6)
    if seam_sides:
        # side slits: a glowing seam along each flank below the plates
        for s in (-1, 1):
            pts = []
            for y in ys[2:-2]:
                w, h = prof(y)
                pts.append(V3(s * (w * 0.98), y, z_of(y) - h * 0.15))
            for i in range(len(pts) - 1):
                a, c = pts[i], pts[i + 1]
                dz = V3(0, 0, 0.006)
                glow.face([a - dz, c - dz, c + dz, a + dz], 'CreatureGlow', toward=V3(s, 0, 0))
    return prof


def spine_ridge(b, ys, z_top, h0, h1):
    """Bone fins along the top, raked back."""
    for i, y in enumerate(ys):
        t = i / max(1, len(ys) - 1)
        h = h0 + (h1 - h0) * math.sin(math.pi * t)
        base = V3(0, y, z_top(y) - 0.005)
        tip = base + V3(0, -0.035, h)
        # a flattened fin: two triangles both sides plus a back
        w = 0.009
        pa, pb = base + V3(-w, 0.02, 0), base + V3(w, 0.02, 0)
        pc_, pd = base + V3(-w, -0.03, 0), base + V3(w, -0.03, 0)
        b.face([pa, pc_, tip], 'Bone', toward=V3(-1, 0, 0))
        b.face([pb, pd, tip], 'Bone', toward=V3(1, 0, 0))
        b.face([pa, pb, tip], 'Bone', toward=V3(0, 1, 0.3))
        b.face([pc_, pd, tip], 'Bone', toward=V3(0, -1, 0.3))


def plated_limb(b, pts, radii, up, n_plates=3, flat=0.78, tendons=2, spines=0):
    """A heavy biomechanical limb along a curve: a sinew core, overlapping armour
    sleeves that flare at their lower ends with bone lips and a glowing gap, tendon
    conduits clamped along the outside, and optional spines down its back."""
    pts = [Vector(p) for p in pts]
    n = len(pts) - 1
    curve_loft(b, pts, [r * 0.82 for r in radii], 'Sinew', up=up, flat=flat)
    for k in range(n_plates):
        i0 = int(round(k * n / n_plates))
        i1 = min(n, int(round((k + 1) * n / n_plates)) + 1)
        seg = pts[i0:i1 + 1]
        rr = [radii[min(i, len(radii) - 1)] * (1.0 + 0.32 * (j / max(1, len(seg) - 1)))
              for j, i in enumerate(range(i0, i1 + 1))]
        curve_loft(b, seg, rr, 'Chitin', up=up, flat=flat)
        d = (seg[-1] - seg[-2]).normalized()
        b.ring(seg[-1], d, rr[-1] * 0.98, 0.0055, 'Bone', seg=14, tseg=4)
        b.ring(seg[-1] + d * 0.012, d, rr[-1] * 0.78, 0.0045, 'CreatureGlow', seg=14, tseg=4)
    # tendons: bundles of armoured cable along the outside, clamped every few cm
    for t in range(tendons):
        ang = 2.0 + t * 1.1
        tp = []
        for i, p in enumerate(pts):
            a = pts[max(0, i - 1)]
            c = pts[min(n, i + 1)]
            d = (c - a).normalized()
            side = d.cross(up).normalized()
            top = side.cross(d).normalized()
            off = (side * math.cos(ang) * flat + top * math.sin(ang)) * radii[min(i, len(radii) - 1)] * 1.18
            tp.append(p + off)
        conduit(b, tp, radii[0] * 0.2, rings_every=0.06)
    for k in range(spines):
        i = 1 + int(k * (n - 1) / max(1, spines))
        p = pts[i]
        d = (pts[i + 1] - p).normalized()
        side = d.cross(up).normalized()
        top = side.cross(d).normalized()
        base = p + top * radii[i] * 1.25
        spike(b, base, base + top * (radii[i] * 0.9 + 0.012) - d * 0.03, radii[i] * 0.28, 'Bone', seg=6)


def dorsal_detail(b, prof, z_of, y0, y1, steps=10):
    """Giger-style surface over the back: paired dorsal conduits either side of the
    spine fins, transverse bone ribbing across the plates, and short flank pipes."""
    ys = [y0 + (y1 - y0) * i / steps for i in range(steps + 1)]
    for s in (-1, 1):
        for a_off, r in ((0.42, 0.011), (0.72, 0.008)):
            pts = []
            for y in ys:
                w, h = prof(y)
                a = math.pi / 2 - s * a_off
                pts.append(V3(math.cos(a) * (w + 0.046), y, z_of(y) + math.sin(a) * (h + 0.046)))
            conduit(b, pts, r, rings_every=0.05)
    # thin bone ribs arching over the back, between the conduits
    for y in ys[1:-1]:
        w, h = prof(y)
        arc = ring(V3(0, y, z_of(y)), V3(1, 0, 0), V3(0, 0, 1), w + 0.042, h + 0.042, e=0.75, n=10,
                   a0=0.55, a1=math.pi - 0.55, closed=False)
        b.tube(arc, 0.0045, 'Bone', seg=6)
    # a row of little bone nodules along each flank, like rivets grown into bone
    for s in (-1, 1):
        for y in ys[1:-1]:
            w, h = prof(y)
            sphere(b, V3(s * (w + 0.008), y, z_of(y) + h * 0.35), 0.008, 'Bone', seg=8, rings=4)


def conduit(b, pts, r, mat='Sinew', ring_mat='Bone', rings_every=0.07):
    """Tendon like armoured cable: a tube with bone clamp rings."""
    b.tube(pts, r, mat, seg=10)
    total = 0.0
    for a, c in zip(pts, pts[1:]):
        L = (c - a).length
        k = 0
        while k * rings_every < L:
            p = a + (c - a) * (k * rings_every / L)
            d = (c - a).normalized()
            b.cyl(p - d * 0.006, p + d * 0.006, r * 1.35, ring_mat, seg=10)
            k += 1
        total += L


# ---------------------------------------------------------------------------
# The piece
# ---------------------------------------------------------------------------

def build_parts():
    """Returns {bone: (Builder, bevel)} and the markers."""
    parts = {}
    markers = []

    def part(name, bevel=0.002):
        b = Builder()
        parts[name] = (b, bevel)
        return b

    # ---- pelvis (rear body) ----
    b = part('pelvis')
    z_p = lambda y: 0.47 + (y + 0.3) * 0.02  # noqa: E731
    prof = body_segment(b, b, [(-0.64, 0.05, 0.045), (-0.56, 0.12, 0.095), (-0.42, 0.165, 0.125),
                               (-0.27, 0.175, 0.13), (-0.14, 0.14, 0.115), (-0.07, 0.09, 0.085)], z_p,
                        [(-0.58, -0.42), (-0.45, -0.29), (-0.32, -0.16), (-0.19, -0.07)])
    spine_ridge(b, [-0.55 + 0.06 * i for i in range(8)], lambda y: z_p(y) + prof(y)[1] + 0.02, 0.03, 0.06)
    dorsal_detail(b, prof, z_p, -0.56, -0.1)
    for s in (-1, 1):  # flank conduits
        conduit(b, [V3(s * 0.15, -0.52, 0.47), V3(s * 0.185, -0.36, 0.49), V3(s * 0.17, -0.2, 0.49),
                    V3(s * 0.12, -0.08, 0.47)], 0.014)
        conduit(b, [V3(s * 0.12, -0.5, 0.405), V3(s * 0.16, -0.34, 0.39), V3(s * 0.13, -0.16, 0.405)], 0.011)
    # ribbed underside
    for i in range(6):
        y = -0.52 + i * 0.075
        w, h = prof(y)
        pts = ring(V3(0, y, z_p(y)), V3(1, 0, 0), V3(0, 0, 1), w * 0.9, h * 1.04, e=0.75, n=12,
                   a0=math.pi + 0.5, a1=2 * math.pi - 0.5, closed=False)
        b.tube(pts, 0.008, 'Bone', seg=6)
    markers.append(('bone_pelvis', BODY['pelvis'], V3(0, 1, 0)))

    # ---- waist ----
    b = part('waist')
    z_w = lambda y: 0.47  # noqa: E731
    body_segment(b, b, [(-0.12, 0.085, 0.08), (0.0, 0.07, 0.07), (0.1, 0.075, 0.07), (0.18, 0.09, 0.08)], z_w,
                 [(-0.1, 0.02), (0.02, 0.15)], seam_sides=False)
    for i in range(4):  # vertebra rings
        y = -0.08 + i * 0.07
        b.ring(V3(0, y, 0.47), V3(0, 1, 0), 0.08, 0.012, 'Bone', seg=16, tseg=6)
    for s in (-1, 1):
        conduit(b, [V3(s * 0.11, -0.1, 0.47), V3(s * 0.09, 0.03, 0.47), V3(s * 0.12, 0.17, 0.49)], 0.012)
    markers.append(('bone_waist', BODY['waist'], V3(0, 1, 0)))

    # ---- thorax ----
    b = part('thorax')
    z_t = lambda y: 0.49 + (y - 0.38) * 0.05  # noqa: E731
    prof = body_segment(b, b, [(0.13, 0.1, 0.09), (0.2, 0.15, 0.125), (0.33, 0.17, 0.145), (0.48, 0.155, 0.13),
                               (0.6, 0.11, 0.1), (0.67, 0.07, 0.07)], z_t,
                        [(0.14, 0.27), (0.25, 0.4), (0.37, 0.52), (0.49, 0.64)])
    spine_ridge(b, [0.16 + 0.065 * i for i in range(8)], lambda y: z_t(y) + prof(y)[1] + 0.02, 0.035, 0.075)
    dorsal_detail(b, prof, z_t, 0.16, 0.63)
    for s in (-1, 1):
        conduit(b, [V3(s * 0.11, 0.16, 0.48), V3(s * 0.17, 0.3, 0.5), V3(s * 0.16, 0.46, 0.51), V3(s * 0.09, 0.62, 0.52)],
                0.014)
        # shoulder and hip sockets (bone collars)
        b.ring(V3(s * 0.11, 0.56, 0.5), V3(s, 0.3, 0), 0.04, 0.011, 'Bone', seg=14, tseg=6)
        hip = LEGS['MR' if s > 0 else 'ML'][0]
        b.ring(hip, (LEGS['MR' if s > 0 else 'ML'][1] - hip).normalized(), 0.05, 0.012, 'Bone', seg=14, tseg=6)
    for i in range(6):
        y = 0.18 + i * 0.075
        w, h = prof(y)
        pts = ring(V3(0, y, z_t(y)), V3(1, 0, 0), V3(0, 0, 1), w * 0.9, h * 1.04, e=0.75, n=12,
                   a0=math.pi + 0.5, a1=2 * math.pi - 0.5, closed=False)
        b.tube(pts, 0.008, 'Bone', seg=6)
    markers.append(('bone_thorax', BODY['thorax'], V3(0, 1, 0)))

    # hind hip sockets on the pelvis
    pb = parts['pelvis'][0]
    for leg in ('HL', 'HR'):
        hip, knee, _ = LEGS[leg]
        pb.ring(hip, (knee - hip).normalized(), 0.05, 0.012, 'Bone', seg=14, tseg=6)

    # ---- neck ----
    b = part('neck')
    z_n = lambda y: 0.52 + (y - 0.66) * 0.2  # noqa: E731
    body_segment(b, b, [(0.6, 0.07, 0.065), (0.7, 0.065, 0.06), (0.8, 0.07, 0.06)], z_n, [(0.62, 0.72)],
                 seam_sides=False)
    for s in (-1, 1):
        conduit(b, [V3(s * 0.06, 0.6, 0.5), V3(s * 0.07, 0.7, 0.52), V3(s * 0.06, 0.79, 0.54)], 0.011)
    markers.append(('bone_neck', BODY['neck'], V3(0, 1, 0)))

    # ---- head: eyeless flat wedge of overlapping plates ----
    b = part('head')
    hp = [(0.74, 0.11, 0.06), (0.8, 0.135, 0.066), (0.92, 0.115, 0.052), (1.0, 0.08, 0.043), (1.07, 0.056, 0.04)]
    hprof = body_prof(hp)
    rings = []
    for i in range(15):
        y = hp[0][0] + (hp[-1][0] - hp[0][0]) * i / 14
        w, h = hprof(y)
        rings.append(ring(V3(0, y, head_axis_z(y)), V3(1, 0, 0), V3(0, 0, 1), w, h, e=0.55))
    loft(b, rings, 'ChitinMatte')
    # three overlapping crown plates, each flaring back over the one behind, bone rims
    for k, (y0, y1, flare) in enumerate(((0.95, 1.07, 0.008), (0.84, 0.98, 0.02), (0.72, 0.87, 0.032))):
        shell(b, y0, y1, lambda y, f=flare: (hprof(y)[0] + f, hprof(y)[1] + f * 0.6), head_axis_z, 'Bone', 'Chitin',
              thick=0.014, a0=0.05, a1=math.pi - 0.05, e=0.55, lift=0.006)
        # glow in the seam under the front of each plate
        w, h = hprof(y1)
        seam = ring(V3(0, y1 - 0.003, head_axis_z(y1)), V3(1, 0, 0), V3(0, 0, 1), w + flare, h + flare * 0.6, e=0.55,
                    n=12, a0=0.25, a1=math.pi - 0.25, closed=False)
        for i in range(len(seam) - 1):
            a, c = seam[i], seam[i + 1]
            b.face([a, c, c + V3(0, -0.01, -0.004), a + V3(0, -0.01, -0.004)], 'CreatureGlow',
                   toward=a - V3(0, a.y, head_axis_z(y1)))
    # ribbing over the skull: longitudinal bone ridges following the crown, and armoured
    # conduits from the cheeks back into the neck
    for xo in (-0.06, -0.03, 0.0, 0.03, 0.06):
        pts = []
        for i in range(9):
            y = 0.76 + (1.03 - 0.76) * i / 8
            w, h = hprof(y)
            if abs(xo) > w * 0.85:
                continue
            zz = (h + 0.045) * math.sqrt(max(0.0, 1.0 - (xo / (w + 0.05)) ** 2)) + 0.012 - 0.02 * (y - 0.76) / 0.27
            pts.append(V3(xo * (1.0 - 0.35 * (y - 0.76) / 0.27), y, head_axis_z(y) + zz))
        if len(pts) > 2:
            b.tube(pts, 0.0042 if xo else 0.0065, 'Bone', seg=6)
    for s in (-1, 1):
        conduit(b, [V3(s * 0.07, 1.02, head_axis_z(1.02) - 0.02), V3(s * 0.12, 0.9, head_axis_z(0.9) - 0.02),
                    V3(s * 0.13, 0.8, head_axis_z(0.8)), V3(s * 0.09, 0.7, 0.53)], 0.009, rings_every=0.04)
        conduit(b, [V3(s * 0.05, 1.0, head_axis_z(1.0) - 0.035), V3(s * 0.09, 0.86, head_axis_z(0.86) - 0.045),
                    V3(s * 0.07, 0.72, 0.5)], 0.007, rings_every=0.04)
    # swept-back crest spikes behind the crown, over the neck
    for s in (-1, 0, 1):
        base = V3(s * 0.07, 0.75, head_axis_z(0.75) + 0.06 - abs(s) * 0.02)
        spike(b, base, base + V3(s * 0.04, -0.15, 0.05 - abs(s) * 0.02), 0.014, 'Bone')
    # cheek ridges where the side jaws hinge
    for s in (-1, 1):
        b.tube([V3(s * 0.115, 0.8, head_axis_z(0.8) - 0.01), V3(s * 0.09, 0.94, head_axis_z(0.94) - 0.012),
                V3(s * 0.058, 1.05, head_axis_z(1.05) - 0.008)], 0.008, 'Bone', seg=8)
    # the throat: a glowing amber hollow behind the jaws, ringed with dark gums
    thc = V3(0, JAW_BASE_Y - 0.01, head_axis_z(JAW_BASE_Y))
    b.cyl(thc + V3(0, -0.03, 0), thc, 0.04, 'Sinew', seg=16, caps=(False, False))
    b.cyl(thc + V3(0, -0.032, 0), thc + V3(0, -0.03, 0), 0.038, 'CreatureGlow', seg=16, caps=(True, False))
    # a glowing ring at the root of the jaws, seen through the gaps between the petals
    b.ring(thc + V3(0, 0.004, 0), V3(0, 1, 0), 0.046, 0.007, 'CreatureGlow', seg=20, tseg=6)
    for k in range(10):  # an inner ring of teeth in the throat
        a = k * 2 * math.pi / 10
        d = V3(math.cos(a), 0, math.sin(a))
        base = thc + d * 0.034
        spike(b, base, base - d * 0.022 + V3(0, 0.012, 0), 0.004, 'Bone', seg=6)
    markers.append(('bone_head', BODY['head'], V3(0, 1, 0)))

    # ---- the four petal jaws ----
    zc = head_axis_z(JAW_BASE_Y)
    base_c = V3(0, JAW_BASE_Y, zc)
    tip = V3(0, JAW_TIP.y, zc - 0.004)
    for name, radial in JAWS.items():
        b = part('jaw_' + name, bevel=0.0012)
        lat = V3(0, 1, 0).cross(radial).normalized()  # across the petal
        R0 = 0.048 if radial.z != 0 else 0.06
        W0 = 0.058
        A, T = 7, 9
        outer, inner = [], []
        for ti in range(T + 1):
            t = ti / T
            row_o, row_i = [], []
            for ai in range(A + 1):
                a = -1 + 2 * ai / A
                w = W0 * (1 - t) ** 0.85 + 0.002
                bulge = 0.022 * math.sin(math.pi * min(1.0, t * 1.15))
                rad = R0 * (1 - t) ** 1.25 + bulge - 0.018 * a * a * (1 - t)
                p = base_c + (tip - base_c) * t + radial * rad + lat * (a * w)
                row_o.append(p)
                row_i.append(p - radial * (0.009 * (1 - t) + 0.002))
            outer.append(row_o)
            inner.append(row_i)
        vo = [[b.bm.verts.new(p) for p in r] for r in outer]
        vi = [[b.bm.verts.new(p) for p in r] for r in inner]
        for ti in range(T):
            for ai in range(A):
                f = b.face_verts([vo[ti][ai], vo[ti][ai + 1], vo[ti + 1][ai + 1], vo[ti + 1][ai]], 'Chitin')
                f.normal_update()
                if f.normal.dot(radial) < 0:
                    f.normal_flip()
                f = b.face_verts([vi[ti][ai], vi[ti][ai + 1], vi[ti + 1][ai + 1], vi[ti + 1][ai]], 'Sinew')
                f.normal_update()
                if f.normal.dot(radial) > 0:
                    f.normal_flip()
            for ai, side in ((0, -1), (A, 1)):  # side edges
                f = b.face_verts([vo[ti][ai], vo[ti + 1][ai], vi[ti + 1][ai], vi[ti][ai]], 'Sinew')
                f.normal_update()
                if f.normal.dot(lat * side) < 0:
                    f.normal_flip()
        f = b.face_verts([vo[0][ai] for ai in range(A + 1)] + [vi[0][ai] for ai in range(A, -1, -1)], 'Sinew')
        f.normal_update()
        if f.normal.dot(V3(0, -1, 0)) < 0:
            f.normal_flip()
        # needle teeth along both inner edges, pointing in and back
        for ti in range(1, T - 1):
            t = ti / T
            for ai in (0, A):
                p = inner[ti][ai]
                L = 0.024 * (1 - t) + 0.008
                spike(b, p, p - radial * L + V3(0, -0.006, 0) + lat * (0.003 if ai == 0 else -0.003), 0.0028, 'Bone',
                      seg=6)
        # a ridge down the middle of each petal, and a glowing slit along its base
        b.tube([outer[1][A // 2] + radial * 0.002, outer[T // 2][A // 2] + radial * 0.004,
                outer[T - 1][A // 2] + radial * 0.001], 0.004, 'ChitinMatte', seg=6)
        for ai in range(1, A - 1):
            p0, p1 = outer[1][ai] + radial * 0.0015, outer[1][ai + 1] + radial * 0.0015
            b.face([p0, p1, p1 + (outer[2][ai + 1] - outer[1][ai + 1]) * 0.35,
                    p0 + (outer[2][ai] - outer[1][ai]) * 0.35], 'CreatureGlow', toward=radial)
        markers.append(('bone_jaw_' + name, base_c + radial * R0, V3(0, 1, 0)))

    # ---- tail: six segments ending in a hook ----
    for i, (y, z) in enumerate(zip(TAIL_Y, TAIL_Z)):
        b = part(f'tail{i}')
        y_next = TAIL_Y[i + 1] if i + 1 < len(TAIL_Y) else y - 0.16
        z_next = TAIL_Z[i + 1] if i + 1 < len(TAIL_Z) else z + 0.05
        r0 = 0.075 * (1 - i / 7.5)
        r1 = 0.075 * (1 - (i + 1) / 7.5)
        a, c = V3(0, y + 0.015, z), V3(0, y_next + 0.012, z_next)
        tube_loft(b, a, c, [(0.0, r0 * 0.8), (0.2, r0), (0.7, r0 * 0.95), (1.0, r1 * 0.85)], 'Chitin', e=0.8,
                  flat=1.15)
        b.ring(a + (c - a) * 0.1, (c - a).normalized(), r0 * 0.95, 0.008, 'Bone', seg=14, tseg=5)
        # a faint seam where this segment sits in the next (mostly hidden)
        b.ring(a + (c - a) * 0.98, (c - a).normalized(), r1 * 0.8, 0.0032, 'CreatureGlow', seg=14, tseg=4)
        mid = (a + c) * 0.5
        spike(b, mid + V3(0, 0, r0 * 0.9), mid + V3(0, -0.05, r0 * 0.9 + 0.035 * (1 - i / 7)), 0.01, 'Bone')
        if i == len(TAIL_Y) - 1:
            hook(b, c, V3(0, -1, 0.15), V3(0, 0.3, 1), 0.22, 0.02, 'Bone', curl=2.1)
        markers.append((f'bone_tail{i}', V3(0, y, z), (c - a).normalized()))

    # ---- walking legs ----
    for leg, (hip, knee, foot) in LEGS.items():
        s = 1 if leg.endswith('R') else -1
        b = part('femur_' + leg, bevel=0.0018)
        up = V3(0, 0, 1)
        d = (knee - hip).normalized()
        side = d.cross(up).normalized()
        top = side.cross(d).normalized()
        # a heavy thigh: plated, slightly bowed upward, tapering to the knee
        fpts = bezier(hip, knee, (hip + knee) * 0.5 + top * 0.04, 8)
        fr = [0.058 - 0.022 * (i / 8) for i in range(9)]
        plated_limb(b, fpts, fr, up, n_plates=3, flat=0.8, tendons=2, spines=3)
        # the knee: a bone knuckle with a long backswept spike
        sphere(b, knee, 0.042, 'Chitin', seg=14, rings=7)
        b.ring(knee, d, 0.04, 0.007, 'Bone', seg=14, tseg=5)
        spike(b, knee + top * 0.02, knee + top * 0.02 + d * 0.1 + V3(0, 0, 0.06), 0.02, 'Bone')
        markers.append(('bone_femur_' + leg, hip, (knee - hip).normalized()))

        b = part('tibia_' + leg, bevel=0.0015)
        claw_base = knee + (foot - knee) * 0.86
        # a bowed lower leg (outward and up, like a crab's), in two plated sections with a
        # joint collar between them
        ctrl = (knee + claw_base) * 0.5 + V3(s * 0.1, 0, 0.07)
        curve = bezier(knee, claw_base, ctrl, 12)
        radii = [0.046 * (1 - 0.6 * (i / 12) ** 0.85) for i in range(13)]
        plated_limb(b, curve, radii, V3(s, 0, 0), n_plates=4, flat=0.72, tendons=1, spines=0)
        # serrated spikes down the back edge
        for k in range(7):  # serrated spikes along the outer edge
            i = 1 + k
            p = curve[i]
            out = V3(s, 0, 0.4).normalized()
            spike(b, p + out * radii[i] * 0.6, p + out * (radii[i] + 0.022) + (curve[i] - curve[i + 1]) * 0.6,
                  0.005 * (1 - k / 10), 'Bone', seg=6)
        conduit(b, [curve[0] + V3(-s * 0.02, 0, -0.01), curve[3] + V3(-s * 0.026, 0, -0.012),
                    curve[6] + V3(-s * 0.02, 0, -0.01)], 0.007, rings_every=0.1)
        # the hooked claw: bends down and inward to grip
        dvec = (foot - claw_base).normalized()
        tip = hook(b, claw_base, dvec, V3(-s, 0, -0.3), (foot - claw_base).length * 1.15, 0.016, 'Bone', curl=0.9)
        # a smaller back claw
        hook(b, claw_base, (dvec + V3(0, -0.6 if leg[0] == 'M' else 0.6, 0)).normalized(), V3(-s, 0, 0), 0.07,
             0.009, 'Bone', curl=0.7)
        markers.append(('bone_tibia_' + leg, knee, (foot - knee).normalized()))
        markers.append(('foot_' + leg, tip, V3(0, 0, -1)))

    # ---- forelimbs: folded, ending in scythe hooks ----
    for side, (sh, el, wr, hk) in ARMS.items():
        s = 1 if side == 'R' else -1
        b = part('arm_' + side, bevel=0.0018)
        # a heavy plated upper arm with tendons and spines along the top
        apts = bezier(sh, el, (sh + el) * 0.5 + V3(s * 0.03, 0.02, 0.0), 8)
        plated_limb(b, apts, [0.06 - 0.016 * (i / 8) for i in range(9)], V3(s, 0, 0), n_plates=3, flat=0.8,
                    tendons=2, spines=0)
        for k in range(4):
            t = 0.2 + k * 0.2
            p = sh + (el - sh) * t + V3(s * 0.025, 0, 0.03)
            spike(b, p, p + V3(s * 0.03, -0.03, 0.04), 0.008, 'Bone', seg=6)
        conduit(b, [sh + V3(0, 0, -0.03), (sh + el) * 0.5 + V3(s * 0.02, 0, -0.04), el + V3(0, 0, -0.028)], 0.008)
        # the elbow ends in a backswept bone spike, not a round cap
        d_up = (el - sh).normalized()
        tube_loft(b, el - d_up * 0.02, el + d_up * 0.05, [(0.0, 0.034), (1.0, 0.012)], 'Chitin', e=0.8,
                  up=V3(s, 0, 0), flat=0.8)
        spike(b, el + d_up * 0.03, el + d_up * 0.06 + V3(s * 0.02, -0.12, 0.1), 0.018, 'Bone')
        markers.append(('bone_arm_' + side, sh, (el - sh).normalized()))

        b = part('fore_' + side, bevel=0.0015)
        # the forearm is a flat blade folded back under the upper arm, serrated inside
        tube_loft(b, el, wr, [(0.0, 0.042), (0.3, 0.05), (1.0, 0.03)], 'Chitin', e=0.65, up=V3(s, 0, 0), flat=0.55)
        conduit(b, [el + V3(s * 0.03, 0, 0.0), (el + wr) * 0.5 + V3(s * 0.035, 0, 0.0), wr + V3(s * 0.022, 0, 0)],
                0.008, rings_every=0.05)
        for k in range(6):  # a comb of spines on the inside edge
            t = 0.15 + k * 0.13
            p = el + (wr - el) * t
            spike(b, p + V3(0, 0.02, 0), p + V3(-s * 0.005, 0.065 - k * 0.004, 0.012), 0.0055, 'Bone', seg=6)
        sphere(b, wr, 0.024, 'ChitinMatte', seg=10, rings=5)
        # the scythe: long hook forward and down, with two small finger hooks
        hook(b, wr, (hk - wr).normalized(), V3(0, 0, -1), (hk - wr).length * 1.05, 0.026, 'Bone', curl=1.25, steps=10)
        hook(b, wr, (V3(-s * 0.3, 1, -0.5)).normalized(), V3(0, 0, -1), 0.09, 0.009, 'Bone', curl=1.0)
        hook(b, wr, (V3(s * 0.4, 1, -0.6)).normalized(), V3(0, 0, -1), 0.08, 0.008, 'Bone', curl=1.0)
        markers.append(('bone_fore_' + side, el, (wr - el).normalized()))

    return parts, markers


BONE_PARENT = {
    'pelvis': 'body', 'waist': 'pelvis', 'thorax': 'waist', 'neck': 'thorax', 'head': 'neck',
    'jaw_t': 'head', 'jaw_b': 'head', 'jaw_l': 'head', 'jaw_r': 'head',
    'tail0': 'pelvis', 'tail1': 'tail0', 'tail2': 'tail1', 'tail3': 'tail2', 'tail4': 'tail3', 'tail5': 'tail4',
    'femur_ML': 'thorax', 'femur_MR': 'thorax', 'femur_HL': 'pelvis', 'femur_HR': 'pelvis',
    'tibia_ML': 'femur_ML', 'tibia_MR': 'femur_MR', 'tibia_HL': 'femur_HL', 'tibia_HR': 'femur_HR',
    'arm_L': 'thorax', 'arm_R': 'thorax', 'fore_L': 'arm_L', 'fore_R': 'arm_R',
}


def build_creature(parent):
    """The exported kit piece: one rigid mesh per bone, in the rest pose, plus markers."""
    pc = Piece('creature')
    parts, markers = build_parts()
    for name, (b, bevel) in parts.items():
        pc.extra_objects.append((name, b, bevel))
    for name, loc, d in markers:
        pc.marker(name, loc, d)
    coll = pc.finish(parent)
    # organic parts shade smooth, with sharp edges kept on plate rims
    for o in coll.objects:
        if o.type == 'MESH':
            o.data.shade_smooth()
            o.data.set_sharp_from_angle(angle=math.radians(48))
            for mod in list(o.modifiers):
                if mod.type == 'BEVEL':
                    mod.harden_normals = False
    return coll


# ---------------------------------------------------------------------------
# Rig and crawl cycle (Blender only, for the renders)
# ---------------------------------------------------------------------------

CYCLE = 32          # frames per crawl cycle
STRIDE = 0.42       # metres per step


def bone_head_tail(name):
    if name in BODY:
        h = BODY[name]
        return h, h + V3(0, 0.12, 0)
    if name.startswith('jaw_'):
        r = JAWS[name[4:]]
        h = V3(0, JAW_BASE_Y, head_axis_z(JAW_BASE_Y)) + r * 0.05
        return h, h + V3(0, 0.1, 0)
    if name.startswith('tail'):
        i = int(name[4:])
        h = V3(0, TAIL_Y[i], TAIL_Z[i])
        t = V3(0, TAIL_Y[i + 1], TAIL_Z[i + 1]) if i + 1 < len(TAIL_Y) else h + V3(0, -0.16, 0.04)
        return h, t
    if name.startswith('femur_'):
        hip, knee, _ = LEGS[name[6:]]
        return hip, knee
    if name.startswith('tibia_'):
        _, knee, foot = LEGS[name[6:]]
        return knee, foot
    if name.startswith('arm_'):
        sh, el, _, _ = ARMS[name[4:]]
        return sh, el
    if name.startswith('fore_'):
        _, el, wr, _ = ARMS[name[5:]]
        return el, wr
    raise KeyError(name)


def build_rig(kit_coll):
    """CREATURE_RIG: armature + the kit meshes (linked) parented to bones, IK legs with
    stepping targets, and a looping crawl cycle on frames 1..CYCLE."""
    rig_coll = collection('CREATURE_RIG')
    clear_collection(rig_coll)
    from bk_studio import layer_collection
    lc = layer_collection('CREATURE_RIG')
    if lc:
        lc.exclude = False
    for o in list(bpy.data.objects):
        if o.name.startswith('CR_') and o.users_collection == ():
            bpy.data.objects.remove(o)
    arm_data = bpy.data.armatures.get('CR_Armature') or bpy.data.armatures.new('CR_Armature')
    arm = bpy.data.objects.new('CR_Rig', arm_data)
    rig_coll.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    for o in bpy.context.selected_objects:
        o.select_set(False)
    arm.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    eb = arm_data.edit_bones
    for e in list(eb):
        eb.remove(e)
    root = eb.new('body')
    root.head, root.tail = V3(0, 0, 0), V3(0, 0.3, 0)
    order = ['pelvis', 'waist', 'thorax', 'neck', 'head', 'jaw_t', 'jaw_b', 'jaw_l', 'jaw_r'] + \
            [f'tail{i}' for i in range(6)] + \
            [f'femur_{l}' for l in LEGS] + [f'tibia_{l}' for l in LEGS] + ['arm_L', 'arm_R', 'fore_L', 'fore_R']
    for name in order:
        bb = eb.new(name)
        bb.head, bb.tail = bone_head_tail(name)
        bb.parent = eb[BONE_PARENT[name]]
        if name.startswith('tibia_'):
            bb.use_connect = True
    bpy.ops.object.mode_set(mode='OBJECT')

    # parts follow their bones (rigid)
    for o in list(kit_coll.objects):
        if o.type != 'MESH':
            continue
        bone = o.name[len('creature_'):]
        if bone not in arm_data.bones:
            continue
        c = bpy.data.objects.new('CR_' + bone, o.data)
        rig_coll.objects.link(c)
        c.parent = arm
        c.parent_type = 'BONE'
        c.parent_bone = bone
        bm = arm_data.bones[bone]
        c.matrix_parent_inverse = (arm.matrix_world @ bm.matrix_local @ Matrix.Translation((0, bm.length, 0))).inverted()
        for mod in o.modifiers:
            m2 = c.modifiers.new(mod.name, mod.type)
            for attr in ('width', 'segments', 'limit_method', 'angle_limit', 'use_clamp_overlap', 'harden_normals',
                         'keep_sharp', 'weight'):
                if hasattr(mod, attr):
                    try:
                        setattr(m2, attr, getattr(mod, attr))
                    except Exception:  # noqa: BLE001
                        pass

    # IK legs: a target per foot and a pole above each knee
    targets = {}
    for leg, (hip, knee, foot) in LEGS.items():
        t = bpy.data.objects.new('CR_tgt_' + leg, None)
        t.empty_display_size = 0.08
        t.location = foot
        rig_coll.objects.link(t)
        p = bpy.data.objects.new('CR_pole_' + leg, None)
        p.empty_display_size = 0.05
        # the pole sits in the leg's plane, straight out from the knee
        p.location = knee + (knee - (hip + foot) * 0.5).normalized() * 0.6
        rig_coll.objects.link(p)
        pb = arm.pose.bones['tibia_' + leg]
        ik = pb.constraints.new('IK')
        ik.target = t
        ik.pole_target = p
        # found by searching for the angle that puts the knee back at its rest position
        ik.pole_angle = math.radians({'MR': 124, 'ML': 56, 'HR': 170, 'HL': 10}[leg])
        ik.chain_count = 2
        targets[leg] = (t, foot)

    # the crawl cycle: diagonal pairs step in turn; the body bobs and the tail sways
    scene = bpy.context.scene
    scene.frame_start, scene.frame_end = 1, CYCLE
    phase = {'ML': 0.0, 'HR': 0.0, 'MR': 0.5, 'HL': 0.5}
    duty = 0.62
    for f in range(1, CYCLE + 2):
        u = ((f - 1) % CYCLE) / CYCLE
        for leg, (t, foot) in targets.items():
            ph = (u + phase[leg]) % 1.0
            if ph < duty:
                k = ph / duty
                y = STRIDE * (0.5 - k)
                z = 0.0
            else:
                k = (ph - duty) / (1 - duty)
                y = STRIDE * (-0.5 + k)
                z = 0.13 * math.sin(math.pi * k)
            t.location = foot + V3(0, y, z)
            t.keyframe_insert('location', frame=f)
        pb = arm.pose.bones
        pb['pelvis'].location = (0, 0, 0.012 * math.sin(4 * math.pi * u))
        pb['pelvis'].keyframe_insert('location', frame=f)
        for name, amp, lag in (('waist', 0.05, 0.3), ('thorax', 0.04, 0.6), ('neck', 0.06, 0.9), ('head', 0.08, 1.2)):
            pb[name].rotation_mode = 'XYZ'
            pb[name].rotation_euler = (0, 0, amp * math.sin(2 * math.pi * u - lag))
            pb[name].keyframe_insert('rotation_euler', frame=f)
        for i in range(6):
            nm = f'tail{i}'
            pb[nm].rotation_mode = 'XYZ'
            pb[nm].rotation_euler = (0.03 * math.sin(2 * math.pi * u - i * 0.5),
                                     0, 0.12 * math.sin(2 * math.pi * u - 0.8 - i * 0.55))
            pb[nm].keyframe_insert('rotation_euler', frame=f)
        for nm in ('arm_L', 'arm_R'):
            pb[nm].rotation_mode = 'XYZ'
            pb[nm].rotation_euler = (0.06 * math.sin(2 * math.pi * u + (0 if nm.endswith('L') else math.pi)), 0, 0)
            pb[nm].keyframe_insert('rotation_euler', frame=f)
        opening = 0.08 + 0.06 * math.sin(2 * math.pi * u)
        # (bones point +Y: +X turns the tip up, +Z turns it toward -X)
        for jn, axis_sign in (('jaw_t', (1, 0, 0)), ('jaw_b', (-1, 0, 0)), ('jaw_l', (0, 0, 1)), ('jaw_r', (0, 0, -1))):
            pb[jn].rotation_mode = 'XYZ'
            pb[jn].rotation_euler = tuple(a * opening for a in axis_sign)
            pb[jn].keyframe_insert('rotation_euler', frame=f)
    if arm.animation_data and arm.animation_data.action:
        arm.animation_data.action.name = 'CR_crawl'
    for o in [arm] + [targets[l][0] for l in targets]:
        if o.animation_data and o.animation_data.action:
            for fc in o.animation_data.action.fcurves if hasattr(o.animation_data.action, 'fcurves') else []:
                for kp in fc.keyframe_points:
                    kp.interpolation = 'LINEAR'
    return rig_coll
