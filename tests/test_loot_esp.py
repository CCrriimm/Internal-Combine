"""Ground-loot reader and packet trailer, without loading the game.

Exercises the exact owner-type allowlist, the Supply is_open rule, the range
filter and the trailer layout that the overlay parses.
"""
import ast
import math
import struct
from pathlib import Path
import types
import unittest


SOURCE = Path(__file__).resolve().parents[1] / 'pyexec/bs_runtime.py'
TEXT = SOURCE.read_text('utf-8-sig')


def functions(names, namespace):
    nodes = [node for node in ast.parse(TEXT).body
             if isinstance(node, (ast.FunctionDef, ast.Assign))]
    wanted = []
    for node in nodes:
        if isinstance(node, ast.FunctionDef) and node.name in names:
            wanted.append(node)
    if {node.name for node in wanted} != names:
        raise AssertionError('Missing runtime function')
    exec(compile(ast.Module(body=wanted, type_ignores=[]), str(SOURCE), 'exec'), namespace)
    return namespace


def module_constant(name):
    """Evaluate a module-level constant assignment only (no other runtime code)."""
    for node in ast.parse(TEXT).body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == name for t in node.targets):
            namespace = {}
            exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), 'exec'), namespace)
            return namespace[name]
    raise AssertionError('missing constant ' + name)


class CombatItemEntity:
    def __init__(self, position, eid=1):
        self.position = position
        self.id = eid


class Supply:
    def __init__(self, position, is_open, eid=2):
        self.position = position
        self.is_open = is_open
        self.id = eid


class Player:
    def __init__(self, position, eid=3):
        self.position = position
        self.id = eid
        self.IsCombatAvatar = True


class LootReaderTests(unittest.TestCase):
    def setUp(self):
        self.loot = [CombatItemEntity((0, 0, 20), 11),
                     CombatItemEntity((0, 0, 40), 12),
                     Supply((0, 0, 30), False, 13),
                     Supply((0, 0, 15), True, 14),        # opened -> hidden
                     CombatItemEntity((0, 0, 900), 15),   # out of range
                     Player((0, 0, 10), 16)]              # not loot
        self.ns = {
            'time': types.SimpleNamespace(time=lambda: 100.0),
            'math': math,
            'genv': types.SimpleNamespace(space=object()),
            'MAX_ENTITIES': 128,
            'LOOT_MAX': module_constant('LOOT_MAX'),
            'GROUND_LOOT_TYPES': module_constant('GROUND_LOOT_TYPES'),
            '_collect_space_entities': lambda space: self.loot,
            '_cam_dir': lambda cam: (0, 0, 1),
            '_vec': lambda v: None if v is None else tuple(float(x) for x in v),
            '_sub': lambda a, b: tuple(x - y for x, y in zip(a, b)),
            '_len': lambda v: math.sqrt(sum(x * x for x in v)),
            '_dot': lambda a, b: sum(x * y for x, y in zip(a, b)),
            '_matrix_world_to_screen': lambda p: (0.5, 0.5) if p[2] < 200 else None,
            '_call_attr': lambda obj, names: next(
                (getattr(obj, n) for n in names if getattr(obj, n, None) is not None), None),
            '_loot_cache': None,
        }
        functions({'_collect_ground_loot', '_ground_loot_kind', '_owner_type_name',
                   '_loot_position'}, self.ns)
        # The scan writes this module global through its own namespace.
        self.ns['_loot_cache'] = None

    def scan(self, max_dist=120.0):
        self.ns['_loot_cache'] = None
        return self.ns['_collect_ground_loot'](object(), (0.0, 0.0, 0.0), max_dist)

    def test_only_captured_owner_types_become_loot(self):
        kinds = {entry[0]: entry[4] for entry in self.scan()}
        self.assertNotIn(16, kinds)                      # player ignored
        self.assertNotIn(15, kinds)                      # out of range
        self.assertNotIn(14, kinds)                      # opened supply hidden
        self.assertEqual(kinds[11], 0)                   # dropped item
        self.assertEqual(kinds[13], 1)                   # closed supply

    def test_sorted_nearest_first_and_distance_is_camera_range(self):
        rows = self.scan()
        self.assertEqual([r[3] for r in rows], sorted(r[3] for r in rows))
        self.assertAlmostEqual(rows[0][3], 20.0)
        self.assertAlmostEqual(rows[-1][3], 40.0)

    def test_opened_supply_with_unknown_flag_is_kept(self):
        # Same captured owner type, but no readable is_open property: unknown is
        # kept rather than treated as an opened container.
        supply = type('Supply', (), {'position': (0, 0, 25), 'id': 21})()
        self.loot = [supply]
        rows = self.scan()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][4], 1)

    def test_behind_camera_and_unprojectable_items_are_dropped(self):
        self.loot = [CombatItemEntity((0, 0, -30), 31),
                     CombatItemEntity((0, 0, 260), 32)]  # screens off in the stub
        self.assertEqual(self.scan(), [])

    def test_scan_is_cached_between_frames(self):
        first = self.scan()
        cached = self.ns['_loot_cache']
        self.assertIsNotNone(cached)
        second = self.ns['_collect_ground_loot'](object(), (0.0, 0.0, 0.0), 120.0)
        self.assertEqual(first, second)

    def test_cache_hits_for_equal_but_distinct_limit_objects(self):
        """Regression: the limit is a fresh float each frame.

        The tick computes `min(500.0, limit)` per frame, so an identity compare
        never matched and the scan re-ran every frame instead of at 4 Hz.
        """
        calls = []
        self.ns['_collect_space_entities'] = lambda space: calls.append(1) or self.loot
        self.ns['_loot_cache'] = None
        self.ns['_collect_ground_loot'](object(), (0.0, 0.0, 0.0), float('120'))
        self.ns['_collect_ground_loot'](object(), (0.0, 0.0, 0.0), float('120.0'))
        self.assertEqual(len(calls), 1)

    def test_changed_limit_rescans(self):
        calls = []
        self.ns['_collect_space_entities'] = lambda space: calls.append(1) or self.loot
        self.ns['_loot_cache'] = None
        self.ns['_collect_ground_loot'](object(), (0.0, 0.0, 0.0), 120.0)
        self.ns['_collect_ground_loot'](object(), (0.0, 0.0, 0.0), 60.0)
        self.assertEqual(len(calls), 2)

    def test_no_space_or_origin_returns_nothing(self):
        self.assertEqual(self.ns['_collect_ground_loot'](object(), None, 100.0), [])
        self.assertEqual(self.ns['_collect_ground_loot'](None, (0, 0, 0), 100.0), [])


