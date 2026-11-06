"""Mode/shot/recoil checks using extracted runtime functions, without loading the game."""
import ast
import math
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch


SOURCE = Path(__file__).resolve().parents[1] / 'pyexec/bs_runtime.py'


def functions(names, namespace):
    nodes = [node for node in ast.parse(SOURCE.read_text('utf-8-sig')).body
             if isinstance(node, ast.FunctionDef) and node.name in names]
    if {node.name for node in nodes} != names:
        raise AssertionError('Missing runtime function')
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'), namespace)
    return namespace


class AimModeTests(unittest.TestCase):
    def setUp(self):
        self.cfg = [0] * 40
        self.cfg[3] = 1 << 13
        self.cfg[7] = 2
        self.cfg[10:13] = [90, .35, 400]
        self.cfg[17] = 1 << 5
        self.player = object()
        self.target = object()
        self.space = object()
        self.native = (0., 0., 1.)
        self.calls = []
        calls, native = self.calls, self.native
        class SpellWorker:
            def __init__(worker, caster):
                worker.caster = caster
            def GetShootDir(self, shoot_pos):
                calls.append(shoot_pos)
                return self.GetRayDirectionFromScreenPoint()
            def GetRayDirectionFromScreenPoint(self):
                return native
        self.cls = SpellWorker
        self.original = SpellWorker.GetShootDir
        module = types.SimpleNamespace(SpellWorker=SpellWorker)
        self.module_patch = patch.dict(sys.modules, {
            'gclient.gameplay.logic_base.spell.spell_core.gun_spell': module,
            'MType': types.SimpleNamespace(Vector3=lambda *v: tuple(v)),
        })
        self.module_patch.start(); self.addCleanup(self.module_patch.stop)
        self.camera = types.SimpleNamespace(GetOrigin=lambda: (0, 0, 0))
        self.genv = types.SimpleNamespace(player=self.player, space=self.space, is_in_hall=False)
        self.turns, self.humanized, self.prediction_modes = [], [], []
        self.ns = functions({'_query_corrected_ray', '_sync_ray_interceptor', '_update_cam_trajectory'}, {
            'sys': sys, 'math': math, 'time': types.SimpleNamespace(time=lambda: 10.), 'genv': self.genv,
            'F_TRACK_VIEW': 1 << 13, 'F_MENU_OPEN': 1, 'OPT_RAY_REDIRECT': 1 << 5,
            'IDX_TRACK_KEY': 7, 'IDX_TRACK_FOV': 10, 'IDX_TRACK_SMOOTH': 11,
            'IDX_TRACK_MAX_DIST': 12, 'IDX_TRACK_JOINT': 15,
            '_flag': lambda cfg, bit: bool(cfg[3] & bit), '_options': lambda cfg: cfg[17],
            '_read_cfg': lambda: self.cfg, '_alive': lambda e: e is self.target,
            '_get_cam': lambda: self.camera, '_get_placer': lambda: types.SimpleNamespace(TurnView=lambda *a: None),
            '_cam_dir': lambda c: (0, 0, 1), '_vec': lambda v: tuple(v),
            '_sub': lambda a, b: tuple(x-y for x, y in zip(a, b)),
            '_len': lambda v: math.sqrt(sum(x*x for x in v)),
            '_norm': lambda v: tuple(x/(math.sqrt(sum(y*y for y in v)) or 1) for x in v),
            '_dot': lambda a, b: sum(x*y for x, y in zip(a, b)),
            '_ray_hook': None, '_ray_target': None, '_track_busy': True,
            '_track_prev': False, '_track_latched': False, '_path': None,
            '_track_hold_until': 0, '_track_diag_t': 10., '_track_fallback_n': 0,
            '_track_eid': None, '_track_eid_t': 0.,
            '_get_track_mode_toggle': lambda cfg: False, '_poll_input_keystate': lambda key: True,
            '_pick': lambda *a: (self.target, (10, 0, 100), (10, 0, 100), 100, 1),
            '_pick_eid': lambda *a: None,
            '_filter_view_direction': lambda *a: self.humanized.append(a) or a[2],
            '_apply_view_transform': lambda *a: self.turns.append(a),
            # Comet port: input noise is opt-in through the secondary options
            # word; this harness leaves it off so the turn calls stay identical.
            'OPT2_AIM_NOISE': 1 << 6, 'IDX_OPTS': 50, 'IDX_NOISE_STRENGTH': 52,
            '_opt2': lambda cfg, bit: bool((cfg[50] if len(cfg) > 50 else 0) & bit),
            '_noise_strength': lambda cfg: 0.0,
            '_sys_log': lambda *a: None,
            '_key_diag': lambda key: '',
        })
        def prediction(e, p, o, cfg, dt, cur, tracking=True):
            self.prediction_modes.append(tracking)
            return p
        self.ns['_estimate_lead_position'] = prediction

    def frame(self, menu=False):
        self.ns['_update_cam_trajectory'](self.cfg, [1], self.camera, (0, 0, 0), menu, 1/60)

    def hook(self):
        self.ns['_sync_ray_interceptor'](self.cfg, True)

    def test_silent_changes_only_shot_direction_and_preserves_native_call(self):
        self.hook(); self.frame()
        result = self.cls(self.player).GetShootDir((500, 300))
        self.assertGreater(result[0], 0)
        self.assertAlmostEqual(sum(x*x for x in result), 1.)
        self.assertEqual(self.calls, [(500, 300)])
        self.assertFalse(self.turns); self.assertFalse(self.humanized)
        self.assertEqual(self.prediction_modes, [False])

    def test_aimbot_uses_motion_path_and_removes_silent_hook(self):
        self.hook(); self.cfg[17] = 0; self.hook(); self.frame()
        self.assertIs(self.cls.GetShootDir, self.original)
        self.assertIsNone(self.ns['_ray_target'])
        self.assertEqual(len(self.turns), 1); self.assertEqual(len(self.humanized), 1)
        self.assertEqual(self.prediction_modes, [True])

    def test_silent_rejects_remote_caster_menu_and_missing_activation(self):
        self.hook(); self.frame()
        self.assertEqual(self.cls(object()).GetShootDir((0, 0)), self.native)
        for menu in (True, False):
            self.frame(menu)
            if not menu:
                self.ns['_track_busy'] = False
            self.assertEqual(self.cls(self.player).GetShootDir((0, 0)), self.native)

    def test_silent_rejects_stale_target_space_dead_target_and_out_of_fov(self):
        self.hook(); self.frame()
        valid = self.ns['_ray_target']
        cases = [(self.target, (10, 0, 100), 9., self.space),
                 (self.target, (10, 0, 100), 10., object()),
                 (object(), (10, 0, 100), 10., self.space),
                 (self.target, (100, 0, 0), 10., self.space),
                 (self.target, (0, 0, 500), 10., self.space)]
        for cached in cases:
            self.ns['_ray_target'] = cached
            self.assertEqual(self.cls(self.player).GetShootDir((0, 0)), self.native)
        self.ns['_ray_target'] = valid

    def test_mode_or_enable_flag_change_falls_back_before_next_tick(self):
        self.hook(); self.frame()
        for index in (3, 17):
            saved, self.cfg[index] = self.cfg[index], 0
            self.assertEqual(self.cls(self.player).GetShootDir((0, 0)), self.native)
            self.cfg[index] = saved

    def test_hook_is_idempotent_and_restored_in_hall(self):
        self.hook(); wrapper = self.cls.GetShootDir; self.hook()
        self.assertIs(self.cls.GetShootDir, wrapper)
        self.ns['_sync_ray_interceptor'](self.cfg, False)
        self.assertIs(self.cls.GetShootDir, self.original)

    def test_silent_does_not_require_camera_turn_interface(self):
        self.ns['_get_placer'] = lambda: None
        self.hook(); self.frame()
        self.assertGreater(self.cls(self.player).GetShootDir((0, 0))[0], 0)


