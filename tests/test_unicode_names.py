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
