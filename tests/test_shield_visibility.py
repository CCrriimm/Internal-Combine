"""Source-level guards for the Win32-only capture shield and its settings card.

`src/protected_output.hpp` needs Win32 + D3D11 + DirectComposition headers, so it
cannot be compiled by the harness that exercises `src/capture_shield.hpp`. The
behaviours pinned here are all user-visible:

* a `Win+Shift+S` snip must come back as a clean game frame - no overlay, and no
  black rectangle (Windows renders an excluded window black, so the shield hides
  itself while the snipping tool / Game Bar capture UI is open);
* `capture exclusion on` on the shield window stays, because that is what keeps
  OBS Display Capture and Game Bar out of the overlay, and the shield is
  composited by DWM, which is the flicker-free path while the camera turns;
* the present path stays blocking - a skipped present leaves a stale ESP frame
  behind and reads as the overlay "trailing" the camera;
* the Protection card carries controls and short status lines only.
"""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ShieldVisibilityTests(unittest.TestCase):
    def setUp(self):
        self.output = (ROOT / 'src' / 'protected_output.hpp').read_text(encoding='utf-8')
        self.overlay = (ROOT / 'src' / 'overlay.cpp').read_text(encoding='utf-8')
        self.cfg = (ROOT / 'src' / 'bs_cfg.hpp').read_text(encoding='utf-8')

    def test_shield_window_is_still_excluded_at_creation(self):
        self.assertIn('SetWindowDisplayAffinity(w, WDA_EXCLUDEFROMCAPTURE)', self.output)
        self.assertNotIn('OPT2_SHIELD_AFFINITY', self.cfg)
        self.assertNotIn('OPT2_SHIELD_AFFINITY', self.overlay)

    def test_capture_ui_detection_covers_snipping_tool_and_game_bar(self):
        self.assertIn('SystemCaptureUiActive', self.output)
        for host in ('ScreenClippingHost.exe', 'SnippingTool.exe', 'ScreenSketch.exe',
                     'GameBar.exe'):
            self.assertIn(host, self.output)
        # A small capture window counts while it owns the foreground: the Win11
        # "new snip" popup is not full-screen.
        self.assertIn('GetForegroundWindow() != window', self.output)

    def test_shield_hides_while_the_capture_ui_is_up(self):
        start = self.overlay.index('static bool ShieldVisible()')
        body = self.overlay[start:self.overlay.index('\n}', start)]
        self.assertIn('SystemCaptureUiActive', body)
        self.assertIn('Follow(g_surface, g_hwnd, g_client_w, g_client_h, ShieldVisible())',
                      self.overlay)
        self.assertIn('if (!ShieldVisible()) return;', self.overlay)

    def test_present_is_blocking_not_skipping(self):
        self.assertNotIn('DXGI_PRESENT_DO_NOT_WAIT', self.output)
        self.assertIn('out.swap->Present(0, 0)', self.output)

    def test_protection_card_has_no_paragraphs(self):
        start = self.overlay.index('BeginCard("settings_protection"')
        end = self.overlay.index('BeginCard("settings_session"', start)
        card = self.overlay[start:end]
        for banned in ('ImGui::TextWrapped', 'ImGui::Text(', 'ImGui::Bullet',
                       'ImGui::TextUnformatted'):
            self.assertNotIn(banned, card,
                             'the Protection card is controls + short status lines only')


if __name__ == '__main__':
    unittest.main()
