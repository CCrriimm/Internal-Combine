"""The one lever the overlay has over a 60 fps engine: the sync interval it forwards.

The log settled what the 60 was: `sync1=600` of 600 frames with `ui_avg_ms=0.000`,
i.e. the game asking for vsync while the overlay cost nothing, and 226 fps in the
same session as soon as it presented with sync=0. These tests keep the override
narrow (sync=1 only), opt-in, and actually applied to the call that goes to the
driver rather than only to the log line.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
OVERLAY = (ROOT / 'src' / 'overlay.cpp').read_text(encoding='utf-8')
CFG = (ROOT / 'src' / 'bs_cfg.hpp').read_text(encoding='utf-8')


class OptionTests(unittest.TestCase):

    def test_the_bit_is_free(self):
        bits = re.findall(r'OPT2_\w+\s*=\s*1u\s*<<\s*(\d+)', CFG)
        self.assertEqual(len(bits), len(set(bits)), 'two options share a bit')
        self.assertIn('9', bits)

    def test_the_option_is_off_in_the_defaults(self):
        default = OVERLAY[OVERLAY.index('g_cfg.opts = '):]
        default = default[:default.index(';', default.index('g_cfg.opts = '))]
        self.assertNotIn('OPT2_NO_VSYNC', default)

    def test_a_saved_config_does_not_turn_it_on(self):
        for line in OVERLAY.splitlines():
            if '.opts' in line and '|=' in line:
                self.assertNotIn('OPT2_NO_VSYNC', line)


class OverrideTests(unittest.TestCase):

    def helper(self):
        body = OVERLAY[OVERLAY.index('static UINT EffectivePresentSync(UINT sync) {'):]
        return body[:body.index('\n}\n')]

    def test_only_the_vsync_case_is_overridden(self):
        body = self.helper()
        self.assertIn('sync != 1', body)

    def test_it_is_gated_by_the_option(self):
        self.assertIn('HasOpt2(OPT2_NO_VSYNC)', self.helper())

    def test_the_override_reaches_both_present_calls(self):
        self.assertIn('const UINT out_sync = EffectivePresentSync(sync);', OVERLAY)
        self.assertIn('g_present(sc, out_sync, flags)', OVERLAY)
        self.assertIn('g_present1(sc, out_sync, flags, params)', OVERLAY)

    def test_the_log_reports_what_was_presented(self):
        # The perf line must describe the interval actually used, otherwise
        # sync0/sync1 would contradict the frame rate in the same line.
        self.assertIn('NotePresentPerf(sc, "Present", out_sync', OVERLAY)
        self.assertIn('NotePresentPerf(sc, "Present1", out_sync', OVERLAY)

    def test_the_switch_is_reachable_from_the_menu(self):
        start = OVERLAY.index('BeginCard("settings_perf"')
        card = OVERLAY[start:OVERLAY.index('EndCard();', start)]
        self.assertIn('Ignore game V-Sync', card)
        self.assertIn('SetOpt2(OPT2_NO_VSYNC', card)

    def test_the_game_present_argument_is_never_mutated_elsewhere(self):
        """One place changes the interval; a second would make it unexplainable."""
        self.assertEqual(OVERLAY.count('EffectivePresentSync(sync)'), 2)


if __name__ == '__main__':
    unittest.main()
