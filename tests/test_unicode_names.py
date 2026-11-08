"""Player names with Chinese, Japanese, Korean, emoji and symbols.

Three things had to line up, and any one of them missing rendered a name as boxes
or as the literal `b'\\xd6\\xd0'`:

1. The engine's name is not always a `str`; a Chinese client hands over GBK
   bytes. `str(bytes)` turns that into Python's repr.
2. The packet field was 40 bytes, which is 13 CJK characters.
3. The atlas was built with `GetGlyphRangesDefault()`, i.e. Latin only.
"""

import ast
import pathlib
import struct
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'pyexec' / 'bs_runtime.py'
OVERLAY = ROOT / 'src' / 'overlay.cpp'


def load_runtime_function(name, extra=None):
    tree = ast.parse(RUNTIME.read_text(encoding='utf-8-sig'))
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == name]
    namespace = dict(extra or {})
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(RUNTIME), 'exec'), namespace)
    return namespace[name]


class NameDecodingTests(unittest.TestCase):

    def setUp(self):
        self.text = load_runtime_function('_text', {
            '_preferred_name_encoding': lambda: 'gbk', '_name_encoding': 'gbk'})

    def test_utf8_name_survives(self):
        self.assertEqual(self.text('玩家一号'.encode('utf-8')), '玩家一号')

    def test_gbk_bytes_are_decoded_not_reprd(self):
        self.assertEqual(self.text('玩家一号'.encode('gbk')), '玩家一号')

    def test_korean_client(self):
        text = load_runtime_function('_text', {
            '_preferred_name_encoding': lambda: 'cp949', '_name_encoding': 'cp949'})
        self.assertEqual(text('플레이어'.encode('cp949')), '플레이어')

    def test_japanese_client(self):
        text = load_runtime_function('_text', {
            '_preferred_name_encoding': lambda: 'shift_jis', '_name_encoding': 'shift_jis'})
        self.assertEqual(text('プレイヤー'.encode('shift_jis')), 'プレイヤー')

    def test_japanese_text(self):
        self.assertEqual(self.text('プレイヤー'), 'プレイヤー')

    def test_emoji_survive(self):
        self.assertEqual(self.text('xX_🔥_Killer'), 'xX_🔥_Killer')

    def test_control_characters_are_dropped(self):
        self.assertEqual(self.text('a\x00b\x1fc'), 'abc')

    def test_none_is_empty(self):
        self.assertEqual(self.text(None), '')


class PacketTests(unittest.TestCase):

    def setUp(self):
        self.runtime = RUNTIME.read_text(encoding='utf-8-sig')
        self.overlay = OVERLAY.read_text(encoding='utf-8')

    def test_the_name_field_holds_cjk_names(self):
        self.assertIn("char name[64];", self.overlay)
        self.assertIn('<Q64s32s', self.runtime)

    def test_the_two_sides_agree_on_the_entity_layout(self):
        fmt = self.runtime[self.runtime.index('ENT_FMT = '):]
        fmt = fmt[:fmt.index('\n')].split("'")[1]
        self.assertEqual(struct.calcsize(fmt), 376, 'must match sizeof(Ent) in overlay.cpp')
        self.assertIn('static_assert(sizeof(Ent) == 376', self.overlay)

    def test_both_sides_carry_the_same_state_version(self):
        tail = self.runtime[self.runtime.index('\nVERSION = ') + 1:]
        version = tail[:tail.index('\n')].split('=')[1].strip()
        self.assertIn('static const uint32_t VERSION = %s;' % version, self.overlay)

    def test_the_name_is_written_as_utf8_with_the_full_field(self):
        self.assertIn('_bytes(_ename(e), 64)', self.runtime)


