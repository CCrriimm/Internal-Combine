"""Visibility: per-point raycasts, and the trigger bug that hid airborne targets.

`_line_of_sight_query` mirrors the game's own `IsVisible` - raycast from the
camera to the aim point with `PHYSICS_SHOOT_TEST`, then check the hit body's
owner id. Cover (wall, floor) came out right from the start.

The bug was `with_trigger=True`: `Space.RawRaycast` resolves that to
`ClosestRaycast`, which returns the nearest hit of any kind, trigger volumes
included. A player under a parachute or mid-jump sits inside a skydive/air-drop
trigger, so the nearest hit was the volume - owner id 0 - and a target in plain
sight was reported as "behind a wall" until the moment they landed and left it.
The game itself uses `with_trigger=False` (`ClosestRaycastWithoutTrigger`)
wherever it wants geometry.
"""
import ast
import math

from pathlib import Path
import sys
import time
import types
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1] / 'pyexec' / 'bs_runtime.py'


def build(raycast):
    node = next(n for n in ast.parse(SOURCE.read_text(encoding='utf-8')).body
                if isinstance(n, ast.FunctionDef) and n.name == '_line_of_sight_query')
    ns = {'math': math, 'time': time, '_vis_cache': {},
          '_vis_stats': [0, 0, 0, 0, 0], '_vis_near': [0], '_vis_log_t': 0.0, '_vis_err': '',
          '_vec': lambda p: tuple(p) if p is not None else None,
          '_v3': lambda p: p,
          '_len': lambda v: math.sqrt(sum(x * x for x in v)),
          '_sub': lambda a, b: tuple(x - y for x, y in zip(a, b)),
          '_get_cam': lambda: types.SimpleNamespace(GetOrigin=lambda: (0, 0, 0)),
          'genv': types.SimpleNamespace(space=types.SimpleNamespace(RawRaycast=raycast))}
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), 'exec'), ns)
    # _vis_count is injected with the function's globals; provide it like the module does.
    ns['_vis_count'] = lambda i: None
    package = types.ModuleType('gclient')
    package.cconst = types.SimpleNamespace(PHYSICS_SHOOT_TEST=1)
    return ns, package


class PointVisibilityTests(unittest.TestCase):

    def test_raycasts_requested_points_and_separates_cache(self):
        rays = []
        head, neck = (0, 1.8, 100), (0, 1.5, 100)

        def raycast(origin, distance, mask, flag, to_pos):
            rays.append(to_pos)
            if to_pos == head:
                # Cover well in front of the target, not the target itself.
                return types.SimpleNamespace(IsHit=True, Pos=(0, 1.2, 40.0),
                                             Body=types.SimpleNamespace(ownerid=999))
            return types.SimpleNamespace(IsHit=True, Pos=to_pos,
                                         Body=types.SimpleNamespace(ownerid=2))

        ns, package = build(raycast)
        entity = types.SimpleNamespace(id=2)
        with patch.dict(sys.modules, {'gclient': package}):
            self.assertFalse(ns['_line_of_sight_query'](entity, head, 100, 400))
            self.assertTrue(ns['_line_of_sight_query'](entity, neck, 100, 400))
            self.assertTrue(ns['_line_of_sight_query'](entity, neck, 100, 400))
        self.assertEqual(rays, [head, neck])

    def test_triggers_are_not_part_of_the_ray(self):
        """The flag that made parachuting and jumping targets invisible."""
        seen = []

        def raycast(origin, distance, mask, with_trigger, to_pos):
            seen.append(with_trigger)
            return types.SimpleNamespace(IsHit=False, Pos=None)

        ns, package = build(raycast)
        with patch.dict(sys.modules, {'gclient': package}):
            ns['_line_of_sight_query'](types.SimpleNamespace(id=2), (0, 1.8, 100), 100, 400)
        self.assertEqual(seen, [False], 'ClosestRaycastWithoutTrigger, not ClosestRaycast')

    def test_a_clear_ray_means_visible(self):
        """An airborne target with nothing behind it: no hit, so visible."""
        def raycast(origin, distance, mask, with_trigger, to_pos):
            return None

        ns, package = build(raycast)
        with patch.dict(sys.modules, {'gclient': package}):
            self.assertTrue(ns['_line_of_sight_query'](types.SimpleNamespace(id=2),
                                                       (0, 45.0, 300), 300, 400))

    def test_cover_still_blocks(self):
        """A wall between the camera and the target keeps the old answer."""
        def raycast(origin, distance, mask, with_trigger, to_pos):
            return types.SimpleNamespace(IsHit=True, Pos=(0, 1.0, 20.0),
                                         Body=types.SimpleNamespace(ownerid=0))

        ns, package = build(raycast)
        with patch.dict(sys.modules, {'gclient': package}):
            self.assertFalse(ns['_line_of_sight_query'](types.SimpleNamespace(id=2),
                                                        (0, 1.8, 100), 100, 400))

    def test_a_hit_at_the_target_counts_even_without_the_owner_id(self):
        """Parachute canopy or hitbox shell: right place, foreign owner id."""
        def raycast(origin, distance, mask, with_trigger, to_pos):
            return types.SimpleNamespace(IsHit=True, Pos=(0, 1.7, 99.6),
                                         Body=types.SimpleNamespace(ownerid=0))

        ns, package = build(raycast)
        with patch.dict(sys.modules, {'gclient': package}):
            self.assertTrue(ns['_line_of_sight_query'](types.SimpleNamespace(id=2),
                                                       (0, 1.8, 100), 100, 400))

    def test_a_failing_raycast_does_not_claim_sight(self):
        def raycast(*args, **kwargs):
            raise RuntimeError('no physics space')

        ns, package = build(raycast)
        with patch.dict(sys.modules, {'gclient': package}):
            self.assertFalse(ns['_line_of_sight_query'](types.SimpleNamespace(id=2),
                                                        (0, 1.8, 100), 100, 400))


if __name__ == '__main__':
    unittest.main()


class VisibilityAccountingTests(unittest.TestCase):
    """[sys:vis] has to say *why* a check did not produce an answer.

    The first version counted a blocked ray as "none" because the cover outcome
    was never assigned, so the log read `cover=0 none=457` while walls were being
    detected perfectly. A tally nobody can read is worse than no tally.
    """

    def setUp(self):
        self.src = (Path(__file__).resolve().parents[1] / 'pyexec' /
                    'bs_runtime.py').read_text(encoding='utf-8-sig')

    def test_cover_is_counted_as_cover(self):
        body = self.src[self.src.index('def _line_of_sight_query'):]
        body = body[:body.index('\ndef ')]
        self.assertIn('v, outcome = False, 2', body, 'a blocked ray is cover')

    def test_a_missing_camera_is_its_own_outcome(self):
        body = self.src[self.src.index('def _line_of_sight_query'):]
        body = body[:body.index('\ndef ')]
        self.assertIn('outcome = 4', body)
        self.assertIn('nocam=', self.src)

    def test_every_counter_is_reset_with_the_log(self):
        body = self.src[self.src.index('def _log_vis_stats'):]
        body = body[:body.index('\ndef ')]
        self.assertIn('range(5)', body,
                      'the no-camera counter must reset with the others')
