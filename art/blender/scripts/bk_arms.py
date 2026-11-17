"""Bulkhead kit: the player's arms and the weapon effects (round 7).

arms           Armoured work gloves and suit sleeves in a two-handed grip on the BH-9.
               Modelled in the handgun's space (origin at the top of the grip, forward +Y,
               up +Z), so the engine draws it with exactly the gun's transform.
               Right hand: palm on the right grip panel, three fingers round the front
               strap, index finger on the trigger, thumb high on the left side.
               Left hand: cups the right hand from the left, four fingers round the front
               of the right hand's fingers, thumb forward along the frame.
muzzle_flash   Crossed petals and a core at the muzzle (gun space), shown for a few frames.
bolt           The energy bolt, a 0.5 m glowing capsule along +Y (the engine stretches it).
spark          One spark streak, 7 cm along +Y.
scorch         A soot mark in the XY plane facing +Z, for where a bolt hits.
"""

import math

from mathutils import Matrix, Vector

from bk_core import V3, Builder, Piece, sphere

TAN_RAKE = math.tan(math.radians(16))
RAKE = Matrix.Rotation(math.radians(-16), 3, 'X')


def front_y(z):
    """y of the grip's front strap at height z."""
    return -0.026 + (0.012 + z) * TAN_RAKE


def back_y(z):
    return -0.062 + (0.012 + z) * TAN_RAKE


def finger(b, armor, pts, r, knuckle_plates=True):
    """A gloved finger along pts (knuckle first, tip last), tapering, with round joints
    and armour plates on the back of the first two segments."""
    pts = [Vector(p) for p in pts]
    n = len(pts)
    for i in range(n - 1):
        ra = r * (1.0 - 0.12 * i / (n - 1))
        rb = r * (1.0 - 0.12 * (i + 1) / (n - 1))
        b.cyl(pts[i], pts[i + 1], ra, 'Glove', seg=10, caps=(False, False), r1=rb)
        sphere(b, pts[i], ra * 1.02, 'Glove', seg=10, rings=5)
    sphere(b, pts[-1], r * 0.88, 'Glove', seg=10, rings=5)
    if knuckle_plates:
        for i in range(min(2, n - 1)):
            a, c = pts[i], pts[i + 1]
            d = (c - a)
            mid = (a + c) * 0.5
            # "back" of the finger: away from the grip's centre line
            out = Vector((mid.x, 0.0, 0.0))
            out = (out - d * (out.dot(d) / d.length_squared))
            if out.length < 1e-6:
                out = Vector((0, 0, 1))
            out.normalize()
            yax = d.normalized()
            xax = yax.cross(out).normalized()
            rot = Matrix((xax, yax, out)).transposed()  # plate: long along the finger, thin side out
            armor.obox(mid + out * r * 0.9, (r * 1.5, d.length * 0.8, r * 0.45), 'GloveArmor', rot=rot)


