"""The overlay's own world -> screen projection, measured before it is used.

Every drawn point is currently projected by the game (`gui.WorldToScreenPercent`),
and a player with a skeleton costs 18 of those per gather. The overlay already has
the camera position, forward vector, fov and screen size, so it can do the maths
itself - but two details are only knowable by measurement: which handedness the
camera basis has, and whether the engine's fov is vertical or horizontal. Until
that is settled in the log, this code draws nothing.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
OVERLAY = (ROOT / 'src' / 'overlay.cpp').read_text(encoding='utf-8')


class ProjectionTests(unittest.TestCase):

    def body(self):
        start = OVERLAY.index('struct ProjFrame {')
        return OVERLAY[start:OVERLAY.index('static void DrawESP() {')]

    def test_the_projection_uses_what_the_state_already_carries(self):
        body = self.body()
        for field in ('g_state.cam_pos', 'g_state.cam_forward', 'g_state.fov',
                      'g_state.screen_w', 'g_state.screen_h'):
            self.assertIn(field, body)

    def test_it_matches_the_reference_formula(self):
        body = self.body()
        # focal = (half screen) / tan(fov/2); x = cx + (px/pz)*focal; y = cy - (py/pz)*focal
        self.assertIn('f.focal = half / std::tan(fov * 0.017453292f * 0.5f)', body)
        self.assertIn('out_px[0] = f.w * 0.5f + (px / pz) * f.focal', body)
        self.assertIn('out_px[1] = f.h * 0.5f - (py / pz) * f.focal', body)

    def test_points_behind_the_camera_are_rejected(self):
        self.assertIn('if (pz <= 0.01f) return false;', self.body())

    def test_it_draws_nothing_until_calibrated(self):
        """The calibration must not feed the draw path in this build."""
        self.assertNotIn('ProjectWorld(', OVERLAY[OVERLAY.index('static void DrawESP() {'):])

    def test_calibration_compares_against_the_engine(self):
        body = self.body()
        self.assertIn('e.head_screen', body)
        self.assertIn('Log("[proj] samples=%d best=%s', body)

    def test_calibration_is_throttled_and_independent_per_variant(self):
        body = self.body()
        self.assertIn('now - last < 10000', body)
        self.assertIn('const char* names[4]', body)

    def test_it_is_called_from_the_frame(self):
        self.assertIn('    LogProjectionCalibration();\n', OVERLAY)


if __name__ == '__main__':
    unittest.main()
