"""Bulkhead kit: derelict dressing (round 6).

Every piece here sits in a straight corridor cell, modelled in the corridor's canonical
orientation (travel along Blender Y, walls at x = +-1.2), origin at the cell centre on
the floor. The engine places them with the same transform as the corridor piece (and
turns wall pieces 180 degrees to use the other wall).

  pipes_low     a pipe bundle crossing the corridor; its underside is at 1.30 m, so the
                player has to crouch. Collision: y 0.65..1.10, below 1.30 m is free.
  pipe_burst    a red coolant pipe on the east wall, torn open; fx_steam marker at the
                torn end
  debris        a fallen ceiling panel, chunks and a bent conduit along the east wall
  beacon        a red emergency beacon high on the east wall, with its own red light

The broken-ceiling corridor (corridor_straight_broken) is in bk_corridor.py.
"""

import math

from mathutils import Matrix, Vector

from bk_core import V3, Piece, sphere, text_at

W = 1.2
CEIL = 2.85
PIPE_Y = 0.88      # centre line of the low bundle
PIPE_BOTTOM = 1.30


def build_pipes_low(parent):
    pc = Piece('pipes_low')
    y = PIPE_Y
    big_r = 0.11
    zc = PIPE_BOTTOM + big_r
    xa, xb = -W - 0.05, W + 0.05
    # main pipe, yellow with black hazard sleeves so it reads from a distance
    pc.round.cyl(V3(xa, y, zc), V3(xb, y, zc), big_r, 'PipeYellow', seg=24, caps=(False, False))
    for k in range(9):
        x = -1.0 + k * 0.25
        pc.round.cyl(V3(x - 0.05, y, zc), V3(x + 0.05, y, zc), big_r + 0.004, 'HazardBlack', seg=24)
    # two smaller pipes behind and above it
    pc.round.cyl(V3(xa, y + 0.16, zc + 0.2), V3(xb, y + 0.16, zc + 0.2), 0.07, 'PipeBlue', seg=20, caps=(False, False))
    pc.round.cyl(V3(xa, y - 0.17, zc + 0.08), V3(xb, y - 0.17, zc + 0.08), 0.05, 'DarkSteel', seg=16,
                 caps=(False, False))
    # wall flanges where they enter the walls
    for s in (-1, 1):
        x = s * (W - 0.03)
        for py, pz, r in ((y, zc, big_r), (y + 0.16, zc + 0.2, 0.07), (y - 0.17, zc + 0.08, 0.05)):
            pc.fine.cyl(V3(x - 0.025, py, pz), V3(x + 0.025, py, pz), r + 0.03, 'Gunmetal', seg=20)
            for k in range(6):
                a = k * math.pi / 3
                pc.fine.cyl(V3(x - 0.035, py + math.cos(a) * (r + 0.018), pz + math.sin(a) * (r + 0.018)),
                            V3(x + 0.035, py + math.cos(a) * (r + 0.018), pz + math.sin(a) * (r + 0.018)), 0.006,
                            'Steel', seg=6)
    # hangers from the ceiling holding the bundle up
    for x in (-0.5, 0.5):
        for py in (y - 0.17, y + 0.16):
            pc.fine.cyl(V3(x, py, CEIL), V3(x, py, zc + 0.12), 0.01, 'DarkSteel', seg=8)
        pc.fine.box((x - 0.02, y - 0.23, zc + 0.1), (x + 0.02, y + 0.23, zc + 0.13), 'Gunmetal')
    # warning plates on both faces of the big pipe
    for side, right in ((-1, V3(1, 0, 0)), (1, V3(-1, 0, 0))):
        py = y + side * (big_r + 0.012)
        pc.fine.box((-0.33, py - 0.006, zc - 0.05), (0.33, py + 0.006, zc + 0.05), 'HazardBlack')
        text_at(pc.flat, 'LOW CLEARANCE', 0.055, V3(0, py + side * 0.0075, zc), right, V3(0, 0, 1), 'PaintYellow')
    return pc.finish(parent)