class FontCoverageTests(unittest.TestCase):

    def setUp(self):
        self.text = OVERLAY.read_text(encoding='utf-8')

    def test_the_name_font_is_not_latin_only(self):
        block = self.text[self.text.index('namespace fontcov'):]
        block = block[:block.index('static void InitFonts()')]
        for call in ('GetGlyphRangesChineseSimplifiedCommon',
                     'GetGlyphRangesJapanese',
                     'GetGlyphRangesKorean'):
            self.assertIn(call, block)

    def test_symbol_and_emoji_blocks_are_covered(self):
        block = self.text[self.text.index('namespace fontcov'):]
        block = block[:block.index('static void InitFonts()')]
        for pair in ('0x2190, 0x21FF', '0x2600, 0x26FF', '0x2700, 0x27BF',
                     '0x1F300, 0x1F5FF', '0x1F600, 0x1F64F'):
            self.assertIn(pair, block)

    def test_cjk_fonts_are_merged_for_names(self):
        block = self.text[self.text.index('MergeNameFallbacks'):]
        block = block[:block.index('} // namespace fontcov')]
        for font in ('msyh.ttc', 'malgun.ttf', 'seguisym.ttf'):
            self.assertIn(font, block)

    def test_thai_names_have_a_font(self):
        """A Thai name was drawing as empty boxes: no Latin or CJK face has it."""
        block = self.text[self.text.index('namespace fontcov'):]
        block = block[:block.index('static void InitFonts()')]
        self.assertIn('GetGlyphRangesThai()', block)
        merged = self.text[self.text.index('MergeNameFallbacks'):]
        merged = merged[:merged.index('} // namespace fontcov')]
        self.assertTrue(any(f in merged for f in ('leelawui.ttf', 'leelawad.ttf', 'tahoma.ttf')),
                        'Thai needs Leelawadee or Tahoma merged into the name font')

    def test_the_menu_font_stays_cheap(self):
        """Only the name font pays for CJK; the menu has no user text."""
        block = self.text[self.text.index('static void InitFonts()'):]
        block = block[:block.index('static SHORT RawAsyncKeyState')]
        ui = block[:block.index('g_font_big')]
        self.assertIn('fontcov::Latin()', ui)
        self.assertNotIn('fontcov::Names()', ui)

    def test_32_bit_imwchar_is_enabled_for_every_imgui_translation_unit(self):
        cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        for target in ('comet_overlay', 'comet_loader'):
            line = next(l for l in cmake.splitlines()
                        if l.startswith('target_compile_definitions(%s ' % target))
            self.assertIn('IMGUI_USE_WCHAR32', line)


if __name__ == '__main__':
    unittest.main()


class SymbolCoverageTests(unittest.TestCase):
    """The characters between Latin-1 and Greek.

    A clan tag like "KNS ʚ Saejin" is IPA - U+029A - and the default glyph
    ranges stop at U+00FF, then resume at Greek U+0370. Everything in between was
    baked as nothing and skipped silently, so the tag vanished and the name read
    as "KNS  Saejin". These pin the ranges that cover it, and the reporter that
    names any codepoint still missing instead of drawing a gap.
    """

    def setUp(self):
        self.src = OVERLAY.read_text(encoding='utf-8', errors='replace')

    def _body(self, name):
        start = self.src.index('inline ImVector<ImWchar>& %s()' % name)
        return self.src[start:self.src.index('\n}', start)]

    def test_the_ipa_and_modifier_blocks_are_covered(self):
        body = self._body('Symbols')
        self.assertIn('0x0250, 0x02FF', body, 'IPA extensions + spacing modifiers')
        self.assertIn('0x0300, 0x036F', body, 'combining diacritical marks')

    def test_small_capitals_are_covered(self):
        body = self._body('Symbols')
        self.assertIn('0x1D00, 0x1DBF', body, 'phonetic extensions, used as decoration')

    def test_the_name_font_asks_for_the_symbols(self):
        body = self._body('Names')
        self.assertIn('Symbols().Data', body,
                      'ranges only apply to the font that asks for them')

    def test_a_missing_glyph_is_logged_once(self):
        self.assertIn('static void NoteMissingGlyphs', self.src)
        body = self.src[self.src.index('static void NoteMissingGlyphs'):]
        body = body[:body.index('\n}\n')]
        self.assertIn('FindGlyph(', body)
        self.assertIn('g->Codepoint == c', body, 'the fallback glyph answers for a miss')
        self.assertIn('Log("[font] no glyph for U+%04X', self.src)
        # And it has to actually run on the strings that get drawn.
        chip = self.src[self.src.index('auto chip = [&]'):]
        chip = chip[:chip.index('};')]
        self.assertIn('NoteMissingGlyphs(', chip)

    def test_the_atlas_size_is_reported(self):
        self.assertIn('atlas=%dx%d', self.src,
                      'an atlas overflow drops glyphs without an error')


