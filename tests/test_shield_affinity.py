"""Source-level guards for the Win32-only capture-shield pieces.

`src/protected_output.hpp` needs Win32 + D3D11 + DirectComposition headers, so it
cannot be compiled by the harness that exercises `src/capture_shield.hpp`. Two
behaviours are worth pinning anyway, because both were user-visible bugs:

* a black screen on `Win+Shift+S` - caused by `WDA_EXCLUDEFROMCAPTURE` on the
  shield window, which covers the game, so it must stay opt-in; and
* the shield must hide (not blank, not fall back into the game's back buffer)
  while the snipping tool / Game Bar capture UI is open.
"""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ShieldAffinityTests(unittest.TestCase):
    def setUp(self):
        self.output = (ROOT / 'src' / 'protected_output.hpp').read_text(encoding='utf-8')
        self.overlay = (ROOT / 'src' / 'overlay.cpp').read_text(encoding='utf-8')
        self.cfg = (ROOT / 'src' / 'bs_cfg.hpp').read_text(encoding='utf-8')

    def test_affinity_is_opt_in(self):
        self.assertIn('OPT2_SHIELD_AFFINITY', self.cfg)
        # Create() must take the decision from the caller instead of forcing it.
        self.assertIn('bool exclude) {', self.output)
        self.assertIn('SetExcluded(out, exclude);', self.output)
        self.assertIn('HasOpt2(OPT2_SHIELD_AFFINITY)', self.overlay)
        # No unconditional exclusion left behind on the shield window.
        self.assertNotIn('out.excluded = SetWindowDisplayAffinity(w, WDA_EXCLUDEFROMCAPTURE)',
                         self.output)

    def test_toggle_reaches_the_window_in_the_same_frame(self):
        self.assertIn('comet_surface::SetExcluded(g_surface, HasOpt2(OPT2_SHIELD_AFFINITY))',
                      self.overlay)
        self.assertIn('comet_surface::SetExcluded(g_surface,shield_affinity)', self.overlay)

    def test_capture_ui_detection_covers_snipping_tool_and_game_bar(self):
        self.assertIn('SystemCaptureUiActive', self.output)
        for host in ('ScreenClippingHost.exe', 'SnippingTool.exe', 'ScreenSketch.exe',
                     'GameBar.exe'):
            self.assertIn(host, self.output)

    def test_shield_hides_while_the_capture_ui_is_up(self):
        # ShieldVisible() is the single place both the window and the draw block
        # consult; it must include the capture-UI check.
        start = self.overlay.index('static bool ShieldVisible()')
        body = self.overlay[start:self.overlay.index('\n}', start)]
        self.assertIn('SystemCaptureUiActive', body)
        self.assertIn('Follow(g_surface, g_hwnd, g_client_w, g_client_h, ShieldVisible())',
                      self.overlay)
        self.assertIn('if (!ShieldVisible()) return;', self.overlay)

    def test_present_does_not_block_the_game(self):
        self.assertIn('DXGI_PRESENT_DO_NOT_WAIT', self.output)
        # ...but an unsupported flag must fall back, not stop presenting.
        self.assertIn('DXGI_ERROR_WAS_STILL_DRAWING', self.output)
        # Any refusal retries a blocking present instead of being reported as a
        # failure (which would tear the shield down).
        self.assertIn('out.do_not_wait = false;', self.output)
        self.assertIn('hr = out.swap->Present(0, 0);', self.output)


if __name__ == '__main__':
    unittest.main()
