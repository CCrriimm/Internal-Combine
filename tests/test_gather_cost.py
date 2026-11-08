"""What the ESP gather is allowed to do per entity, per frame.

The runtime was spending 8.5 ms inside a 13.4 ms frame in a match
(`[sys:tick] n=747 avg_ms=8.535 frame_ms=13.39 in_match=1`). Sixteen skeleton
joints were being fetched for *every* entity inside the scan distance - about
1700 engine calls a frame - including entities nowhere near the screen. These
tests pin the two fixes: sample skeleton and weapon only for targets that are on
screen, and let the skeleton's world transforms be one frame old in the ESP path.
"""

import ast
import math
import os
import pathlib
import time
import types
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'pyexec' / 'bs_runtime.py'


def build(screen_value, want_bone=True, want_gun=True):
    """A minimal gather harness with call counters."""
    source = RUNTIME.read_text(encoding='utf-8-sig')
    names = {'_query_visible_entities', '_collect_space_entities', '_ai_kind', '_is_combat',
             '_combat_type',
             '_entity_flag', '_team_id', '_check_faction_relation', '_local', '_call_attr',
             '_is_vehicle', '_is_skill_object', '_normalize_team_id'}
    tree = ast.parse(source)
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
    calls = {'joint': [], 'gun': 0}

    def joint(e, which, ttl=0.008):
        calls['joint'].append((which, ttl))
        return (0.0, 1.0, 10.0)

    def gun(e):
        calls['gun'] += 1
        return 'AK'

    local = types.SimpleNamespace(id=1, combat_team_id='7', position=(0, 0, 0),
                                  IsPlayerCombatAvatar=True)
    space = types.SimpleNamespace(entities={}, game_logic=types.SimpleNamespace())
    ns = {
        'time': time, 'math': math, 'genv': types.SimpleNamespace(
            player=local, avatar=local, space=space, is_in_hall=False),
        '_local_cache': None, '_ent_list_cache': None,
        '_vec': lambda p: tuple(p) if p is not None else None, '_v3': lambda p: p,
        '_get_cam': lambda: types.SimpleNamespace(GetOrigin=lambda: (0, 0, 0)),
        '_cam_dir': lambda _: (0, 0, 1),
        '_sub': lambda a, b: tuple(x - y for x, y in zip(a, b)),
        '_dot': lambda a, b: sum(x * y for x, y in zip(a, b)),
        '_len': lambda v: math.sqrt(sum(x * x for x in v)),
        '_alive': lambda e: True, '_is_dying': lambda e: False,
        '_sample_target_motion': lambda *a: None,
        '_sample_joint_transform': joint,
        '_matrix_world_to_screen': lambda p: screen_value,
        '_gun_name': gun, '_g_log_t': 0, '_g_slow': 0, '_sys_log': lambda *a, **k: None,
        'MAX_ENTITIES': 256, '_bone_cache': {}, '_combat_type_cache': {},
    }
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(RUNTIME), 'exec'), ns)
    return ns, space, calls, local


def target(eid, is_combat=True):
    e = types.SimpleNamespace(id=eid, combat_team_id='8', position=(0, 0, 10),
                              IsPlayerCombatAvatar=True,
                              model=types.SimpleNamespace(Position=(0, 0, 10)))
    return e


def gather(ns, space, entities, want_bone=True, want_gun=True):
    space.entities = {e.id: e for e in entities}
    ns['_ent_list_cache'] = None
    return ns['_query_visible_entities'](400, want_gun, want_bone)[2]


