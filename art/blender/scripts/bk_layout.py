"""Bulkhead kit: assembles pieces into layouts (test corridor, showcase) using
collection instances, exactly the way the engine will place them: cell centres
4 m apart, rotations in 90 degree steps, doors on cell edges."""

import math

import bpy

from bk_core import collection

CELL = 4.0
ROT = {'N': 0, 'W': 90, 'S': 180, 'E': 270}  # canonical north turned to face this side


def place(coll_name, piece, x, y, rot_deg=0.0, z=0.0, name=None, offset=(0.0, 0.0)):
    src = bpy.data.collections.get('KIT_' + piece)
    if src is None:
        raise KeyError('missing kit piece ' + piece)
    parent = bpy.data.collections[coll_name]
    e = bpy.data.objects.new(name or f'{coll_name}_{piece}_{x}_{y}', None)
    e.instance_type = 'COLLECTION'
    e.instance_collection = src
    e.location = (x * CELL + offset[0], y * CELL + offset[1], z)
    e.rotation_euler = (0, 0, math.radians(rot_deg))
    e.empty_display_size = 0.5
    parent.objects.link(e)
    return e


def door(coll_name, piece, x, y, side, open_amount=0.0):
    """A door on the edge `side` of cell (x, y). Door pieces are modelled on an edge
    at local y = 0 with the passage along local Y."""
    dx, dy = {'N': (0, 2), 'S': (0, -2), 'E': (2, 0), 'W': (-2, 0)}[side]
    rot = 0 if side in 'NS' else 90
    return place(coll_name, piece, x, y, rot, offset=(dx, dy), name=f'{coll_name}_{piece}_{x}_{y}_{side}')


def reset(coll_name):
    c = collection(coll_name)
    for o in list(c.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    return c


def test_corridor(with_doors=True, with_room=True):
    """The orchestrator's check: straights, a junction, a corner, a door, looked at
    from 1.7 m. Cells (x east, y north):

        (0,4)(1,4)   room 2x2 behind a sliding door on the north edge of (0,3)
        (0,3)        straight
        (0,2)--(1,2) tee, branch east into a corner turning north, ending at (1,3)
        (0,1)        straight
        (0,0)        straight   <- camera
        (0,-1)       dead end
    """
    c = 'TEST_CORRIDOR'
    reset(c)
    place(c, 'corridor_end', 0, -1, ROT['N'])
    place(c, 'corridor_straight', 0, 0)
    place(c, 'corridor_straight', 0, 1)
    place(c, 'corridor_tee', 0, 2)
    place(c, 'corridor_corner', 1, 2, 90)       # open W and N
    place(c, 'corridor_end', 1, 3, 180)         # open S
    place(c, 'corridor_straight', 0, 3)
    if with_doors:
        door(c, 'door_frame', 0, 3, 'N')
        door(c, 'door_sliding', 0, 3, 'N')
    if with_room:
        room_2x2(c, 0, 4)
        dress_room(c)
    return c


def dress_room(c):
    """Props in the test room (world metres; room spans x -2..6, y 14..22)."""
    def prop(piece, x, y, rot=0.0, z=0.0, tag=''):
        e = place(c, piece, 0, 0, rot, z=z, name=f'{c}_{piece}{tag}', offset=(x, y))
        return e
    prop('crate_medium', 4.55, 15.35, 8)
    prop('crate_small', 4.45, 15.3, -12, z=0.8)
    prop('crate_small', 3.55, 15.25, 25, tag='_2')
    prop('crate_long', -0.75, 20.7, 90)
    prop('barrel_red', 5.05, 20.85)
    prop('barrel_blue', 4.35, 21.15, 40)
    prop('barrel_blue', 5.25, 20.15, 75, tag='_2')
    prop('canister', -1.15, 15.0)
    prop('console', 2.0, 18.6, 180)


def room_2x2(c, x0, y0, door_cell=(0, 0), door_side='S'):
    """A 2x2 room whose south-west cell is (x0, y0); one door on the south wall."""
    for ix in range(2):
        for iy in range(2):
            x, y = x0 + ix, y0 + iy
            place(c, 'room_floor', x, y)
            place(c, 'room_ceiling', x, y)
            walls = []
            if iy == 0:
                walls.append('S')
            if iy == 1:
                walls.append('N')
            if ix == 0:
                walls.append('W')
            if ix == 1:
                walls.append('E')
            for s in walls:
                piece = 'room_wall_door' if ((ix, iy) == door_cell and s == door_side) else \
                    ('room_wall_a' if (ix + iy + len(s)) % 2 else 'room_wall_b')
                # room walls are modelled on the south edge of the cell (facing north)
                place(c, piece, x, y, {'S': 0, 'E': 90, 'N': 180, 'W': 270}[s])
    # corner posts (modelled at the south-west corner)
    for (ix, iy), rot in (((0, 0), 0), ((1, 0), 90), ((1, 1), 180), ((0, 1), 270)):
        place(c, 'room_corner', x0 + ix, y0 + iy, rot)
