"""Exercise the fast frame path without loading the game or its hooks."""
import ast
from pathlib import Path
import traceback
import types
import unittest


class RuntimeTickTests(unittest.TestCase):
    def test_fast_tick_finishes_without_performance_logging_exception(self):
        source = Path(__file__).resolve().parents[1] / 'pyexec/bs_runtime.py'
        node = next(n for n in ast.parse(source.read_text(encoding='utf-8-sig')).body
                    if isinstance(n, ast.FunctionDef) and n.name == '_on_story_tick')
        logs = []
        ns = {
            'time': types.SimpleNamespace(time=lambda: 100.0),
            'builtins': types.SimpleNamespace(_m_generation=1), '_my_gen': 1,
            'genv': types.SimpleNamespace(is_in_hall=False), 'traceback': traceback,
            '_read_cfg': lambda: [123], 'IDX_MAGIC': 0, 'CFG_MAGIC': 123,
            '_last_hooks': 100.0, '_last_scrub': 100.0, '_last_catalog_check': 100.0,
            '_fire_release_at': 0.0, '_perf_t': 0.0, '_perf_last': 0.0,
            '_flag': lambda *args: False,
            '_sys_log': logs.append, '_get_cam': lambda: None,
            '_local': lambda: (None, None, None, None),
            '_serialize_frame_data': lambda *args: b'frame', 'STATE_PATH': 'unused',
            '_write': lambda *args: None,
            # Comet port knobs: loot/noise stay off in this fast-path harness.
            'OPT2_AIM_NOISE': 1 << 6,
            'IDX_OPTS': 50, 'IDX_LOOT_MAX_DIST': 51, 'IDX_NOISE_STRENGTH': 52,
            '_opt2': lambda cfg, bit: False, '_noise_strength': lambda cfg: 0.0,
            'math': __import__('math'),
        }
        for name in ('F_MENU_OPEN', 'F_UNLOCK_EFFECTS', 'F_FORCE_KILLFX',
                     'F_TRACK_VIEW', 'F_RENDER_ENTITIES'):
            ns[name] = 0
        for name in ('_sync_ray_interceptor', '_apply', '_sync_configured_appearance',
                     '_dispatch_appearance_queue', '_sync_impulse_damping', '_sync_weapon_case_damping'):
            ns[name] = lambda *args: None
        exec(compile(ast.Module(body=[node], type_ignores=[]), str(source), 'exec'), ns)
        ns['_on_story_tick'](0.016)
        self.assertEqual(logs, [])
        self.assertEqual(ns['_last_state'], 100.0)


    def test_entity_gather_is_rate_limited_and_invalidated(self):
        source = Path(__file__).resolve().parents[1] / 'pyexec/bs_runtime.py'
        node = next(n for n in ast.parse(source.read_text(encoding='utf-8-sig')).body
                    if isinstance(n, ast.FunctionDef) and n.name == '_on_story_tick')
        now = [100.0]
        cfg = [123, 100.0, 100.0]
        flags = {1, 2}
        gathers, tracks, logs = [], [], []
        game = types.SimpleNamespace(is_in_hall=False, space=object())
        ns = {
            'time': types.SimpleNamespace(time=lambda: now[0]),
            'builtins': types.SimpleNamespace(_m_generation=1), '_my_gen': 1,
            'genv': game, 'traceback': traceback, '_read_cfg': lambda: cfg,
            'IDX_MAGIC': 0, 'CFG_MAGIC': 123, 'IDX_RENDER_MAX_DIST': 1,
            'IDX_TRACK_MAX_DIST': 2, '_last_hooks': 100.0, '_last_scrub': 100.0,
            '_last_catalog_check': 100.0, '_fire_release_at': 0.0,
            '_perf_t': 100.0, '_perf_last': 100.0, '_diag_t': 100.0,
            '_last_gather': 0.0, '_gather_cache': (0.0, None, None, []),
            '_gather_key': None, '_flag': lambda c, f: f in flags,
            '_sys_log': logs.append, '_local': lambda: (None, None, None, None),
            '_query_visible_entities': lambda *args: (gathers.append(args) or (object(), (0, 0, 0), [1])),
            '_update_cam_trajectory': lambda *args: tracks.append(args),
            '_serialize_frame_data': lambda *args: b'frame', 'STATE_PATH': 'unused',
            '_write': lambda *args: None, '_get_cam': lambda: None,
            'F_TRACK_VIEW': 1, 'F_RENDER_ENTITIES': 2, 'F_RENDER_EQUIPMENT': 3,
            'F_RENDER_SKELETON': 4, 'F_MENU_OPEN': 5, 'F_UNLOCK_EFFECTS': 6,
            'F_FORCE_KILLFX': 7,
        }
        for name in ('_sync_ray_interceptor', '_apply', '_sync_configured_appearance',
                     '_dispatch_appearance_queue', '_sync_impulse_damping', '_sync_weapon_case_damping'):
            ns[name] = lambda *args: None
        exec(compile(ast.Module(body=[node], type_ignores=[]), str(source), 'exec'), ns)
        tick = ns['_on_story_tick']
        tick(0.004)
        now[0] += 0.004
        tick(0.004)
        self.assertEqual(len(gathers), 1)
        self.assertEqual(len(tracks), 2)  # tracking remains per-tick
        now[0] += 0.014
        tick(0.004)
        self.assertEqual(len(gathers), 2)
        flags.add(4)
        tick(0.004)
        self.assertEqual(len(gathers), 3)  # skeleton option changed
        game.space = object()
        tick(0.004)
        self.assertEqual(len(gathers), 4)
        game.is_in_hall = True
        tick(0.004)
        game.is_in_hall = False
        tick(0.004)
        self.assertEqual(len(gathers), 5)  # re-entering a match invalidates cache
        self.assertEqual(logs, [])


if __name__ == '__main__':
    unittest.main()
