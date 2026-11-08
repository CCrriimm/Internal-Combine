"""The overlay's own world -> screen projection, and the measurement behind it.

A skeleton used to cost eighteen `gui.WorldToScreenPercent` calls per gather (one
head, one feet, sixteen joints). The overlay has the camera position, forward
vector, fov and screen size already, so it does the maths itself; the engine's
answer is still used for the box, which is what the calibration keeps checking.

Two details were only knowable by measurement: which handedness the camera basis
has, and whether the engine's fov is vertical or horizontal. The 2026-11 log
settled both - `right = forward x worldUp` with the focal from the *vertical* fov
matched to 0.01-0.19 px, the other three conventions were 59-652 px out - so the
draw path uses that variant and disables itself if the error ever grows.
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

    def test_the_skeleton_projects_its_own_bones(self):
        """Sixteen engine projections per skeleton became sixteen multiplies."""
        draw = OVERLAY[OVERLAY.index('static void DrawESP() {'):]
        skel = draw[draw.index('if (Has(F_ESP_SKELETON) && !is_knocked)'):]
        skel = skel[:skel.index('float y = hy - (esp_font_sz + 6.0f);')]
        self.assertIn('ProjectWorld(proj, b1, s1)', skel)
        self.assertIn('ProjectWorld(proj, b2, s2)', skel)
        self.assertIn('ProjectWorld(proj, b, sp)', skel)
        # Screen widths must never multiply a world coordinate again.
        self.assertNotIn('b[0] * W', skel)
        self.assertNotIn('b[0] * W', draw)

    def test_the_draw_frame_is_built_once_per_frame(self):
        draw = OVERLAY[OVERLAY.index('static void DrawESP() {'):]
        self.assertEqual(draw.count('MakeProjFrame(proj,'), 1)
        self.assertLess(draw.index('MakeProjFrame(proj,'), draw.index('for (auto it = g_ents.rbegin()'))

    def test_the_draw_frame_uses_the_measured_convention(self):
        draw = OVERLAY[OVERLAY.index('static void DrawESP() {'):]
        self.assertIn('MakeProjFrame(proj, 1, true)', draw)
        body = self.body()
        # 1 = right = forward x worldUp, true = fov is vertical: the winner.
        self.assertIn('{"fwdxup,vfov", 1, true},', body)

    def test_the_labels_cannot_disagree_with_the_arguments(self):
        """The first log named the wrong winner: labels were derived twice."""
        body = self.body()
        self.assertIn('struct Variant { const char* name; int handedness; bool fov_is_vertical; };', body)
        self.assertIn('VARIANTS[v].handedness, VARIANTS[v].fov_is_vertical', body)
        self.assertIn('VARIANTS[best].name', body)
        self.assertNotIn('names[4]', body)

    def test_a_wrong_calibration_disables_the_skeleton(self):
        body = self.body()
        self.assertIn('const bool trusted = best_mean < 40.0;', body)
        self.assertIn('g_proj_ok = trusted;', body)
        draw = OVERLAY[OVERLAY.index('static void DrawESP() {'):]
        skel = draw[draw.index('bool has_bones = false;'):]
        self.assertIn('bi < 16 && g_proj_ok', skel)

    def test_the_bones_arrive_as_world_space_triples(self):
        runtime = (ROOT / 'pyexec' / 'bs_runtime.py').read_text(encoding='utf-8-sig')
        self.assertIn('<Q64s32s3f3f2f2f5fi4BI48fII', runtime)
        self.assertIn('bones_2d.extend((-1.0, -1.0, -1.0))', runtime)
        body = runtime[runtime.index('def _query_visible_entities'):]
        body = body[:body.index('def _owner_type_name')]
        joints = body[body.index('for bi in range(16):'):]
        joints = joints[:joints.index('t_bone +=')]
        self.assertNotIn('_matrix_world_to_screen', joints, 'joints are no longer projected in Python')
        self.assertIn('float bones[16][3];', OVERLAY)

    def test_calibration_compares_against_the_engine(self):
        body = self.body()
        self.assertIn('e.head_screen', body)
        self.assertIn('Log("[proj] samples=%d best=%s', body)

    def test_calibration_is_throttled_and_independent_per_variant(self):
        body = self.body()
        self.assertIn('now - last < 10000', body)
        self.assertIn('static const Variant VARIANTS[4]', body)

    def test_it_is_called_from_the_frame(self):
        self.assertIn('    LogProjectionCalibration();\n', OVERLAY)


if __name__ == '__main__':
    unittest.main()
