"""The Protection toggle must not touch the surface mid-frame.

The menu is drawn between `comet_surface::Begin()` and `comet_surface::End()`,
so a Toggle handler runs with the shield's render target bound. Tearing the
surface down there released the bound view and the swap chain while the frame's
draw data was already recorded against them, and skipped the restore of the
game's render targets (both are kept inside the Output for the duration of the
frame). The visible result was a menu that stopped responding and a game drawing
into a dead buffer - the crash the user reported when they pressed the switch.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
OVERLAY = ROOT / 'src' / 'overlay.cpp'
SURFACE = ROOT / 'src' / 'protected_output.hpp'


class ToggleLifetimeTests(unittest.TestCase):

    def setUp(self):
        self.overlay = OVERLAY.read_text(encoding='utf-8')
        self.surface = SURFACE.read_text(encoding='utf-8')

    def toggle_block(self):
        start = self.overlay.index('Hide ESP from screen capture')
        end = self.overlay.index('if (protect && !ShieldSurfaceReady())', start)
        block = self.overlay[start:end]
        # comments explain the rule; only code counts
        return '\n'.join(line.split('//')[0] for line in block.splitlines())

    def test_toggle_does_not_touch_the_surface(self):
        block = self.toggle_block()
        self.assertNotIn('comet_surface::Destroy', block,
                         'the toggle runs mid-frame; it may only record the wish')
        self.assertNotIn('MaintainShieldSurface', block,
                         'the toggle runs mid-frame; it may only record the wish')

    def test_maintain_defers_while_bound(self):
        start = self.overlay.index('static void MaintainShieldSurface()')
        end = self.overlay.index('static void ApplyObsProtection()', start)
        body = self.overlay[start:end]
        self.assertIn('g_surface.bound', body)
        self.assertLess(body.index('g_surface.bound'), body.index('comet_surface::Create'),
                        'the bound check has to run before the surface is created')

    def test_destroy_refuses_while_bound(self):
        start = self.surface.index('inline void Destroy(Output& out)')
        body = self.surface[start:start + 900]
        self.assertIn('if (out.bound) return;', body)
        self.assertLess(body.index('if (out.bound) return;'),
                        body.index('detail::ReleasePipeline'),
                        'the guard has to run before anything is released')

    def test_begin_and_end_bracket_the_draw(self):
        begin = self.overlay.index('comet_surface::Begin(g_surface')
        end = self.overlay.index('comet_surface::End(g_surface')
        draw = self.overlay.index('ImGui_ImplDX11_RenderDrawData', begin)
        self.assertLess(begin, draw)
        self.assertLess(draw, end)


if __name__ == '__main__':
    unittest.main()
