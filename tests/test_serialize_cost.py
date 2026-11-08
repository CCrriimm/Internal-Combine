"""What the packet build is allowed to read per target, per tick.

`[sys:tick]` in a 22-target fight said `rest=0.538` and, once the fields were
labelled in the order they are actually emitted, `ser=0.458` of it - the largest
single line in the whole tick. The packet carries a name, a hero id, a monster
flag, hp, max hp, armor and max armor per target, and every one of those was
read from the engine 227 times a second: ~8 reads per target per tick, 40k a
second, for a health bar nobody can resolve above 10 Hz.

These run the real functions against a stub entity that counts its reads.
"""

import ast
import pathlib
import re
import struct
import types
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'pyexec' / 'bs_runtime.py'
SOURCE = RUNTIME.read_text(encoding='utf-8-sig')

CONSTANTS = ('STATE_MAGIC', 'VERSION', 'MAX_ENTITIES', 'STATE_FMT', 'ENT_FMT',
             'LOOT_MAX', 'LOOT_FMT', 'LOOT_SIZE', 'LOOT_HEAD',
             'STATE_HEAD_SIZE', 'ENT_SIZE', 'STATE_SIZE',
             'F_RENDER_OCCLUSION', 'F_TRACK_OCCLUSION',
             'IDX_RENDER_MAX_DIST', 'F_RENDER_FACTION', 'STATIC_TTL', 'VITALS_TTL')


def constants():
    """Evaluate the module's own constants, so the test cannot drift from them."""
    ns = {'struct': struct}
    # In source order, in one namespace: later constants are written in terms of
    # earlier ones (STATE_SIZE uses ENT_SIZE and LOOT_SIZE).
    pattern = re.compile(r'^(%s) = (.+?)\s*(?:#.*)?$' % '|'.join(CONSTANTS), re.M)
    for match in pattern.finditer(SOURCE):
        try:
            ns[match.group(1)] = eval(match.group(2), ns)
        except NameError:
            continue
    return ns


def build():
    tree = ast.parse(SOURCE)
    names = {'_serialize_frame_data', '_packed_static', '_packed_vitals'}
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
    reads = {'name': 0, 'hero': 0, 'hp': 0, 'armor': 0}
    clock = [100.0]

    def counted(key, value):
        def fn(*_a):
            reads[key] += 1
            return value
        return fn

    consts = constants()
    ns = dict(consts)
    ns.update({
        'struct': struct,
        'time': types.SimpleNamespace(time=lambda: clock[0]),
        'genv': types.SimpleNamespace(camera=types.SimpleNamespace(
            placer=types.SimpleNamespace(Direction=types.SimpleNamespace(yaw=0.0, pitch=0.0)))),
        'gui': types.SimpleNamespace(cocos_screen_size=(1920.0, 1080.0)),
        'MUI': None,
        '_state_seq': 0,
        '_state_buf': None,
        '_static_cache': None,
        '_vitals_cache': None,
        '_local_team_raw': '7',
        '_vec': lambda p: tuple(p) if p is not None else None,
        '_cam_dir': lambda _cam: (0.0, 0.0, 1.0),
        '_flag': lambda cfg, bit: False,
        '_local_hero_id': lambda: 0,
        '_team_number': lambda team, reset=False: 1,
        '_id_u64': lambda v: int(v) & 0xFFFFFFFFFFFFFFFF,
        '_bytes': lambda s, n: str(s or '').encode('utf-8')[:n].ljust(n, b'\0'),
        '_query_occlusion_cache': lambda *a, **k: None,
        '_line_of_sight_query': lambda *a: True,
        '_ename': counted('name', 'Foo'),
        '_hero_of': counted('hero', 0),
        '_hp': counted('hp', 80.0),
        '_max_hp': counted('hp', 100.0),
        '_armor': counted('armor', 50.0),
        '_max_armor': counted('armor', 100.0),
    })
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(RUNTIME), 'exec'), ns)
    return ns, reads, clock


def config():
    """A config with nothing enabled and a render distance that admits the rows."""
    cfg = [0.0] * 64
    cfg[constants()['IDX_RENDER_MAX_DIST']] = 400.0
    return cfg


def entity(eid):
    return types.SimpleNamespace(id=eid, IsMonster=False)


def items(eid, count=2):
    out = []
    for i in range(count):
        out.append((10.0 + i, entity(eid + i), eid + i,
                    (0.0, 1.6, 10.0), (0.0, 0.0, 10.0), (0.5, 0.4), (0.5, 0.8),
                    [-1.0] * 48, (-1.0, -1.0), 'AK', '8', False, False))
    return out


