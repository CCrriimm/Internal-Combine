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
            # Per-tick cost window (see the [sys:tick] line).
            '_perf_tick_window': 0.0, '_perf_tick_count': 0, '_perf_tick_sum': 0.0,
            '_perf_tick_max': 0.0, '_perf_tick_dt': 0.0,
            '_perf_phase_sum': [0.0, 0.0, 0.0, 0.0, 0.0], '_appearance_sync_t': 0.0, '_damping_t': 0.0, '_perf_items': 0, '_unlock_t': 0.0,
            '_perf_bone_sum': 0.0, '_g_bone_t': 0.0,
            '_perf_proj_sum': 0.0, '_perf_team_sum': 0.0, '_perf_list_sum': 0.0, '_perf_walk': 0,
            '_g_proj_t': 0.0, '_g_team_t': 0.0, '_g_list_t': 0.0, '_g_walk': 0,
            '_flag': lambda *args: False,
            '_sys_log': logs.append, '_log_vis_stats': lambda: None, '_get_cam': lambda: None,
            '_local': lambda: (None, None, None, None),
            '_serialize_frame_data': lambda *args: b'frame', 'STATE_PATH': 'unused',
            '_write': lambda *args: None,
            # Comet port knobs: loot/noise stay off in this fast-path harness.
            'OPT2_AIM_NOISE': 1 << 6,
            'IDX_OPTS': 50, 'IDX_LOOT_MAX_DIST': 51, 'IDX_NOISE_STRENGTH': 52,
            '_opt2': lambda cfg, bit: False, '_noise_strength': lambda cfg: 0.0,
            '_no_hooks_requested': lambda: False,
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


if __name__ == '__main__':
    unittest.main()