def build_pipe_burst(parent):
    pc = Piece('pipe_burst')
    r = 0.065
    x = W - 0.12
    y = 0.35
    # vertical riser from the floor chamfer up to the upper chamfer, broken at mid height
    pc.round.cyl(V3(x, y, 0.5), V3(x, y, 1.3), r, 'PipeRed', seg=20, caps=(False, False))
    pc.round.cyl(V3(x, y, 1.62), V3(x, y, 2.2), r, 'PipeRed', seg=20, caps=(False, False))
    for z in (0.62, 2.05):
        pc.fine.box((x - 0.02, y - 0.09, z - 0.03), (W, y + 0.09, z + 0.03), 'Gunmetal')
    # the torn ends: jagged petals of pipe wall bent outward
    rng = pc.rng
    for z0, up, n in ((1.3, 1, 9), (1.62, -1, 8)):
        for k in range(n):
            a = 2 * math.pi * (k + rng.uniform(-0.2, 0.2)) / n
            d = V3(math.cos(a), math.sin(a), 0)
            length = rng.uniform(0.04, 0.11)
            bend = rng.uniform(0.25, 0.9)
            base = V3(x, y, z0) + d * r
            tip = base + (V3(0, 0, up) * math.cos(bend) + d * math.sin(bend)) * length
            side = V3(-d.y, d.x, 0) * rng.uniform(0.018, 0.03)
            pc.fine.face([base - side, base + side, tip], 'PipeRed', toward=d)
            pc.fine.face([base - side, base + side, tip], 'Rust', toward=-d)
    # a broken-off branch pointing into the corridor, where the steam comes out
    a0 = V3(x, y, 1.45)
    pc.round.cyl(V3(W, y - 0.25, 1.45), V3(x - 0.08, y - 0.25, 1.45), 0.045, 'PipeRed', seg=16, caps=(True, False))
    end = V3(x - 0.08, y - 0.25, 1.45)
    for k in range(7):
        a = 2 * math.pi * k / 7
        d = V3(0, math.cos(a), math.sin(a))
        base = end + d * 0.045
        tip = base + (V3(-1, 0, 0) * 0.6 + d * 0.5).normalized() * rng.uniform(0.03, 0.07)
        side = V3(0, -d.z, d.y) * 0.02
        pc.fine.face([base - side, base + side, tip], 'PipeRed', toward=d)
        pc.fine.face([base - side, base + side, tip], 'Rust', toward=-d)
    pc.marker('steam', end + V3(-0.02, 0, 0), (-1, -0.15, 0.1))
    # valve wheel on the riser
    pc.fine.ring(a0 + V3(-r - 0.03, 0, 0.45), V3(1, 0, 0), 0.09, 0.012, 'PipeYellow', seg=20, tseg=6)
    pc.fine.cyl(a0 + V3(-r - 0.03, 0, 0.45), a0 + V3(0, 0, 0.45), 0.012, 'Steel', seg=8)
    # rust and water stains running down the wall below the break
    for k in range(5):
        yy = y - 0.25 + k * 0.12 + rng.uniform(-0.03, 0.03)
        top = rng.uniform(1.2, 1.4)
        w = rng.uniform(0.03, 0.07)
        pc.flat.face([V3(W - 0.003, yy - w, 0.47), V3(W - 0.003, yy + w, 0.47), V3(W - 0.003, yy + w * 0.4, top),
                      V3(W - 0.003, yy - w * 0.4, top)], 'Rust', toward=V3(-1, 0, 0))
    # a puddle-dark patch on the floor under it
    pc.flat.face([V3(0.5, y - 0.6, 0.004), V3(0.88, y - 0.6, 0.004), V3(0.88, y + 0.3, 0.004),
                  V3(0.5, y + 0.3, 0.004)], 'Soot', toward=V3(0, 0, 1))
    return pc.finish(parent)


def build_debris(parent):
    pc = Piece('debris')
    rng = pc.rng
    # a ceiling panel that fell and lies tilted against the east wall
    R = Matrix.Rotation(math.radians(-58), 3, 'Y') @ Matrix.Rotation(math.radians(6), 3, 'X')
    pc.main.obox(V3(0.86, -0.15, 0.32), (0.58, 0.84, 0.024), 'PanelLight', rot=R, mats={'-z': 'PanelGrey'})
    # chunks of panel, brackets and a bent conduit on the floor
    for k in range(9):
        c = V3(rng.uniform(0.45, 1.0), rng.uniform(-0.9, 0.9), 0.0)
        s = (rng.uniform(0.06, 0.22), rng.uniform(0.05, 0.18), rng.uniform(0.012, 0.05))
        c.z = s[2] * 0.5
        rot = Matrix.Rotation(rng.uniform(0, math.pi), 3, 'Z') @ Matrix.Rotation(rng.uniform(-0.3, 0.3), 3, 'X')
        mat = rng.choice(['PanelGrey', 'PanelLight', 'DarkSteel', 'Gunmetal'])
        pc.fine.obox(c, s, mat, rot=rot)
    pts = [V3(0.95, -0.85, 0.03), V3(0.75, -0.45, 0.03), V3(0.62, -0.1, 0.05), V3(0.7, 0.35, 0.03),
           V3(0.58, 0.7, 0.03)]
    pc.round.tube(pts, 0.022, 'DarkSteel', seg=10)
    # dust and soot on the floor round it
    pc.flat.face([V3(0.42, -1.0, 0.003), V3(1.05, -1.0, 0.003), V3(1.05, 1.0, 0.003), V3(0.42, 1.0, 0.003)], 'Soot',
                 toward=V3(0, 0, 1))
    return pc.finish(parent)


def build_beacon(parent):
    pc = Piece('beacon')
    z = 2.08
    pc.fine.box((W - 0.03, -0.12, z - 0.12), (W, 0.12, z + 0.12), 'Gunmetal')
    pc.fine.cyl(V3(W - 0.03, 0, z), V3(W - 0.07, 0, z), 0.075, 'DarkSteel', seg=20)
    # the lamp: a red emissive dome facing into the corridor
    M = Matrix.Translation(V3(W - 0.07, 0, z)) @ Matrix.Rotation(math.radians(-90), 4, 'Y')
    pc.flat.lathe([(0.0, 0.0), (0.06, 0.0), (0.06, 0.03), (0.05, 0.065), (0.03, 0.085), (0.0, 0.09)], 'LightRed',
                  seg=20, M=M)
    # protective cage
    for k in range(4):
        a = k * math.pi / 4
        d = V3(0, math.cos(a), math.sin(a))
        pc.fine.tube([V3(W - 0.07, 0, z) + d * 0.075, V3(W - 0.13, 0, z) + d * 0.07, V3(W - 0.17, 0, z)], 0.004,
                     'Steel', seg=6)
    sphere(pc.fine, V3(W - 0.172, 0, z), 0.008, 'Steel', seg=8, rings=4)
    pc.light((W - 0.25, 0, z), power=70.0, radius=0.06, color=(1.0, 0.12, 0.05))
    return pc.finish(parent)


def build_derelict(parent):
    build_pipes_low(parent)
    build_pipe_burst(parent)
    build_debris(parent)
    build_beacon(parent)
