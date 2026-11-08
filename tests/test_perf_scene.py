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
RUNTIME = (ROOT / 'pyexec' / 'bs_runtime.py').read_text(encoding='utf-8-sig')


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


class AppearanceSyncTests(unittest.TestCase):
    """Skin reconciliation must not run at the frame rate."""

    def test_it_is_throttled(self):
        start = RUNTIME.index('_sync_configured_appearance(cfg)',
                              RUNTIME.index('def _on_story_tick'))
        window = RUNTIME[start - 600:start + 200]
        self.assertIn('0.2', window)
        self.assertIn('_appearance_sync_t', window)

    def test_the_throttle_state_is_module_level(self):
        self.assertIn('\n_appearance_sync_t = 0.0', RUNTIME)


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


class RuntimeTickCostTests(unittest.TestCase):
    """The runtime's own cost per frame, which used to be invisible.

    `[sys:perf]` only fires for a tick over 60 ms, so the tick that runs on the
    game thread every frame - the only place the runtime could cost frames - had
    never been measured. These keep the average in the log.
    """

    def tick_body(self):
        start = RUNTIME.index('def _on_story_tick(dt):')
        return RUNTIME[start:RUNTIME.index('_on_story_tick.__name__', start)]

    def test_every_tick_is_counted(self):
        body = self.tick_body()
        self.assertIn('_perf_tick_count += 1', body)
        self.assertIn('_perf_tick_sum += total', body)

    def test_the_average_is_logged_periodically(self):
        body = self.tick_body()
        self.assertIn("[sys:tick] n=%d avg_ms=%.3f max_ms=%.3f frame_ms=%.2f in_match=%d", body)
        for phase in ('apply=%.3f', 'unlock=%.3f', 'gather=%.3f', 'track=%.3f', 'rest=%.3f'):
            self.assertIn(phase, body)
        self.assertIn('>= 10.0', body)

    def test_the_line_cannot_be_swallowed_by_the_log_throttle(self):
        body = self.tick_body()
        line = body[body.index("[sys:tick]"):][:600]
        self.assertIn('force=True', line)

    def test_the_window_resets(self):
        body = self.tick_body()
        self.assertIn('_perf_tick_window = t0', body)
        self.assertIn('_perf_tick_count = 0', body)

    def test_the_accumulators_are_module_level(self):
        self.assertIn('\n_perf_tick_count = 0', RUNTIME)
        self.assertIn('global _perf_tick_window', RUNTIME)


if __name__ == '__main__':
    unittest.main()
