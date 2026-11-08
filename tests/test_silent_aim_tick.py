"""The aim path must run on every tick, unlike the ESP.

`_query_corrected_ray` - the silent-aim redirect - only replaces the bullet's ray
when `_track_busy` is true and `_ray_target` is fresh, and both are published by
`_update_cam_trajectory`. That function also integrates the view turn with `dt`.

A build throttled it to 60 Hz "because the items are 60 Hz old anyway". At 145 fps
that skipped roughly every other tick, and the skipped ones fell into the `else`
branch which sets `_track_busy = False` - so the redirect returned the unmodified
ray on half of all frames and silent aim stopped landing. The gather can be
rate-limited because stale positions are predicted forward; this cannot.
"""

import ast
from pathlib import Path
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'pyexec' / 'bs_runtime.py'


class _Clock:
    """time.time() frozen per tick, so `now - last` is exactly the tick gap."""

    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


def build(tick_calls, campoint=(0.0, 0.0, 0.0)):
    node = next(n for n in ast.parse(SOURCE.read_text(encoding='utf-8-sig')).body
                if isinstance(n, ast.FunctionDef) and n.name == '_on_story_tick')
    clock = _Clock()
    item = (12.0, types.SimpleNamespace(id=7), 7, (1.0, 2.0, 3.0), (1.0, 0.0, 3.0),
            (0.5, 0.5), (0.5, 0.6), [], '', '', 'au', False)

    def update_cam_trajectory(cfg, items, cam, origin, menu_open, dt):
        tick_calls.append(clock.t)
        ns['_track_busy'] = True
        ns['_ray_target'] = ('target', (1.0, 2.0, 3.0), clock.t, 'space')

    ns = {
        'time': types.SimpleNamespace(time=clock),
        'builtins': types.SimpleNamespace(_m_generation=1), '_my_gen': 1,
        'genv': types.SimpleNamespace(is_in_hall=False, space='space'),
        'traceback': __import__('traceback'),
        'math': __import__('math'),
        'cfg': [0], 'IDX_MAGIC': 0, 'CFG_MAGIC': 123, 'DEFAULT_CFG': [0],
        'IDX_TRACK_MAX_DIST': 20, 'IDX_RENDER_MAX_DIST': 21,
        'IDX_TRACK_FOV': 22, 'IDX_TRACK_JOINT': 23,
        '_read_cfg': lambda: [123, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 400.0, 0],
        '_last_hooks': 100.0, '_last_scrub': 100.0, '_last_catalog_check': 100.0,
        '_fire_release_at': 0.0, '_perf_t': 0.0, '_perf_last': 0.0,
        '_perf_tick_window': 0.0, '_perf_tick_count': 0, '_perf_tick_sum': 0.0,
        '_perf_tick_max': 0.0, '_perf_tick_dt': 0.0,
        '_perf_phase_sum': [0.0] * 5, '_appearance_sync_t': 100.0, '_damping_t': 100.0,
        '_perf_items': 0, '_unlock_t': 100.0, '_perf_bone_sum': 0.0, '_g_bone_t': 0.0,
            '_perf_proj_sum': 0.0, '_perf_team_sum': 0.0, '_perf_list_sum': 0.0, '_perf_walk': 0,
            '_g_proj_t': 0.0, '_g_team_t': 0.0, '_g_list_t': 0.0, '_g_walk': 0,
        '_last_gather': 0.0, '_gather_cache': (0.0, None, None, []),
        '_g_bone_t': 0.0, '_track_busy': False, '_ray_target': None,
        # Track on, render off: the aim path is the only consumer.
        '_flag': lambda cfg, bit: bit == 1,
        'F_MENU_OPEN': 2, 'F_UNLOCK_EFFECTS': 3, 'F_FORCE_KILLFX': 4,
        'F_TRACK_VIEW': 1, 'F_RENDER_ENTITIES': 5, 'F_RENDER_EQUIPMENT': 6,
        'F_RENDER_SKELETON': 7,
        '_sys_log': lambda *a: None, '_log_vis_stats': lambda: None,
        '_get_cam': lambda: types.SimpleNamespace(GetOrigin=lambda: campoint),
        '_query_visible_entities': lambda *a: ('cam', campoint, [item]),
        '_update_cam_trajectory': update_cam_trajectory,
        '_sync_impulse_damping': lambda *a: None,
        '_sync_weapon_case_damping': lambda *a: None,
        '_local': lambda: (None, None, None, (0.0, 0.0, 0.0)),
        '_serialize_frame_data': lambda *a: None, 'STATE_PATH': 'unused',
        '_write': lambda *a: None, '_no_hooks_requested': lambda: False,
        '_install_instance_hooks': lambda *a: None, '_save_template_cache': lambda *a: None,
        '_trigger_primary_input': lambda *a: None, '_mouse': lambda *a: None,
        '_sync_ray_interceptor': lambda *a: None, '_apply': lambda *a: None,
        '_sync_configured_appearance': lambda *a: None,
        '_dispatch_appearance_queue': lambda *a: None,
        '_local_hero_id': lambda: 0,
        '_opt2': lambda cfg, bit: False, 'OPT2_AIM_NOISE': 1 << 6,
        'IDX_OPTS': 50, 'IDX_LOOT_MAX_DIST': 51, 'IDX_NOISE_STRENGTH': 52,
        '_noise_strength': lambda cfg: 0.0,
    }
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), 'exec'), ns)
    return ns, clock


class SilentAimTickTests(unittest.TestCase):

    def test_the_trajectory_runs_on_every_tick_even_at_high_fps(self):
        """145 fps: the second tick is 6.9 ms after the first, under 1/60."""
        calls = []
        ns, clock = build(calls)
        for step in (0.0, 0.0069, 0.0138):
            clock.t = 100.0 + step
            ns['_on_story_tick'](0.0069)
        self.assertEqual(len(calls), 3, 'a rate limit here silently disables the redirect')

    def test_the_redirect_stays_enabled_between_ticks(self):
        calls = []
        ns, clock = build(calls)
        for step in (0.0, 0.0069, 0.0138):
            clock.t = 100.0 + step
            ns['_on_story_tick'](0.0069)
            self.assertTrue(ns['_track_busy'], '_query_corrected_ray needs this true')
            self.assertIsNotNone(ns['_ray_target'])

    def test_nothing_rate_limits_the_trajectory_call(self):
        """The gate that broke silent aim, stated as a rule on the source."""
        source = SOURCE.read_text(encoding='utf-8-sig')
        tick = source[source.index('def _on_story_tick'):]
        tick = tick[:tick.index('def ', 10)]
        head = tick[:tick.index('_update_cam_trajectory(cfg, items')]
        self.assertIn('if items and track_active:', head)
        self.assertNotIn('_track_t', source,
                         'the trajectory must not be on a timer; see the comment')
        self.assertNotIn('now - _track_t', source)

    def test_the_gather_is_still_the_one_that_is_rate_limited(self):
        source = SOURCE.read_text(encoding='utf-8-sig')
        start = source.index('interval = 1.0 / 60.0')
        self.assertIn('_last_gather', source[start:start + 600])


if __name__ == '__main__':
    unittest.main()