class PacketTrailerTests(unittest.TestCase):
    """The trailer the overlay parses must match LOOT_FMT/LOOT_HEAD exactly."""

    def test_layout_constants(self):
        self.assertEqual(module_constant('LOOT_MAX'), 64)
        self.assertEqual(module_constant('LOOT_HEAD'), 4)
        # id + 2 screen floats + distance + kind
        self.assertEqual(struct.calcsize('<Q2ffI'), 24)
        self.assertEqual(module_constant('GROUND_LOOT_TYPES'), ('CombatItemEntity', 'Supply'))

    def test_entity_record_size_matches_overlay(self):
        ent_fmt = module_constant('ENT_FMT')
        self.assertEqual(struct.calcsize(ent_fmt), 288)
        self.assertEqual(struct.calcsize(module_constant('CFG_FMT')), 224)
        self.assertEqual(struct.calcsize(module_constant('STATE_FMT')), 84)
        self.assertEqual(module_constant('VERSION'), 8)

    def test_serializer_appends_counted_trailer(self):
        state_head = struct.calcsize(module_constant('STATE_FMT'))
        ent_size = struct.calcsize(module_constant('ENT_FMT'))
        ns = {
            'struct': struct, 'time': types.SimpleNamespace(time=lambda: 5.0),
            'math': math,
            'STATE_SIZE': 4096, 'STATE_HEAD_SIZE': state_head, 'ENT_SIZE': ent_size,
            'LOOT_MAX': 64, 'LOOT_FMT': '<Q2ffI', 'LOOT_SIZE': 24, 'LOOT_HEAD': 4,
            'MAX_ENTITIES': 128, 'STATE_FMT': module_constant('STATE_FMT'),
            'STATE_MAGIC': 0x42535331, 'VERSION': 8,
            'OPT2_ESP_LOOT': 1 << 1, 'IDX_OPTS': 50,
            '_opt2': lambda cfg, bit: bool(cfg[50] & bit),
            '_state_seq': 0, '_local_hero_id': lambda: 0,
            '_get_cam': lambda: None, '_vec': lambda v: None,
            'genv': types.SimpleNamespace(), 'gui': types.SimpleNamespace(),
            'MUI': None, '_flag': lambda cfg, bit: False,
            'F_RENDER_OCCLUSION': 1 << 11, 'F_TRACK_OCCLUSION': 1 << 14,
            'F_RENDER_FACTION': 1 << 10, 'IDX_RENDER_MAX_DIST': 16,
            'MAX_ENTITIES': 128, '_id_u64': lambda v: 0, '_bytes': lambda b, n: b'',
            '_ename': lambda e: '', '_hp': lambda e: 1.0, '_max_hp': lambda e: 1.0,
            '_armor': lambda e: 0.0, '_max_armor': lambda e: 0.0, '_is_dying': lambda e: False,
            'ENT_FMT': module_constant('ENT_FMT'),
            '_hero_of': lambda e: 4023,
        }
        cfg = [0] * 58
        cfg[16] = 400.0
        cfg[50] = 1 << 1
        functions({'_serialize_frame_data'}, ns)
        loot = [(7, 0.25, 0.75, 33.0, 1, (0, 0, 33)), (8, 0.5, 0.5, 12.0, 0, (0, 0, 12))]
        buf = ns['_serialize_frame_data'](cfg, None, True, (0, 0, 0), (0, 0, 0), [], loot)
        off = state_head
        count = struct.unpack_from('<I', buf, off)[0]
        self.assertEqual(count, 2)
        first = struct.unpack_from('<Q2ffI', buf, off + 4)
        self.assertEqual(first[0], 7)
        self.assertAlmostEqual(first[1], 0.25, places=5)
        self.assertAlmostEqual(first[3], 33.0, places=5)
        self.assertEqual(first[4], 1)
        self.assertEqual(len(buf), off + 4 + 2 * 24)

        # Feature off -> header still present so the reader never guesses.
        cfg[50] = 0
        buf = ns['_serialize_frame_data'](cfg, None, True, (0, 0, 0), (0, 0, 0), [], loot)
        self.assertEqual(struct.unpack_from('<I', buf, state_head)[0], 0)
        self.assertEqual(len(buf), state_head + 4)

    def test_entity_record_carries_hero_id(self):
        """Regression: the player packer must fill every field of ENT_FMT.

        A mismatched argument count raises inside the per-entity try/except, so
        the failure mode is silently publishing zero entities rather than an
        error. Packing one real entity catches it.
        """
        ent_fmt = module_constant('ENT_FMT')
        ent_size = struct.calcsize(ent_fmt)
        v = lambda *a: tuple(float(x) for x in a)
        ns = {
            'struct': struct, 'time': types.SimpleNamespace(time=lambda: 5.0),
            'math': math,
            'STATE_SIZE': 4096, 'STATE_HEAD_SIZE': struct.calcsize(module_constant('STATE_FMT')),
            'ENT_SIZE': ent_size,
            'LOOT_MAX': 64, 'LOOT_FMT': '<Q2ffI', 'LOOT_SIZE': 24, 'LOOT_HEAD': 4,
            'MAX_ENTITIES': 128, 'STATE_FMT': module_constant('STATE_FMT'),
            'STATE_MAGIC': 0x42535331, 'VERSION': 8,
            'OPT2_ESP_LOOT': 1 << 1, 'IDX_OPTS': 50,
            '_opt2': lambda cfg, bit: bool(cfg[50] & bit),
            '_state_seq': 0, '_local_hero_id': lambda: 0,
            '_get_cam': lambda: None, '_vec': lambda vv: None,
            'genv': types.SimpleNamespace(), 'gui': types.SimpleNamespace(),
            'MUI': None, '_flag': lambda cfg, bit: False,
            'F_RENDER_OCCLUSION': 1 << 11, 'F_TRACK_OCCLUSION': 1 << 14,
            'F_RENDER_FACTION': 1 << 10, 'IDX_RENDER_MAX_DIST': 16,
            '_id_u64': lambda vv: 42, '_bytes': lambda b, n: b'\x00' * 0,
            '_ename': lambda e: 'Foe', '_hp': lambda e: 90.0, '_max_hp': lambda e: 100.0,
            '_armor': lambda e: 25.0, '_max_armor': lambda e: 50.0, '_is_dying': lambda e: False,
            'ENT_FMT': ent_fmt,
            '_hero_of': lambda e: 4023,
        }
        functions({'_serialize_frame_data'}, ns)
        cfg = [0] * 58
        cfg[16] = 400.0
        item = (12.0, object(), 99, v(1, 2, 300), v(1, 2, 100), v(0.5, 0.2),
                v(0.5, 0.8), [-1.0] * 32, None, 'M4', 2001, False, False)
        buf = ns['_serialize_frame_data'](cfg, None, True, (0, 0, 0), (0, 0, 0), [item], [])
        head = struct.calcsize(module_constant('STATE_FMT'))
        self.assertEqual(struct.unpack_from('<I', buf, 12)[0], 1, 'entity must not be skipped')
        rec = struct.unpack_from(ent_fmt, buf, head)
        self.assertEqual(len(rec), 58)
        self.assertEqual(rec[0], 42)          # id
        self.assertEqual(rec[18], 2001)       # team
        self.assertEqual(rec[56], 4023)       # hero_id  (Comet addition)
        self.assertEqual(rec[57], 0)          # is_dying (Comet addition)
        self.assertEqual(len(buf), head + ent_size + 4)


if __name__ == '__main__':
    unittest.main()
