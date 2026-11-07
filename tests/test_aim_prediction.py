"""Prediction math and aim-path tests without importing the live runtime."""
import ast
import math
from pathlib import Path
import types
import unittest


class PredictionTests(unittest.TestCase):
    def setUp(self):
        source = Path(__file__).resolve().parents[1] / 'pyexec/bs_runtime.py'
        names = {'_sample_target_motion', '_calc_ballistic_velocity', '_calc_tracking_ratio',
                 '_estimate_lead_position', '_human_frac', '_lerp', '_ncaf', '_flag', '_call_attr',
                 '_update_cam_trajectory'}
        nodes = [n for n in ast.parse(source.read_text(encoding='utf-8-sig')).body
                 if isinstance(n, ast.FunctionDef) and n.name in names]
        self.assertEqual({n.name for n in nodes}, names)
        self.now = 10.0
        self.target = types.SimpleNamespace(id=12)
        self.case = types.SimpleNamespace()
        self.genv = types.SimpleNamespace(space=object())
        self.ns = {'math': math, 'genv': self.genv,
                   'time': types.SimpleNamespace(monotonic=lambda: self.now, time=lambda: self.now),
                   '_vec': lambda p: tuple(p) if p is not None else None,
                   '_len': lambda p: math.sqrt(sum(x*x for x in p)),
                   '_norm': lambda p: tuple(x / (math.sqrt(sum(y*y for y in p)) or 1) for x in p),
                   '_dot': lambda a,b: sum(x*y for x,y in zip(a,b)),
                   '_sub': lambda a,b: tuple(x-y for x,y in zip(a,b)),
                   '_local': lambda: (object(), None, None, None), '_current_case': lambda e: self.case,
                   'IDX_FLAGS': 3, 'F_TRACK_LEAD': 1 << 18, 'IDX_TRACK_SMOOTH': 11,
                   'IDX_TRACK_KEY': 7, 'IDX_TRACK_JOINT': 15, 'IDX_TRACK_MAX_DIST': 12, 'IDX_TRACK_FOV': 10,
                   'F_TRACK_VIEW': 1 << 13, 'OPT_RAY_REDIRECT': 1 << 5,
                   '_options': lambda cfg: cfg[17]}
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), self.ns)
        self.cfg = [0] * 40
        self.cfg[3] = (1 << 18) | (1 << 13)
        self.cfg[10:13] = [90, .35, 400]
        self.sample((0, 0, 100), 9.9)
        self.sample((.5, 0, 100), 10.0)

    def sample(self, p, now):
        self.now = now
        self.ns['_sample_target_motion'](self.target, p, now)

    def predict(self, point=(.5, 0, 100)):
        return self.ns['_estimate_lead_position'](self.target, point, (0, 0, 0), self.cfg, 1/60, (0, 0, 1))

    def test_running_target_leads_forward(self):
        point = self.predict()
        self.assertGreater(point[0], .5)
        self.assertEqual(point[1:], (0, 100))

    def test_silent_prediction_ignores_tracking_smooth(self):
        points = []
        for smooth in (.01, .5, 1.):
            self.cfg[11] = smooth
            points.append(self.ns['_estimate_lead_position'](self.target, (.5, 0, 100), (0, 0, 0),
                                                           self.cfg, 1/60, (0, 0, 1), tracking=False))
        self.assertEqual(points[0], points[1]); self.assertEqual(points[1], points[2])

    def test_flag_off_returns_original_point(self):
        self.cfg[3] &= ~(1 << 18)
        self.assertEqual(self.predict(), (.5, 0, 100))

    def test_stationary_target_has_no_lead(self):
        for i in range(1, 21):
            self.sample((.5, 0, 100), 10 + i*.02)
        self.assertAlmostEqual(self.predict()[0], .5, places=4)

    def test_direction_change_reverses_lead(self):
        for i in range(1, 11):
            self.sample((.5 - i*.1, 0, 100), 10 + i*.02)
        self.assertLess(self.predict((-.5, 0, 100))[0], -.5)

    def test_teleport_resets_velocity(self):
        self.sample((200, 0, 100), 10.02)
        self.assertEqual(self.predict((200, 0, 100)), (200, 0, 100))

    def test_stale_samples_do_not_predict(self):
        self.now = 10.3
        self.assertEqual(self.predict(), (.5, 0, 100))

    def test_space_change_discards_old_motion(self):
        self.genv.space = object()
        self.assertEqual(self.predict(), (.5, 0, 100))
        self.sample((100, 0, 100), 10.1)
        self.assertEqual(self.predict((100, 0, 100)), (100, 0, 100))

    def test_first_sample_and_invalid_position_do_not_predict(self):
        self.target = types.SimpleNamespace(id=99)
        self.sample((1, 0, 100), 10.1)
        self.assertEqual(self.predict((1, 0, 100)), (1, 0, 100))
        self.sample((float('nan'), 0, 100), 10.2)
        self.assertEqual(self.predict(), (.5, 0, 100))

    def test_weapon_speed_adds_intercept_time(self):
        tracking_only = self.predict()
        self.case.weapon_proto = {'bullet_speed': 200.0}
        point = self.predict()
        self.assertGreater(point[0], tracking_only[0] + 2.4)
        # The intercept time satisfies the moving-target travel equation.
        velocity = 5.0
        lag = tracking_only[0] / velocity - .5 / velocity
        total = (point[0] - .5) / velocity
        flight = total - lag
        self.assertAlmostEqual(math.sqrt(point[0]**2 + 100**2), 200 * flight, places=6)

    def test_invalid_speed_falls_back_to_tracking(self):
        expected = self.predict()
        for value in (None, 0, -1, float('nan'), float('inf'), 'bad'):
            self.case.weapon_proto = {'bullet_speed': value}
            self.assertEqual(self.predict(), expected)

    def test_low_fps_lead_is_bounded(self):
        point = self.ns['_estimate_lead_position'](self.target, (.5, 0, 100), (0,0,0), self.cfg, 1.0, (0,0,1))
        self.assertLessEqual(point[0], .5 + 5*.25)

    def test_prediction_reduces_steady_running_tracking_lag(self):
        def run(enabled):
            self.ns['_sample_target_motion'].__dict__.clear()
            self.cfg[3] = (1 << 18) if enabled else 0
            yaw = 0.0
            errors = []
            for tick in range(181):
                x = 5.0 * tick / 60.0
                self.sample((x, 0, 100), 20.0 + tick/60.0)
                direction = (math.sin(yaw), 0, math.cos(yaw))
                point = self.ns['_estimate_lead_position'](self.target, (x,0,100), (0,0,0),
                                                          self.cfg, 1/60, direction)
                target_yaw = math.atan2(point[0], point[2])
                fraction = self.ns['_calc_tracking_ratio'](.35, 1/60, abs(target_yaw-yaw))
                yaw += (target_yaw-yaw) * fraction
                if tick > 60:
                    errors.append(abs(x - 100.0 * math.tan(yaw)))
            return sum(errors)/len(errors)
        raw_error = run(False)
        predicted_error = run(True)
        self.assertLess(predicted_error, raw_error * .5)

    def test_new_and_retained_aim_paths_both_apply_prediction(self):
        point = (.5, 0, 100)
        best = (self.target, point, point, 100, 1)
        self.ns.update({'_track_prev': False, '_track_latched': False, '_track_busy': False, '_path': None,
                        '_track_hold_until': 0, '_track_diag_t': self.now, '_track_fallback_n': 0,
                        '_track_eid': 12, '_track_eid_t': self.now,
                        '_poll_input_keystate': lambda key: True,
            '_poll_aim_keys': lambda cfg, key: True, '_get_track_mode_toggle': lambda cfg: False,
                        '_filter_view_direction': lambda e,c,t,cfg,dt: t,
                        '_get_placer': lambda: types.SimpleNamespace(TurnView=lambda *args: None),
                        '_cam_dir': lambda cam: (0,0,1), '_key_diag': lambda key: '', '_sys_log': lambda msg: None,
                        # Comet port: aim-only input noise stays off in this harness.
                        'OPT2_AIM_NOISE': 1 << 6, 'IDX_OPTS': 50, 'IDX_NOISE_STRENGTH': 52,
                        '_opt2': lambda cfg, bit: False, '_noise_strength': lambda cfg: 0.0})
        for retained in (False, True):
            turns = []
            self.ns['_pick'] = lambda *args: None if retained else best
            self.ns['_pick_eid'] = lambda *args: best
            self.ns['_apply_view_transform'] = lambda p,c,t,s,dt,*a: turns.append(t)
            self.ns['_update_cam_trajectory'](self.cfg, [best], object(), (0,0,0), False, 1/60)
            self.assertGreater(turns[0][0], point[0])


if __name__ == '__main__':
    unittest.main()
