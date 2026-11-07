"""The capture-protection switch must never mutate state mid-frame, and the
protection itself must not cost the game a frame.

History, both bugs are covered here:

1. The menu is drawn between `comet_surface::Begin()` and `End()`. The old Toggle
   handler destroyed/created the shield surface right there, releasing the bound
   render target and skipping the restore of the game's own targets - the menu
   froze and the game kept drawing into a dead buffer.
2. The shield drew into a second composition swap chain with its own topmost
   window: a second Present per frame plus a DWM-composited surface over the game
   took 240 fps down to 40-65. Protection is now a copy of the game's frame plus a
   GetBuffer substitution, which adds no window and no Present.
"""

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
OVERLAY = ROOT / 'src' / 'overlay.cpp'


class CaptureProtectionTests(unittest.TestCase):

    def setUp(self):
        self.text = OVERLAY.read_text(encoding='utf-8')

    def code(self, start_marker, end_marker):
        start = self.text.index(start_marker)
        end = self.text.index(end_marker, start)
        block = self.text[start:end]
        return '\n'.join(line.split('//')[0] for line in block.splitlines())

    # -- the switch ---------------------------------------------------------
    def test_toggle_only_records_the_wish(self):
        block = self.code('Hide ESP from screen capture', 'if (protect && !g_clean_tex)')
        for forbidden in ('comet_surface::Destroy', 'comet_surface::Create',
                          'MaintainShieldSurface', 'ReleaseCleanCopy', 'RefreshCleanCopy'):
            self.assertNotIn(forbidden, block,
                             'the toggle runs mid-frame; it may only record the wish')

    def test_maintain_does_not_create_a_second_swap_chain(self):
        block = self.code('static void MaintainShieldSurface()', 'static void ApplyObsProtection()')
        self.assertNotIn('comet_surface::Create', block,
                         'a composition swap chain over the game costs the game its frame rate')
        self.assertIn('ReleaseCleanCopy', block,
                      'turning the switch off has to give the kept copy back')

    def test_destroy_refuses_while_a_frame_is_in_flight(self):
        surface = (ROOT / 'src' / 'protected_output.hpp').read_text(encoding='utf-8')
        start = surface.index('inline void Destroy(Output& out)')
        body = surface[start:start + 900]
        self.assertIn('if (out.bound) return;', body)
        self.assertLess(body.index('if (out.bound) return;'),
                        body.index('detail::ReleasePipeline'))

    # -- the protection -----------------------------------------------------
    def test_clean_copy_is_taken_before_the_overlay_is_drawn(self):
        copy = self.text.index('RefreshCleanCopy(back)')
        draw = self.text.index('ImGui_ImplDX11_RenderDrawData', copy)
        self.assertLess(copy, draw,
                        'copying after the draw would capture the overlay itself')

    def test_get_buffer_answers_capture_modules_only(self):
        block = self.code('static HRESULT STDMETHODCALLTYPE HookGetBuffer',
                          'static bool CaptureProtectionKilled() {')
        self.assertIn('CallerIsCaptureModule()', block)
        self.assertIn('CaptureProtectionOn()', block)
        self.assertIn('g_clean_tex', block)
        self.assertIn('g_get_buffer(sc, index, iid, out)', block,
                      'every other caller must get the real back buffer')

    def test_get_buffer_is_hooked_on_slot_nine(self):
        self.assertIn('vt[9]', self.text)
        self.assertIn('MH_CreateHook(vt[9], (void*)&HookGetBuffer', self.text)

    def test_a_desktop_snip_suppresses_the_draw(self):
        self.assertIn('comet_surface::SystemCaptureUiActive()) return;', self.text,
                      'a window-based snip reads the window, so the overlay must not be drawn')

    def test_known_capture_modules_are_listed(self):
        block = self.code('static bool CallerIsCaptureModule()', 'static HRESULT STDMETHODCALLTYPE HookGetBuffer')
        for name in ('graphics-hook64.dll', 'GameOverlayRenderer64.dll'):
            self.assertIn(name, block)


class FrameCostTests(unittest.TestCase):
    """Nothing expensive may run on every presented frame."""

    def test_state_file_read_is_throttled(self):
        text = OVERLAY.read_text(encoding='utf-8')
        start = text.index('// 2. Disk file fallback')
        body = text[start:start + 900]
        self.assertIn('now - last_poll < 8', body,
                      'reading state.bin every frame cost ~5 ms of every frame')


if __name__ == '__main__':
    unittest.main()
