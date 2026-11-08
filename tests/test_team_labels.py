"""Team label + per-team colours.

The engine hands out team ids as opaque strings - the user's log shows
`team=au+R9FvWlF0VveRQ` - and the runtime used to do `int(team)`, which raised and
left every entity at team 0. With team 0 the overlay can neither tint a target per
team nor print "Team 1 / Team 2", which is exactly what the user reported.

The runtime now numbers teams in first-seen order per match, seeded with the local
team so Team 1 is always the player's own, and the overlay reserves both box
colours so a generated tint can never be confused with visible/hidden.
"""

import ast
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'pyexec' / 'bs_runtime.py'
OVERLAY = ROOT / 'src' / 'overlay.cpp'


def load_team_number():
    tree = ast.parse(RUNTIME.read_text(encoding='utf-8-sig'))
    nodes = [n for n in tree.body
             if isinstance(n, ast.Assign) and any(getattr(t, 'id', '') == '_team_numbers'
                                                  for t in n.targets)]
    nodes += [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == '_team_number']
    namespace = {'_team_numbers': {}}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(RUNTIME), 'exec'), namespace)
    return namespace['_team_number']


class TeamNumberingTests(unittest.TestCase):

    def setUp(self):
        self.number = load_team_number()

    def test_opaque_team_ids_get_small_numbers_in_first_seen_order(self):
        self.assertEqual(self.number('au+R9FvWlF0VveRQ'), 1)
        self.assertEqual(self.number('au+X8UGT3VJlUgBp'), 2)
        self.assertEqual(self.number('au+R9FvWlF0VveRQ'), 1, 'stable for the match')

    def test_the_local_team_is_one(self):
        self.number('au+local')          # seeded first, as the runtime does
        self.assertEqual(self.number('au+enemy'), 2)
        self.assertEqual(self.number('au+local'), 1)

    def test_missing_ids_are_zero_not_a_new_team(self):
        self.assertEqual(self.number(None), 0)
        self.assertEqual(self.number(''), 0)
        self.assertEqual(self.number('0'), 0)
        self.assertEqual(self.number('-1'), 0)

    def test_a_new_match_starts_over(self):
        self.number('au+a')
        self.assertEqual(self.number('au+b'), 2)
        self.number(None, reset=True)
        self.assertEqual(self.number('au+b'), 1)

    def test_the_snapshot_no_longer_parses_the_team_id_as_an_int(self):
        text = RUNTIME.read_text(encoding='utf-8-sig')
        self.assertNotIn('team_i = int(eteam or 0)', text)
        self.assertIn('team_i = _team_number(eteam)', text)


class OverlayTeamColourTests(unittest.TestCase):

    def setUp(self):
        self.text = OVERLAY.read_text(encoding='utf-8')

    def test_both_box_colours_are_reserved_from_the_tint_pool(self):
        body = self.text[self.text.index('static ReservedColours BuildReservedColours()'):]
        body = body[:body.index('static ImU32 TeamTint')]
        self.assertIn('col_box &', body)
        self.assertIn('col_box_hidden &', body,
                      'a tint must never be mistakable for "hidden" either')

    def test_the_box_keeps_the_visibility_colour_when_the_check_is_on(self):
        body = self.text[self.text.index('const ImU32 tint ='):]
        body = body[:body.index('float th = g_cfg.box_thickness;')]
        self.assertIn('if (!esp_show_visible) col = tint;', body,
                      'colour must still say visible / not visible when the check is on')
        self.assertIn('scol = tint;', body, 'the tint goes on the skeleton and the line')

    def test_the_label_is_shown_for_every_player(self):
        body = self.text[self.text.index('if (team_label_on'):]
        body = body[:body.index('// The header above the box')]
        self.assertIn('!e.is_monster', body)
        self.assertNotIn('!e.is_team', body,
                         'the label names the team, so it applies to both sides')

    def test_the_label_uses_the_team_colour(self):
        body = self.text[self.text.index('if (team_label_on'):]
        body = body[:body.index('// The header above the box')]
        self.assertIn('team_tint ? tint : g_cfg.col_team', body)


class MenuLayoutTests(unittest.TestCase):

    def setUp(self):
        self.text = OVERLAY.read_text(encoding='utf-8')

    def player_overlay_card(self):
        start = self.text.index('Section("Player overlay")')
        end = self.text.index('BeginCard("visuals_filters"', start)
        return self.text[start:end]

    def test_the_extras_live_in_the_player_overlay_card(self):
        card = self.player_overlay_card()
        for label in ('Enemy hero name', 'Team label', 'Team box + line colors'):
            self.assertIn(label, card)

    def test_the_extras_card_and_its_notes_are_gone(self):
        self.assertNotIn('visuals_extras', self.text)
        self.assertNotIn('ESP extras', self.text)
        for note in ('Team colours are assigned per team',
                     'Armor bar colour follows max armor',
                     'Armour colour is automatic'):
            self.assertNotIn(note, self.text)


if __name__ == '__main__':
    unittest.main()
