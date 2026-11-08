"""The perf line has to describe the scene, not just the frame rate.

`fps=60` on its own cannot be judged: it is the same number whether the map is
heavy, the game asked for vsync, or our own per-entity drawing is expensive. These
tests keep the three pieces of context in the log that tell them apart.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
OVERLAY = (ROOT / 'src' / 'overlay.cpp').read_text(encoding='utf-8')


class PerfLineTests(unittest.TestCase):

    def perf_format(self):
        start = OVERLAY.index('Log("perf %s frames=')
        return OVERLAY[start:OVERLAY.index(';', start)]

    def test_the_scene_is_reported_with_the_frame_rate(self):
        fmt = self.perf_format()
        for field in ('ents=%u', 'menu=%d', 'esp=%d'):
            self.assertIn(field, fmt)

    def test_vsync_is_counted_separately_from_performance(self):
        fmt = self.perf_format()
        self.assertIn('sync0=%u', fmt)
        self.assertIn('sync1=%u', fmt)

    def test_the_overlay_cost_is_split_into_state_and_drawing(self):
        fmt = self.perf_format()
        for field in ('overlay_avg_ms=', 'ui_avg_ms=', 'state_avg_ms='):
            self.assertIn(field, fmt)

    def test_arguments_match_the_format_string(self):
        """A format string with the wrong number of arguments logs garbage."""
        start = OVERLAY.index('Log("perf %s frames=')
        end = OVERLAY.index('}', OVERLAY.index('modules=%d",', start))
        block = OVERLAY[start:end]
        fmt = block[block.index('"'):block.index('",')]
        conversions = len(re.findall(r'%[-0-9.]*(?:ll|h|l)?[dufsx]', fmt))
        args = block[block.index('which,'):]
        args = args[:args.rindex(')')]
        depth = 0
        count = 0
        for ch in args:
            if ch in '([':
                depth += 1
            elif ch in ')]':
                depth -= 1
            elif ch == ',' and depth == 0:
                count += 1
        self.assertEqual(count + 1, conversions)


class DisplayModeTests(unittest.TestCase):

    def test_the_display_mode_is_logged_when_the_swapchain_binds(self):
        self.assertIn('static void LogDisplayMode(bool force);', OVERLAY)
        bind = OVERLAY[OVERLAY.index('static bool BindSwapchain'):]
        bind = bind[:bind.index('static void UpdateClientSize')]
        self.assertIn('LogDisplayMode(true);', bind)

    def test_the_display_mode_is_rechecked_with_every_perf_line(self):
        self.assertIn('LogDisplayMode(false);', OVERLAY)

    def test_both_the_swapchain_and_the_panel_are_reported(self):
        body = OVERLAY[OVERLAY.index('static void LogDisplayMode(bool force) {'):]
        body = body[:body.index('static void NotePresentPerf')]
        self.assertIn('GetDesc(&desc)', body)
        self.assertIn('EnumDisplaySettingsA', body)
        self.assertIn('chain_hz', body)
        self.assertIn('desktop_hz', body)

    def test_the_mode_is_not_logged_every_frame(self):
        """Log() is a synchronous disk write and must not run inside Present."""
        body = OVERLAY[OVERLAY.index('static void LogDisplayMode(bool force) {'):]
        body = body[:body.index('static void NotePresentPerf')]
        self.assertIn('if (!force && chain_hz == last_chain', body)


if __name__ == '__main__':
    unittest.main()