def build_arms(parent):
    pc = Piece('arms')
    hand = Builder()    # glove: smooth-ish, small bevel
    armor = Builder()   # plates
    sleeve = Builder()  # suit

    # ---- right hand ----------------------------------------------------------
    hand.obox(V3(0.035, -0.058, -0.062), (0.026, 0.072, 0.088), 'Glove', rot=RAKE)            # palm on the panel
    hand.obox(V3(0.014, -0.094, -0.066), (0.034, 0.03, 0.082), 'Glove', rot=RAKE)             # heel round the back strap
    hand.obox(V3(0.03, -0.098, -0.092), (0.046, 0.04, 0.05), 'Glove', rot=RAKE)               # toward the wrist
    armor.obox(V3(0.0495, -0.058, -0.056), (0.006, 0.05, 0.058), 'GloveArmor', rot=RAKE)    # back-of-hand plate
    for z, r in ((-0.045, 0.0095), (-0.066, 0.0092), (-0.087, 0.0082)):
        yf = front_y(z)
        finger(hand, armor, [(0.037, yf - 0.009, z), (0.031, yf + 0.008, z), (0.01, yf + 0.0145, z),
                             (-0.014, yf + 0.0105, z), (-0.0285, yf - 0.004, z)], r)
        armor.obox(V3(0.043, yf - 0.008, z), (0.01, 0.018, 0.017), 'GloveArmor')            # knuckle guard
    # index finger on the trigger
    finger(hand, armor, [(0.036, -0.03, -0.024), (0.027, -0.006, -0.027), (0.014, 0.011, -0.031),
                         (0.004, 0.0135, -0.037)], 0.0092)
    armor.obox(V3(0.043, -0.03, -0.024), (0.01, 0.018, 0.017), 'GloveArmor')
    # thumb over the back of the grip to the left side, high, pointing forward
    finger(hand, armor, [(0.014, -0.094, -0.03), (-0.004, -0.083, -0.01), (-0.026, -0.058, -0.004),
                         (-0.0295, -0.032, -0.003), (-0.029, -0.012, -0.003)], 0.0105)

    # ---- left hand ------------------------------------------------------------
    hand.obox(V3(-0.043, -0.05, -0.078), (0.026, 0.074, 0.084), 'Glove', rot=RAKE)
    hand.obox(V3(-0.05, -0.088, -0.1), (0.044, 0.04, 0.05), 'Glove', rot=RAKE)
    armor.obox(V3(-0.0575, -0.05, -0.074), (0.006, 0.05, 0.056), 'GloveArmor', rot=RAKE)
    for z, r in ((-0.062, 0.0098), (-0.081, 0.0098), (-0.099, 0.0094), (-0.116, 0.0084)):
        yf = front_y(z)
        finger(hand, armor, [(-0.046, yf - 0.01, z), (-0.041, yf + 0.018, z), (-0.018, yf + 0.0325, z),
                             (0.012, yf + 0.0345, z), (0.036, yf + 0.022, z), (0.046, yf + 0.004, z)], r)
        armor.obox(V3(-0.054, yf - 0.01, z), (0.01, 0.018, 0.017), 'GloveArmor')
    # thumb forward along the frame, under the right thumb
    finger(hand, armor, [(-0.052, -0.072, -0.048), (-0.04, -0.045, -0.026), (-0.031, -0.012, -0.018),
                         (-0.0295, 0.016, -0.017)], 0.0108)

    # ---- wrists and sleeves -------------------------------------------------------
    for w0, w1, s1, sgn in ((V3(0.032, -0.108, -0.1), V3(0.05, -0.155, -0.13), V3(0.15, -0.44, -0.29), 1),
                            (V3(-0.052, -0.098, -0.108), V3(-0.08, -0.145, -0.135), V3(-0.25, -0.43, -0.29), -1)):
        armor.cyl(w0, w1, 0.031, 'GloveArmor', seg=20)                                       # armoured cuff
        armor.cyl(w0 + (w1 - w0) * 0.15, w0 + (w1 - w0) * 0.3, 0.0335, 'DarkSteel', seg=20)
        sleeve.cyl(w1 - (w1 - w0) * 0.2, s1, 0.039, 'SuitFabric', seg=20, r1=0.05)            # forearm sleeve
        d = (s1 - w1).normalized()
        up = Vector((0, 0, 1))
        side = d.cross(up).normalized()
        top = side.cross(d).normalized()
        rot = Matrix((side, d, top)).transposed()
        mid = w1 + (s1 - w1) * 0.45
        sleeve.obox(mid + top * 0.042 + side * sgn * 0.01, (0.05, (s1 - w1).length * 0.45, 0.012), 'SuitPlate',
                    rot=rot)
        sleeve.cyl(w1 + d * 0.02, w1 + d * 0.035, 0.043, 'DarkSteel', seg=20)                 # sleeve band

    pc.extra_objects.append(('gloves', hand, 0.007))   # big bevel: padded, rounded glove
    pc.extra_objects.append(('armor', armor, 0.0018))
    pc.extra_objects.append(('sleeves', sleeve, 0.004))
    return pc.finish(parent)


def build_muzzle_flash(parent):
    pc = Piece('muzzle_flash')
    o = V3(0, 0.168, 0.022)
    for k in range(3):
        a = k * math.pi / 3
        side = V3(math.cos(a), 0, math.sin(a))
        pts = [o + side * 0.004, o + side * 0.03 + V3(0, 0.04, 0), o + V3(0, 0.12, 0), o - side * 0.03 + V3(0, 0.04, 0),
               o - side * 0.004]
        nrm = side.cross(V3(0, 1, 0))
        pc.flat.face(pts, 'LightFlash', toward=nrm)
        pc.flat.face(pts, 'LightFlash', toward=-nrm)
    sphere(pc.flat, o + V3(0, 0.012, 0), 0.016, 'LightFlash', seg=10, rings=5)
    return pc.finish(parent)


def build_bolt(parent):
    pc = Piece('bolt')
    a, b = V3(0, -0.25, 0), V3(0, 0.25, 0)
    pc.flat.cyl(a, b, 0.011, 'LightBolt', seg=10, caps=(False, False))
    sphere(pc.flat, a, 0.011, 'LightBolt', seg=10, rings=5)
    sphere(pc.flat, b, 0.011, 'LightBolt', seg=10, rings=5)
    return pc.finish(parent)


def build_spark(parent):
    pc = Piece('spark')
    pc.flat.box((-0.0035, -0.035, -0.0035), (0.0035, 0.035, 0.0035), 'LightSpark')
    return pc.finish(parent)


def build_scorch(parent):
    pc = Piece('scorch')
    rng = pc.rng
    pts = []
    n = 16
    for k in range(n):
        a = 2 * math.pi * k / n
        r = 0.13 * rng.uniform(0.7, 1.25)
        pts.append(V3(math.cos(a) * r, math.sin(a) * r, 0.0))
    pc.flat.face(pts, 'Soot', toward=V3(0, 0, 1))
    return pc.finish(parent)


def build_all_fx(parent):
    build_arms(parent)
    build_muzzle_flash(parent)
    build_bolt(parent)
    build_spark(parent)
    build_scorch(parent)
