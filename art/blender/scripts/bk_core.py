"""Bulkhead kit: shared modelling helpers.

All kit geometry is built procedurally with bmesh so it is exact, repeatable and
modular. Units are metres; Blender axes: X east, Y north, Z up. A maze cell is
4 x 4 m with its origin at the cell centre on the floor.

Key ideas
- sweep(): extrudes a 2D profile (o = offset toward the walkway, z = height) along a
  2D polyline with mitred corners. Walls, trim rails, pipes and light strips are all
  sweeps, so they join seamlessly between neighbouring modules.
- Every face is created facing the viewer side; materials use backface culling, so a
  wrongly oriented face shows up as a hole in renders.
"""

import math
import random

import bmesh
import bpy
from mathutils import Matrix, Vector

# ---------------------------------------------------------------------------
# Materials
# ---------------------------------------------------------------------------


def _lin(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def hexcol(h):
    h = h.lstrip('#')
    return tuple(_lin(int(h[i:i + 2], 16) / 255.0) for i in (0, 2, 4))


# name: c = base colour (sRGB hex), m = metallic, r = roughness,
#       e = emission colour, es = emission strength, a = alpha
PALETTE = {
    'PanelWhite':   dict(c='c3c7c5', m=0.0, r=0.36),
    'PanelLight':   dict(c='9ea5aa', m=0.0, r=0.42),
    'PanelGrey':    dict(c='5a6268', m=0.0, r=0.48),
    'PanelDark':    dict(c='2b3035', m=0.0, r=0.46),
    'Gunmetal':     dict(c='41474d', m=0.85, r=0.3),
    'Steel':        dict(c='b9bdc1', m=1.0, r=0.24),
    'DarkSteel':    dict(c='383c40', m=0.9, r=0.4),
    'Recess':       dict(c='121417', m=0.2, r=0.65),
    'Rubber':       dict(c='151617', m=0.0, r=0.85),
    'HazardYellow': dict(c='e2a614', m=0.0, r=0.48),
    'HazardBlack':  dict(c='1b1b1c', m=0.0, r=0.55),
    'AccentOrange': dict(c='dd5f1b', m=0.0, r=0.42),
    'PipeBlue':     dict(c='2c6894', m=0.35, r=0.36),
    'PipeYellow':   dict(c='c99a24', m=0.35, r=0.4),
    'PipeRed':      dict(c='9a2d25', m=0.3, r=0.42),
    'Copper':       dict(c='c47c52', m=1.0, r=0.32),
    'CableBlack':   dict(c='19191a', m=0.0, r=0.6),
    'CableRed':     dict(c='8a1c17', m=0.0, r=0.55),
    'CableBlue':    dict(c='1c4e8a', m=0.0, r=0.55),
    'CableYellow':  dict(c='c49c1e', m=0.0, r=0.55),
    'Paint':        dict(c='d9dad2', m=0.0, r=0.6),
    'PaintYellow':  dict(c='e1b01b', m=0.0, r=0.6),
    'PaintDark':    dict(c='26292c', m=0.0, r=0.6),
    'CrateOlive':   dict(c='57643d', m=0.0, r=0.55),
    'CrateOrange':  dict(c='c55f1d', m=0.0, r=0.5),
    'CrateGrey':    dict(c='79808a', m=0.0, r=0.5),
    'BarrelBlue':   dict(c='265483', m=0.25, r=0.42),
    'BarrelRed':    dict(c='952a20', m=0.25, r=0.42),
    'Polymer':      dict(c='212428', m=0.0, r=0.62),
    'Glass':        dict(c='2e4b5c', m=0.0, r=0.04, a=0.28),
    # part 3: viewport glass looking out into space: nearly clear, faintly blue-green
    # (no diffuse to speak of: dark base colour, the tint is its hue; the engine's glass
    # shading reflects with Fresnel and transmits the rest)
    'ViewGlass':    dict(c='1a2a2e', m=0.0, r=0.02, a=0.12),
    'LightWhite':   dict(c='ffffff', m=0.0, r=0.3, e='e4f2ff', es=14.0),
    'LightTeal':    dict(c='ffffff', m=0.0, r=0.3, e='46dcff', es=9.0),
    'LightRed':     dict(c='ffffff', m=0.0, r=0.3, e='ff2a12', es=9.0),
    'LightGreen':   dict(c='ffffff', m=0.0, r=0.3, e='38ff66', es=3.5),
    'LightAmber':   dict(c='ffffff', m=0.0, r=0.3, e='ffa01c', es=9.0),
    'Screen':       dict(c='061017', m=0.0, r=0.2, e='1fbfff', es=3.0),
    'ScreenDim':    dict(c='061017', m=0.0, r=0.2, e='0c4f6e', es=2.0),
    # round 6-7: derelict pieces, effects, the player's arms
    'Rust':         dict(c='5e2a12', m=0.3, r=0.8),
    'Soot':         dict(c='080807', m=0.0, r=0.95),
    'Glove':        dict(c='242523', m=0.0, r=0.72),
    'GloveArmor':   dict(c='474c50', m=0.65, r=0.36),
    'SuitFabric':   dict(c='3b4136', m=0.0, r=0.86),
    'SuitPlate':    dict(c='8a8e86', m=0.15, r=0.48),
    'LightSpark':   dict(c='ffffff', m=0.0, r=0.3, e='ffb35a', es=60.0),
    'LightBolt':    dict(c='ffffff', m=0.0, r=0.3, e='86ecff', es=70.0),
    'LightFlash':   dict(c='ffffff', m=0.0, r=0.3, e='e2f8ff', es=30.0),   # muzzle flash: hot core
    'LightFlashMid': dict(c='ffffff', m=0.0, r=0.3, e='4fd2ff', es=9.0),   # inner petals, cyan
    'LightFlashOut': dict(c='ffffff', m=0.0, r=0.3, e='1a7bff', es=3.5),   # outer petals, deep blue
    # round 8: the creature
    'Chitin':       dict(c='15171a', m=0.4, r=0.13),     # wet graphite black
    'ChitinMatte':  dict(c='1d1f1f', m=0.2, r=0.28),
    'Bone':         dict(c='54493a', m=0.05, r=0.3),     # ridges, teeth, claws: aged dark ivory, waxy
    'Sinew':        dict(c='2a1512', m=0.1, r=0.2),      # tendons, gums: dark, wet
    'CreatureGlow': dict(c='ffffff', m=0.0, r=0.3, e='ff8a1e', es=16.0),  # amber seams and throat
}


def material(name):
    """Returns (creating if needed) the kit material BH_<name>."""
    full = 'BH_' + name
    mat = bpy.data.materials.get(full)
    spec = PALETTE[name]
    if mat is None:
        mat = bpy.data.materials.new(full)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if bsdf is None:
        nt.nodes.clear()
        out = nt.nodes.new('ShaderNodeOutputMaterial')
        bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
        nt.links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
    base = hexcol(spec['c'])
    bsdf.inputs['Base Color'].default_value = (*base, 1.0)
    bsdf.inputs['Metallic'].default_value = spec['m']
    bsdf.inputs['Roughness'].default_value = spec['r']
    if 'e' in spec:
        bsdf.inputs['Emission Color'].default_value = (*hexcol(spec['e']), 1.0)
        bsdf.inputs['Emission Strength'].default_value = spec['es']
    else:
        bsdf.inputs['Emission Strength'].default_value = 0.0
    alpha = spec.get('a', 1.0)
    bsdf.inputs['Alpha'].default_value = alpha
    mat.diffuse_color = (*base, 1.0)
    mat.metallic = spec['m']
    mat.roughness = spec['r']
    mat.use_backface_culling = alpha >= 1.0
    if alpha < 1.0:
        if hasattr(mat, 'surface_render_method'):
            mat.surface_render_method = 'BLENDED'
        if hasattr(mat, 'blend_method'):
            mat.blend_method = 'BLEND'
    return mat


# ---------------------------------------------------------------------------
# Geometry helpers
# ---------------------------------------------------------------------------

V2 = lambda x, y: Vector((x, y))  # noqa: E731
V3 = lambda x, y, z: Vector((x, y, z))  # noqa: E731


def perp(d):
    """Left normal of a 2D direction."""
    return Vector((-d.y, d.x))


def miters(path, closed=False):
    """Mitre vectors for a 2D polyline: offsetting point i by m_i * o gives a
    parallel line at distance o on the left side, with exact mitred corners."""
    n = len(path)
    out = []
    for i in range(n):
        if closed or 0 < i < n - 1:
            a, b, c = path[(i - 1) % n], path[i], path[(i + 1) % n]
            n1 = perp((b - a).normalized())
            n2 = perp((c - b).normalized())
            out.append((n1 + n2) / (1.0 + n1.dot(n2)))
        elif i == 0:
            out.append(perp((path[1] - path[0]).normalized()))
        else:
            out.append(perp((path[-1] - path[-2]).normalized()))
    return out


def subdivide(path, max_len):
    pts = [path[0].copy()]
    for a, b in zip(path, path[1:]):
        k = max(1, math.ceil((b - a).length / max_len - 1e-6))
        for j in range(1, k + 1):
            pts.append(a.lerp(b, j / k))
    return pts


def path_length(path):
    return sum((b - a).length for a, b in zip(path, path[1:]))


def basis_from_axis(axis):
    """Two unit vectors perpendicular to axis (and each other)."""
    axis = axis.normalized()
    ref = Vector((0, 0, 1)) if abs(axis.z) < 0.9 else Vector((1, 0, 0))
    u = axis.cross(ref).normalized()
    v = axis.cross(u).normalized()
    return u, v


class Builder:
    """Accumulates faces in a bmesh with per-face material names."""

    def __init__(self):
        self.bm = bmesh.new()
        self.mats = []

    def mi(self, name):
        if name not in self.mats:
            self.mats.append(name)
        return self.mats.index(name)

    def face(self, pts, mat, toward=None):
        """New face from points. If `toward` is given the face is flipped so its
        normal points along that direction."""
        if toward is not None:
            n = (pts[1] - pts[0]).cross(pts[2] - pts[0])
            if n.length < 1e-12 and len(pts) > 3:
                n = (pts[2] - pts[0]).cross(pts[3] - pts[0])
            if n.dot(toward) < 0:
                pts = list(reversed(pts))
        vs = [self.bm.verts.new(p) for p in pts]
        f = self.bm.faces.new(vs)
        f.material_index = self.mi(mat)
        return f

    def face_verts(self, vs, mat):
        f = self.bm.faces.new(vs)
        f.material_index = self.mi(mat)
        return f

    # --- solids ----------------------------------------------------------

    def box(self, mn, mx, mat, M=None, mats=None):
        """Axis-aligned box (in local space, then transformed by M). Returns faces.
        mats can override per side: dict with keys '+x','-x','+y','-y','+z','-z'."""
        x0, y0, z0 = mn
        x1, y1, z1 = mx
        c = [V3(x0, y0, z0), V3(x1, y0, z0), V3(x1, y1, z0), V3(x0, y1, z0),
             V3(x0, y0, z1), V3(x1, y0, z1), V3(x1, y1, z1), V3(x0, y1, z1)]
        if M is not None:
            c = [M @ p for p in c]
        vs = [self.bm.verts.new(p) for p in c]
        sides = {
            '-z': (0, 3, 2, 1), '+z': (4, 5, 6, 7), '-y': (0, 1, 5, 4),
            '+y': (2, 3, 7, 6), '-x': (0, 4, 7, 3), '+x': (1, 2, 6, 5)}
        faces = {}
        for key, idx in sides.items():
            m = (mats or {}).get(key, mat)
            faces[key] = self.face_verts([vs[i] for i in idx], m)
        return faces

    def obox(self, center, size, mat, rot=None, mats=None):
        """Box of `size` centred at `center`, rotated by 3x3 matrix `rot`."""
        M = Matrix.Translation(center)
        if rot is not None:
            M = M @ rot.to_4x4()
        h = Vector(size) * 0.5
        return self.box(-h, h, mat, M=M, mats=mats)

    def cyl(self, p0, p1, r, mat, seg=16, caps=(True, True), cap_mat=None, r1=None):
        """Cylinder (or cone if r1 given) from p0 to p1."""
        p0, p1 = Vector(p0), Vector(p1)
        axis = p1 - p0
        u, v = basis_from_axis(axis)
        r1 = r if r1 is None else r1
        ring0, ring1 = [], []
        for i in range(seg):
            a = 2 * math.pi * i / seg
            d = u * math.cos(a) + v * math.sin(a)
            ring0.append(self.bm.verts.new(p0 + d * r))
            ring1.append(self.bm.verts.new(p1 + d * r1))
        faces = []
        for i in range(seg):
            j = (i + 1) % seg
            quad = [ring0[i], ring0[j], ring1[j], ring1[i]]
            f = self.face_verts(quad, mat)
            f.normal_update()
            mid = (ring0[i].co + ring0[j].co) * 0.5 - p0
            if f.normal.dot(mid) < 0:
                f.normal_flip()
            faces.append(f)
        cm = cap_mat or mat
        if caps[0]:
            f = self.face_verts(list(ring0), cm)
            f.normal_update()
            if f.normal.dot(-axis) < 0:
                f.normal_flip()
        if caps[1]:
            f = self.face_verts(list(ring1), cm)
            f.normal_update()
            if f.normal.dot(axis) < 0:
                f.normal_flip()
        return faces

    def tube(self, pts, r, mat, seg=10, closed=False, caps=True):
        """Tube along a 3D polyline using rotation-minimising frames."""
        pts = [Vector(p) for p in pts]
        n = len(pts)
        tangents = []
        for i in range(n):
            if closed:
                t = (pts[(i + 1) % n] - pts[i - 1])
            elif i == 0:
                t = pts[1] - pts[0]
            elif i == n - 1:
                t = pts[-1] - pts[-2]
            else:
                t = (pts[i + 1] - pts[i]).normalized() + (pts[i] - pts[i - 1]).normalized()
            tangents.append(t.normalized())
        nrm, _ = basis_from_axis(tangents[0])
        rings = []
        for i in range(n):
            t = tangents[i]
            nrm = (nrm - t * nrm.dot(t)).normalized()
            b = t.cross(nrm)
            ring = []
            for k in range(seg):
                a = 2 * math.pi * k / seg
                ring.append(self.bm.verts.new(pts[i] + (nrm * math.cos(a) + b * math.sin(a)) * r))
            rings.append(ring)
        last = n if closed else n - 1
        for i in range(last):
            ra, rb = rings[i], rings[(i + 1) % n]
            for k in range(seg):
                k2 = (k + 1) % seg
                f = self.face_verts([ra[k], ra[k2], rb[k2], rb[k]], mat)
                f.normal_update()
                if f.normal.dot(ra[k].co - pts[i]) < 0:
                    f.normal_flip()
        if caps and not closed:
            for ring, t in ((rings[0], -tangents[0]), (rings[-1], tangents[-1])):
                f = self.face_verts(list(ring), mat)
                f.normal_update()
                if f.normal.dot(t) < 0:
                    f.normal_flip()

    def lathe(self, profile, mat, seg=32, M=None, mats=None):
        """Revolves a profile [(r, z), ...] (traversed with the outside on the right,
        e.g. bottom centre -> rim -> top centre) around local Z. Welded mesh.
        mats: optional list of material names per profile segment."""
        rings = []
        for r, z in profile:
            if r < 1e-9:
                p = V3(0, 0, z)
                rings.append([self.bm.verts.new(M @ p if M is not None else p)])
                continue
            ring = []
            for j in range(seg):
                a = 2 * math.pi * j / seg
                p = V3(r * math.cos(a), r * math.sin(a), z)
                ring.append(self.bm.verts.new(M @ p if M is not None else p))
            rings.append(ring)
        R3 = M.to_3x3() if M is not None else Matrix.Identity(3)
        for k in range(len(profile) - 1):
            (r0, z0), (r1, z1) = profile[k], profile[k + 1]
            dr, dz = r1 - r0, z1 - z0
            rn = Vector((dz, -dr))  # right normal in (r, z)
            ra, rb = rings[k], rings[k + 1]
            m = mats[k] if mats else mat
            for j in range(seg):
                a = 2 * math.pi * (j + 0.5) / seg
                expect = R3 @ V3(rn.x * math.cos(a), rn.x * math.sin(a), rn.y)
                j2 = (j + 1) % seg
                if len(ra) == 1:
                    vs = [ra[0], rb[j], rb[j2]]
                elif len(rb) == 1:
                    vs = [ra[j], ra[j2], rb[0]]
                else:
                    vs = [ra[j], ra[j2], rb[j2], rb[j]]
                f = self.face_verts(vs, m)
                f.normal_update()
                if f.normal.dot(expect) < 0:
                    f.normal_flip()

    def ring(self, center, axis, R, r, mat, seg=24, tseg=8):
        """Torus around `axis`."""
        u, v = basis_from_axis(Vector(axis))
        pts = [Vector(center) + (u * math.cos(2 * math.pi * i / seg) + v * math.sin(2 * math.pi * i / seg)) * R
               for i in range(seg)]
        self.tube(pts, r, mat, seg=tseg, closed=True, caps=False)

    # --- sweeps ------------------------------------------------------------

    def sweep(self, path, profile, mat, closed_profile=False, M=None, closed_path=False):
        """Extrude `profile` [(o, z), ...] along 2D `path`. o offsets toward the left
        of travel. Faces face the left side (the walkway). Returns faces[i][k]."""
        ms = miters(path, closed_path)
        rows = []
        for p, m in zip(path, ms):
            row = []
            for o, z in profile:
                q = p + m * o
                v = V3(q.x, q.y, z)
                if M is not None:
                    v = M @ v
                row.append(self.bm.verts.new(v))
            rows.append(row)
        K = len(profile)
        kmax = K if closed_profile else K - 1
        n = len(path)
        imax = n if closed_path else n - 1
        faces = []
        for i in range(imax):
            i2 = (i + 1) % n
            fr = []
            for k in range(kmax):
                k2 = (k + 1) % K
                mname = mat(i, k) if callable(mat) else mat
                fr.append(self.face_verts([rows[i][k], rows[i][k2], rows[i2][k2], rows[i2][k]], mname))
            faces.append(fr)
        return faces

    def band(self, outline, t, y0, y1, mat, M=None, front=True, back=True, inner_mat=None):
        """A frame/rib following a cross-section outline [(x, z), ...] (traversed so the
        interior is on the RIGHT), thickness t toward the interior, extruded y0..y1."""
        out2 = [V2(x, z) for x, z in outline]
        ms = miters(out2)
        inn = [p - m * t for p, m in zip(out2, ms)]

        def P(p, y):
            v = V3(p.x, y, p.y)
            return M @ v if M is not None else v
        dir_front = Vector((0, 1, 0))
        if M is not None:
            dir_front = (M.to_3x3() @ dir_front).normalized()
        faces = []
        for k in range(len(out2) - 1):
            a, b, ia, ib = out2[k], out2[k + 1], inn[k], inn[k + 1]
            if front:
                faces.append(self.face([P(a, y1), P(b, y1), P(ib, y1), P(ia, y1)], mat, toward=dir_front))
            if back:
                faces.append(self.face([P(a, y0), P(b, y0), P(ib, y0), P(ia, y0)], mat, toward=-dir_front))
            # inner surface faces the interior (right of travel)
            d = (b - a).normalized()
            into = V3(d.y, 0, -d.x)  # right normal in the x-z plane
            if M is not None:
                into = M.to_3x3() @ into
            faces.append(self.face([P(ia, y0), P(ib, y0), P(ib, y1), P(ia, y1)], inner_mat or mat, toward=into))
        return faces

    def plate(self, corners, mat, normal):
        return self.face(corners, mat, toward=normal)

    # --- post-processing -----------------------------------------------------

    def inset(self, face, thickness, depth, inner_mat=None, rim_mat=None):
        """Insets a face (negative depth = recess). Returns the inner face."""
        res = bmesh.ops.inset_individual(self.bm, faces=[face], thickness=thickness, depth=depth,
                                         use_even_offset=True)
        if rim_mat is not None:
            for f in res['faces']:
                f.material_index = self.mi(rim_mat)
        if inner_mat is not None:
            face.material_index = self.mi(inner_mat)
        return face

    def merge(self, other):
        """Appends another Builder's geometry."""
        remap = [self.mi(m) for m in other.mats]
        me = bpy.data.meshes.new('_tmp_merge')
        other.bm.to_mesh(me)
        before = len(self.bm.faces)
        self.bm.from_mesh(me)
        self.bm.faces.ensure_lookup_table()
        for f in list(self.bm.faces)[before:]:
            f.material_index = remap[f.material_index] if f.material_index < len(remap) else 0
        bpy.data.meshes.remove(me)

    def to_object(self, name, collection, bevel=0.0, segments=2, smooth=True, angle=40.0, sharp_angle=None):
        me = bpy.data.meshes.new(name)
        self.bm.normal_update()
        self.bm.to_mesh(me)
        self.bm.free()
        for m in self.mats:
            me.materials.append(material(m))
        obj = bpy.data.objects.new(name, me)
        collection.objects.link(obj)
        if bevel > 0:
            mod = obj.modifiers.new('Bevel', 'BEVEL')
            mod.width = bevel
            mod.segments = segments
            mod.limit_method = 'ANGLE'
            mod.angle_limit = math.radians(angle)
            mod.use_clamp_overlap = True
            mod.harden_normals = True
            wn = obj.modifiers.new('WeightedNormal', 'WEIGHTED_NORMAL')
            wn.keep_sharp = True
            wn.weight = 50
        if sharp_angle is not None:
            me.shade_smooth()
            me.set_sharp_from_angle(angle=math.radians(sharp_angle))
        elif smooth and bevel > 0:
            me.shade_smooth()
        else:
            me.shade_flat()
        return obj


# ---------------------------------------------------------------------------
# Text decals (stencil labels) using Bahnschrift, the DIN-like Windows font
# ---------------------------------------------------------------------------

_FONT_PATHS = ['C:/Windows/Fonts/bahnschrift.ttf']


def _font():
    f = bpy.data.fonts.get('BH_Stencil')
    if f:
        return f
    for p in _FONT_PATHS:
        try:
            f = bpy.data.fonts.load(p)
            f.name = 'BH_Stencil'
            return f
        except Exception:  # noqa: BLE001
            pass
    return None


def text(builder, s, size, M, mat, align='CENTER', extrude=0.0):
    """Adds flat text lying in the local XY plane (facing +Z) transformed by M."""
    cu = bpy.data.curves.new('_tmp_text', 'FONT')
    cu.body = s
    cu.size = size
    cu.align_x = align
    cu.align_y = 'CENTER'
    cu.extrude = extrude
    f = _font()
    if f:
        cu.font = f
    ob = bpy.data.objects.new('_tmp_text', cu)
    bpy.context.scene.collection.objects.link(ob)
    dg = bpy.context.evaluated_depsgraph_get()
    me = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
    tmp = bmesh.new()
    tmp.from_mesh(me)
    bmesh.ops.transform(tmp, matrix=M, verts=tmp.verts)
    b2 = Builder()
    b2.bm.free()
    b2.bm = tmp
    b2.mats = [mat]
    for fc in tmp.faces:
        fc.material_index = 0
    builder.merge(b2)
    tmp.free()
    bpy.data.objects.remove(ob)
    bpy.data.curves.remove(cu)
    bpy.data.meshes.remove(me)


def text_at(builder, s, size, origin, right, up, mat, align='CENTER'):
    """Text centred at origin, reading along `right`, with `up` as its up direction.
    The text faces right x up."""
    text(builder, s, size, frame_matrix(Vector(origin), Vector(right), Vector(up)), mat, align=align)


def frame_matrix(origin, u, v):
    """Matrix mapping local X->u, Y->v, Z->u x v at origin (u, v unit vectors)."""
    u = u.normalized()
    v = v.normalized()
    n = u.cross(v).normalized()
    M = Matrix((
        (u.x, v.x, n.x, origin.x),
        (u.y, v.y, n.y, origin.y),
        (u.z, v.z, n.z, origin.z),
        (0, 0, 0, 1)))
    return M


# ---------------------------------------------------------------------------
# Pieces and collections
# ---------------------------------------------------------------------------


def collection(name, parent=None):
    c = bpy.data.collections.get(name)
    if c is None:
        c = bpy.data.collections.new(name)
        (parent or bpy.context.scene.collection).children.link(c)
    return c


def clear_collection(c):
    for o in list(c.objects):
        data = o.data
        bpy.data.objects.remove(o, do_unlink=True)
        if data is not None and getattr(data, 'users', 1) == 0:
            if isinstance(data, bpy.types.Mesh):
                bpy.data.meshes.remove(data)
            elif isinstance(data, bpy.types.Light):
                bpy.data.lights.remove(data)


class Piece:
    """One exportable kit piece: several mesh parts plus optional lights."""

    def __init__(self, name, seed=None):
        self.name = name
        self.main = Builder()    # big surfaces, 1 cm bevel
        self.fine = Builder()    # small parts, 3 mm bevel
        self.flat = Builder()    # decals, emissive, glass: no bevel
        self.round = Builder()   # pipes and cylinders: smooth, sharp only at mitres
        self.lights = []         # (name, location, power W, radius, colour)
        self.extra_objects = []  # (name, Builder, bevel) separately exported nodes
        self.markers = []        # (name, location, direction): empties named <piece>_fx_<name>
        self.rng = random.Random(seed if seed is not None else name)

    def light(self, loc, power=120.0, radius=0.12, color=(1.0, 0.97, 0.93), name=None):
        self.lights.append((name or f'L{len(self.lights)}', Vector(loc), power, radius, color))

    def marker(self, name, loc, direction=(0, 0, -1)):
        """An effect anchor for the engine (sparks, steam, the muzzle): an empty whose
        local +Y points along `direction`. Exported as glTF node <piece>_fx_<name>."""
        self.markers.append((name, Vector(loc), Vector(direction).normalized()))

    def finish(self, parent):
        coll = collection('KIT_' + self.name, parent)
        clear_collection(coll)
        objs = []
        for suffix, b, bev, segs in (('body', self.main, 0.01, 2), ('detail', self.fine, 0.003, 2),
                                     ('decal', self.flat, 0.0, 0)):
            if len(b.bm.faces):
                objs.append(b.to_object(f'{self.name}_{suffix}', coll, bevel=bev, segments=segs))
            else:
                b.bm.free()
        if len(self.round.bm.faces):
            objs.append(self.round.to_object(f'{self.name}_pipes', coll, sharp_angle=40.0))
        else:
            self.round.bm.free()
        for nm, b, bev in self.extra_objects:
            objs.append(b.to_object(f'{self.name}_{nm}', coll, bevel=bev, segments=2))
        for nm, loc, power, radius, color in self.lights:
            ld = bpy.data.lights.new(f'{self.name}_{nm}', 'POINT')
            ld.energy = power
            ld.shadow_soft_size = radius
            ld.color = color
            lo = bpy.data.objects.new(f'{self.name}_{nm}', ld)
            lo.location = loc
            coll.objects.link(lo)
        for nm, loc, d in self.markers:
            em = bpy.data.objects.new(f'{self.name}_fx_{nm}', None)
            em.empty_display_type = 'SINGLE_ARROW'
            em.empty_display_size = 0.15
            em.location = loc
            em.rotation_mode = 'QUATERNION'
            em.rotation_quaternion = d.to_track_quat('Y', 'Z')
            coll.objects.link(em)
        return coll


def sphere(b, center, r, mat, seg=12, rings=6, M=None):
    """A UV sphere added to Builder b."""
    prof = [(0.0, -r)]
    for i in range(1, rings):
        a = -math.pi / 2 + math.pi * i / rings
        prof.append((r * math.cos(a), r * math.sin(a)))
    prof.append((0.0, r))
    T = Matrix.Translation(Vector(center))
    if M is not None:
        T = M @ T
    b.lathe(prof, mat, seg=seg, M=T)