class SerializeCostTests(unittest.TestCase):

    def test_the_packet_is_not_rebuilt_from_scratch_every_tick(self):
        ns, reads, clock = build()
        cfg = config()
        rows = items(100)
        first = ns['_serialize_frame_data'](cfg, None, True, (0.0, 0.0, 0.0), None, rows)
        after_first = dict(reads)
        second = ns['_serialize_frame_data'](cfg, None, True, (0.0, 0.0, 0.0), None, rows)
        self.assertEqual(reads, after_first,
                         'a second tick with the same targets reads nothing')
        self.assertEqual(len(first), len(second))
        # Magic and version, not the sequence number or the timestamp: those
        # are supposed to change every tick.
        self.assertEqual(bytes(first[:8]), bytes(second[:8]))

    def test_vitals_refresh_on_their_own_timer(self):
        ns, reads, clock = build()
        cfg = config()
        rows = items(100)
        ns['_serialize_frame_data'](cfg, None, True, (0.0, 0.0, 0.0), None, rows)
        after_first = dict(reads)
        clock[0] += 0.5          # past VITALS_TTL, far short of STATIC_TTL
        ns['_serialize_frame_data'](cfg, None, True, (0.0, 0.0, 0.0), None, rows)
        self.assertGreater(reads['hp'], after_first['hp'], 'hp is re-read after 100 ms')
        self.assertEqual(reads['name'], after_first['name'], 'the name is not')
        self.assertEqual(reads['hero'], after_first['hero'])

    def test_the_statics_refresh_eventually(self):
        ns, reads, clock = build()
        cfg = config()
        rows = items(100)
        ns['_serialize_frame_data'](cfg, None, True, (0.0, 0.0, 0.0), None, rows)
        after_first = dict(reads)
        clock[0] += 2.0
        ns['_serialize_frame_data'](cfg, None, True, (0.0, 0.0, 0.0), None, rows)
        self.assertGreater(reads['name'], after_first['name'])

    def test_one_buffer_for_the_life_of_the_process(self):
        ns, reads, clock = build()
        cfg = config()
        ns['_serialize_frame_data'](cfg, None, True, (0.0, 0.0, 0.0), None, items(100))
        buf = ns['_state_buf']
        ns['_serialize_frame_data'](cfg, None, True, (0.0, 0.0, 0.0), None, items(200))
        self.assertIs(ns['_state_buf'], buf, '64 KB per tick was allocated here')
        self.assertIsNotNone(buf)

    def test_a_shorter_packet_does_not_leak_the_previous_one(self):
        ns, reads, clock = build()
        cfg = config()
        ns['_serialize_frame_data'](cfg, None, True, (0.0, 0.0, 0.0), None, items(100, 5))
        short = ns['_serialize_frame_data'](cfg, None, True, (0.0, 0.0, 0.0), None, items(300, 1))
        consts = constants()
        self.assertEqual(len(short),
                         consts['STATE_HEAD_SIZE'] + consts['ENT_SIZE'] + consts['LOOT_HEAD'])
        count = struct.unpack_from('<I', short, 12)[0]
        self.assertEqual(count, 1)


class TickLabelTests(unittest.TestCase):
    """The four sub-timers were emitted in a different order than they were
    labelled, so `bone=` was really sync and `wr=` was really collect - they did
    not even add up to `rest`. 0 of 91 lines summed; every line did once the
    labels were put back in the order of the arguments."""

    def test_the_parts_are_labelled_where_the_arguments_are(self):
        body = SOURCE[SOURCE.index('[sys:tick] n=%d'):]
        body = body[:body.index('force=True')]
        self.assertIn("'rest=%.3f sync=%.3f local=%.3f ser=%.3f wr=%.3f '", body)
        self.assertLess(body.index('rest=%.3f'), body.index('sync=%.3f'))
        self.assertLess(body.index('wr=%.3f'), body.index('bone=%.3f'))

    def test_the_arguments_are_emitted_in_that_order(self):
        body = SOURCE[SOURCE.index('[sys:tick] n=%d'):]
        body = body[:body.index('force=True')]
        order = [body.index('_perf_phase_sum[%d]' % i) for i in range(9)]
        self.assertEqual(order, sorted(order), 'phases 0..8 in order')
        self.assertLess(body.index('_perf_phase_sum[8]'), body.index('_perf_bone_sum'))


if __name__ == '__main__':
    unittest.main()