class NameStyleTests(unittest.TestCase):
    """The name line is drawn like the game's own nameplate: text, black halo, no
    plate. The reference screenshot is red bold text with a black outline."""

    def setUp(self):
        self.src = OVERLAY.read_text(encoding='utf-8', errors='replace')

    def body(self):
        return self.src[self.src.index('auto outlined = [&]'):
                        self.src.index('if (Has(F_ESP_LINE))')]

    def test_the_name_goes_through_the_outlined_draw(self):
        self.assertIn('outlined(cx, y, e.name, g_cfg.col_name)', self.src)

    def test_the_outline_is_a_ring_of_black_draws(self):
        body = self.body()
        self.assertIn('static const float OFF[8][2]', body)
        self.assertIn('for (int i = 0; i < 8; ++i)', body)
        self.assertIn('IM_COL32(0, 0, 0, 255)', body)
        self.assertNotIn('AddRectFilled', body, 'no plate behind the name')

    def test_the_fill_comes_last(self):
        body = self.body()
        self.assertLess(body.index('for (int i = 0; i < 8; ++i)'),
                        body.rindex('dl->AddText(esp_font, esp_font_sz, p, fg, s)'),
                        'the halo is drawn under the fill')

    def test_only_the_name_pays_for_the_halo(self):
        """Nine draws per target is fine; nine per chip is not - there are four."""
        self.assertNotIn('outlined(cx, y, e.gun', self.src)
        self.assertNotIn('outlined(cx, foot_y', self.src)


class BuildStampTests(unittest.TestCase):
    """An overlay log that cannot name its own build makes every report a guess."""

    def test_the_overlay_logs_its_commit(self):
        src = OVERLAY.read_text(encoding='utf-8', errors='replace')
        self.assertIn('Log("[ov] build=%s packet_v=%d", OVL_BUILD_ID, VERSION)', src)
        self.assertIn('#define OVL_BUILD_ID "dev"', src, 'a local build still logs')

    def test_cmake_derives_it_from_git(self):
        cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        self.assertIn('git rev-parse --short=8 HEAD', cmake)
        self.assertIn('OVL_BUILD_ID="${OVL_BUILD_ID}"', cmake)


class TibetanCoverageTests(unittest.TestCase):
    """༺ ༻ - U+0F3A / U+0F3B - reported by the missing-glyph logger in the user's
    log, not guessed at. Names wrap themselves in them."""

    def setUp(self):
        self.src = OVERLAY.read_text(encoding='utf-8', errors='replace')

    def test_the_tibetan_block_is_covered(self):
        body = self.src[self.src.index('inline ImVector<ImWchar>& Symbols()'):]
        body = body[:body.index('\n}')]
        self.assertIn('0x0F00, 0x0FFF', body)

    def test_a_font_that_carries_tibetan_is_merged(self):
        body = self.src[self.src.index('inline void MergeNameFallbacks'):]
        body = body[:body.index('\n}')]
        self.assertIn('himalaya.ttf', body,
                      'no other stock Windows font has Tibetan')

    def test_the_fallbacks_that_loaded_are_logged(self):
        self.assertIn('loaded=%s', self.src)
        self.assertIn('Loaded() += file;', self.src,
                      'name the fonts, or "is it installed" is unanswerable')