class NoRecoilTests(unittest.TestCase):
    def setUp(self):
        class Placer:
            RecoilYawRange = .2
            RecoilPitchRange = .3
            RecoilDownYawRange = .1
            RecoilDownPitchRange = .15
            RecoilDownCyclePct = .5
            forcePersistRecoilRecover = True
            def __init__(self): self.calls = []
            def RecoilUp(self):
                self.calls.append((self.RecoilYawRange, self.RecoilPitchRange, self.RecoilDownCyclePct,
                                   self.forcePersistRecoilRecover))
                return 'native-result'
            RecoilFall = RecoilUp
            RecoilDown = RecoilUp
        self.placer = Placer()
        self.cfg = [0]*40; self.cfg[3] = (1 << 23) | (1 << 13)
        self.ns = functions({'_damp_impulse_call', '_restore_impulse_state', '_sync_impulse_damping'}, {
            '_impulse_hooks': None, 'F_DAMP_IMPULSE': 1 << 23,
            '_flag': lambda cfg, bit: bool(cfg[3] & bit), '_get_placer': lambda: self.placer,
            '_sys_log': lambda *a: None, 'traceback': types.SimpleNamespace(format_exc=lambda: 'error'),
        })

    def test_recoil_suppressed_during_aim_and_original_fields_restored(self):
        self.ns['_sync_impulse_damping'](self.cfg, True)
        for name in ('RecoilUp', 'RecoilFall', 'RecoilDown'):
            self.assertEqual(getattr(self.placer, name)(), 'native-result')
        self.assertEqual(self.placer.calls, [(0., 0., 0., False)]*3)
        self.assertEqual(self.placer.RecoilYawRange, .2)
        self.assertTrue(self.placer.forcePersistRecoilRecover)

    def test_disabled_hall_and_placer_change_restore_methods(self):
        old = self.placer
        self.ns['_sync_impulse_damping'](self.cfg, True)
        self.ns['_sync_impulse_damping'](self.cfg, False)
        self.assertNotIn('RecoilUp', old.__dict__)
        self.ns['_sync_impulse_damping'](self.cfg, True)
        self.placer = type(old)(); self.ns['_sync_impulse_damping'](self.cfg, True)
        self.assertNotIn('RecoilUp', old.__dict__)
        self.cfg[3] = 0; self.ns['_sync_impulse_damping'](self.cfg, True)
        self.assertNotIn('RecoilUp', self.placer.__dict__)

    def test_exception_restores_fields_and_external_wrapper_is_preserved(self):
        def fail(): raise ValueError('native failure')
        with self.assertRaises(ValueError):
            self.ns['_damp_impulse_call'](self.placer, fail, (), {})
        self.assertEqual(self.placer.RecoilPitchRange, .3)
        self.ns['_sync_impulse_damping'](self.cfg, True)
        external = lambda: 'external'
        self.placer.RecoilUp = external
        self.ns['_restore_impulse_state']()
        self.assertIs(self.placer.RecoilUp, external)


if __name__ == '__main__':
    unittest.main()
