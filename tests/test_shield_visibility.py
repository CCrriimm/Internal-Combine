"""Source-level guards for the Win32-only capture shield and its settings card.

`src/protected_output.hpp` needs Win32 + D3D11 + DirectComposition headers, so it
cannot be compiled by the harness that exercises `src/capture_shield.hpp`. What is
pinned here is the behaviour the user asked for, in their words: "don't display
the ESP on screencaptures", "no black screen", and "one button".

* no display affinity anywhere: `WDA_EXCLUDEFROMCAPTURE` is what DWM paints black,
  so the shield and the game window both stay affinity-free;
* a screenshot hides the shield instead - Win+Shift+S / PrtScn are read straight
  from the keyboard, and the snip overlay process/window are watched as well;
* the Protection card is a single toggle plus three feature flags - no GDI guard
  switch, no game-window exclusion switch, no paragraphs;
* the menu has an explicit drag strip and corner grip (the window has no title
  bar, so there was nothing to grab).
"""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ShieldVisibilityTests(unittest.TestCase):
    def setUp(self):
        self.output = (ROOT / 'src' / 'protected_output.hpp').read_text(encoding='utf-8')
        self.overlay = (ROOT / 'src' / 'overlay.cpp').read_text(encoding='utf-8')

    def test_nothing_can_be_painted_black(self):
        # No affinity on the shield window and none on the game window: either one
        # is what turns a screenshot into a black rectangle.
        self.assertNotIn('SetWindowDisplayAffinity(w, WDA_EXCLUDEFROMCAPTURE)', self.output)
        self.assertIn('const bool want = false;', self.overlay)
        self.assertNotIn('OPT2_OBS_PROTECT | OPT2_CAPTURE_SHIELD', self.overlay)

    def test_screenshots_hide_the_shield(self):
        self.assertIn('SystemCaptureUiActive', self.output)
        # The hotkey is read directly, so the shield is gone before the tool grabs
        # the composed desktop.
        self.assertIn('ScreenshotHotkeyRecentlyDown', self.output)
        self.assertIn('VK_SNAPSHOT', self.output)
        self.assertIn("GetAsyncKeyState('S')", self.output)
        self.assertIn('SnippingOverlayProcessRunning', self.output)
        self.assertIn('ScreenClippingHost.exe', self.output)
        for host in ('SnippingTool.exe', 'ScreenSketch.exe', 'GameBar.exe'):
            self.assertIn(host, self.output)

    def test_shield_hides_while_the_capture_ui_is_up(self):
        start = self.overlay.index('static bool ShieldVisible()')
        body = self.overlay[start:self.overlay.index('\n}', start)]
        self.assertIn('SystemCaptureUiActive', body)
        self.assertIn('Follow(g_surface, g_hwnd, g_client_w, g_client_h, ShieldVisible())',
                      self.overlay)
        self.assertIn('if (!ShieldVisible()) return;', self.overlay)

    def test_protection_card_is_one_toggle(self):
        start = self.overlay.index('BeginCard("settings_protection"')
        end = self.overlay.index('BeginCard("settings_session"', start)
        card = self.overlay[start:end]
        self.assertEqual(card.count('CometUi::Toggle'), 1, 'one switch for one behaviour')
        self.assertIn('Hide ESP from screen capture', card)
        # No paragraphs, and no leftover controls for the retired switches.
        for banned in ('ImGui::TextWrapped', 'ImGui::Text(',
                       '"Delay GDI captures"', '"Windows capture exclusion',
                       'Capture modules', 'Present chain', 'Pacing:'):
            self.assertNotIn(banned, card, 'the Protection card stays minimal: %r' % banned)

    def test_menu_can_be_moved_and_resized(self):
        self.assertIn('##menu_move', self.overlay)
        self.assertIn('##menu_resize', self.overlay)
        self.assertIn('ImGui::SetWindowPos(ImVec2(wp.x + delta.x, wp.y + delta.y))', self.overlay)
        self.assertIn('ImGui::SetWindowSize(', self.overlay)


if __name__ == '__main__':
    unittest.main()