class GatherCostTests(unittest.TestCase):

    def test_offscreen_targets_get_no_joint_sampling(self):
        ns, space, calls, _ = build(None)          # (None) -> nothing projects on screen
        out = gather(ns, space, [target(2), target(3), target(4)], want_bone=True)
        self.assertEqual(len(out), 3, 'offscreen targets must still be gathered for arrows')
        self.assertEqual([c for c in calls['joint'] if c[0] != 0], [],
                         'no skeleton joints for targets that are not on screen')

    def test_on_screen_targets_do_get_skeleton_joints(self):
        ns, space, calls, _ = build((0.5, 0.5))
        gather(ns, space, [target(2)], want_bone=True)
        # 1 head sample (fresh ttl) + 16 skeleton joints (16 ms ttl).
        self.assertEqual(len(calls['joint']), 17)
        self.assertEqual([c for c in calls['joint'] if c[0] == 0][0][1], 0.008)

    def test_skeleton_joints_may_be_one_frame_old_in_the_esp_path(self):
        ns, space, calls, _ = build((0.5, 0.5))
        gather(ns, space, [target(2)], want_bone=True)
        ttls = {ttl for which, ttl in calls['joint'] if which != 0}
        self.assertEqual(ttls, {0.016})

    def test_the_head_stays_fresh(self):
        source = RUNTIME.read_text(encoding='utf-8-sig')
        self.assertIn('def _sample_joint_transform(e, which, ttl=0.008):', source)
        # The aim path calls it without the ttl argument, so it keeps 8 ms.
        body = source[source.index('def _query_visible_entities'):]
        body = body[:body.index('def _owner_type_name')]
        self.assertIn("head = _sample_joint_transform(e, 0) if (want_bone and in_front) else None",
                      body)

    def test_the_weapon_name_is_only_looked_up_for_visible_targets(self):
        ns, space, calls, _ = build(None)
        gather(ns, space, [target(2), target(3)], want_gun=True)
        self.assertEqual(calls['gun'], 0)

    def test_behind_camera_targets_do_not_fetch_a_head_joint(self):
        ns, space, calls, _ = build(None)
        behind = target(2)
        behind.position = (0, 0, -50)
        behind.model = types.SimpleNamespace(Position=(0, 0, -50))
        out = gather(ns, space, [behind], want_bone=True)
        self.assertEqual(calls['joint'], [], 'an arrow needs a direction, not a joint')
        self.assertEqual(len(out), 1)

    def test_the_distance_filter_runs_before_classification(self):
        """Classification is ~two dozen flag checks per entity; far entities skip it."""
        source = RUNTIME.read_text(encoding='utf-8-sig')
        body = source[source.index('def _query_visible_entities'):]
        body = body[:body.index('def _owner_type_name')]
        self.assertLess(body.index('rough > max_dist'), body.index('_ai_kind(e, tn)'))

    def test_combat_class_check_is_cached_by_type(self):
        source = RUNTIME.read_text(encoding='utf-8-sig')
        self.assertIn('def _combat_type(cls):', source)
        self.assertIn('_combat_type_cache[cls] = hit', source)
        self.assertIn("_entity_flag(e, 'IsCombatAvatar') and _combat_type(type(e))", source)

    def test_skeleton_still_comes_back_in_packet_shape(self):
        ns, space, calls, _ = build(None)
        out = gather(ns, space, [target(2)], want_bone=True)
        self.assertEqual(len(out[0][7]), 48, '16 joints x world xyz, all -1 when not sampled')


class GatherRateTests(unittest.TestCase):

    def test_the_gather_is_capped_rather_than_run_per_frame(self):
        source = RUNTIME.read_text(encoding='utf-8-sig')
        start = source.index('interval = 1.0 / 60.0')
        self.assertIn('_last_gather', source[start:start + 600])

    def test_only_the_nearest_few_targets_get_a_skeleton(self):
        source = RUNTIME.read_text(encoding='utf-8-sig')
        body = source[source.index('def _query_visible_entities'):]
        body = body[:body.index('def _owner_type_name')]
        self.assertIn('bone_budget = 10', body)
        self.assertIn('bone_budget > 0', body)

    def test_a_cached_gather_is_reused(self):
        source = RUNTIME.read_text(encoding='utf-8-sig')
        start = source.index('if now - _last_gather >= interval')
        self.assertIn('_gather_cache = (now, cam, origin, items)', source[start:start + 500])


if __name__ == '__main__':
    unittest.main()
