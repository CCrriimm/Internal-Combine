"""A failed UI import must not kill the game.

The logs the user sent (`runtime_fatal-2.txt`) show the same chain every time a
button in the personal-space / combat-record UI is pressed:

    MImporter.finder.find_spec_in_patch_encrypt -> find_spec_in_encrypt
      -> get_relpath_with_path: TypeError: NoneType + str
    Exception: ---- exception when execute (…moba_faction_calculation) module
    ui_node.py eventDispatcher -> onTouchEnded -> OnClickForDetails -> …

which the engine reports as "bloodstrike.exe has stopped working". `_guard_ui_events`
wraps the dispatcher so the click becomes a logged no-op instead.
"""
import ast
from pathlib import Path
import sys
import types
import traceback
import unittest


SOURCE = Path(__file__).resolve().parents[1] / 'pyexec' / 'bs_runtime.py'


def build(namespace):
    names = {'_guard_ui_events', '_log_ui_failure'}
    nodes = [n for n in ast.parse(SOURCE.read_text(encoding='utf-8-sig')).body
             if isinstance(n, ast.FunctionDef) and n.name in names]
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'), namespace)
    return namespace['_guard_ui_events']


class UiGuardTests(unittest.TestCase):
    def setUp(self):
        self.logs = []
        self.module = types.ModuleType('gclient.framework.ui.widgets.ui_node')

        def boom(self, *args, **kwargs):
            raise TypeError("unsupported operand type(s) for +: 'NoneType' and 'str'")

        def click(self, *args, **kwargs):
            return 'clicked'

        self.Node = type('UINode', (), {'eventDispatcher': boom, 'onTouchEnded': click})
        self.module.UINode = self.Node
        self.module.helper = 3
        for name in ('gclient', 'gclient.framework', 'gclient.framework.ui',
                     'gclient.framework.ui.widgets'):
            sys.modules.setdefault(name, types.ModuleType(name))
        self.saved = sys.modules.get('gclient.framework.ui.widgets.ui_node')
        sys.modules['gclient.framework.ui.widgets.ui_node'] = self.module

    def tearDown(self):
        if self.saved is not None:
            sys.modules['gclient.framework.ui.widgets.ui_node'] = self.saved
        else:
            sys.modules.pop('gclient.framework.ui.widgets.ui_node', None)

    def _install(self):
        return build({'traceback': traceback, 'sys': sys, '_sys_log': self.logs.append,
                      '_no_hooks_requested': lambda: False,
                      '_ui_guard_logged': set()})

    def test_a_failing_click_is_swallowed_and_logged_once(self):
        install = self._install()
        self.assertEqual(install(), 2)             # eventDispatcher + onTouchEnded
        node = self.Node()
        self.assertIsNone(node.eventDispatcher())  # no exception escapes
        self.assertIsNone(node.eventDispatcher())  # repeats stay quiet
        self.assertEqual(node.onTouchEnded(), 'clicked')   # working path unaffected
        swallowed = [l for l in self.logs if 'UI event swallowed' in l]
        self.assertEqual(len(swallowed), 1, self.logs)

    def test_missing_patch_module_is_reported_in_plain_words(self):
        install = self._install()
        install()
        text = ('Traceback (most recent call last):\n  File "MImporter_py3.py", '
                'line 305, in MImporter.finder.get_relpath_with_path\n'
                "TypeError: unsupported operand type(s) for +: 'NoneType' and 'str'")
        self.Node().eventDispatcher.__self__ if False else None
        # drive the logger directly with the real traceback shape
        import types as t
        ns = {'_sys_log': self.logs.append, '_ui_guard_logged': set()}
        node = next(n for n in ast.parse(SOURCE.read_text(encoding='utf-8-sig')).body
                    if isinstance(n, ast.FunctionDef) and n.name == '_log_ui_failure')
        exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), 'exec'), ns)
        ns['_log_ui_failure'](text)
        joined = ' '.join(self.logs)
        self.assertIn('patch module could not be imported', joined)
        self.assertIn('verify / re-download', joined)

    def test_the_runtime_deletes_nothing_any_more(self):
        source = SOURCE.read_text(encoding='utf-8-sig')
        self.assertNotIn('_scrub_disk_markers', source)
        # The only os.remove left is the stale error-log reset.
        self.assertEqual(source.count('os.remove('), 1, 'no file deletion in the runtime')
        self.assertEqual(source.count('shutil.rmtree('), 0)


if __name__ == '__main__':
    unittest.main()


class ImporterRescueTests(unittest.TestCase):
    """A path finder that raises TypeError aborts the whole import.

    `MImporter.finder.find_spec_in_patch_encrypt` raises
    `TypeError: NoneType + str` for anything that is not in the patch index, so
    every module that was not already loaded failed to import - `ctypes.wintypes`
    included, which is how the runtime silently lost its shared-memory IPC.
    """

    def test_typeerror_becomes_not_found(self):
        source = SOURCE.read_text(encoding='utf-8-sig')
        node = next(n for n in ast.parse(source).body
                    if isinstance(n, ast.FunctionDef) and n.name == '_rescue_importer')

        class Finder:
            def find_spec_in_patch_encrypt(self, name, path=None, target=None):
                raise TypeError("unsupported operand type(s) for +: 'NoneType' and 'str'")

            def find_spec_in_encrypt(self, name, path=None, target=None):
                return 'FOUND'

        Finder.__module__ = 'MImporter_py3'
        finder = Finder()
        saved = list(sys.meta_path)
        logs = []
        ns = {'sys': sys, '_sys_log': logs.append, '_importer_rescued': False}
        exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), 'exec'), ns)
        sys.meta_path.insert(0, finder)
        try:
            self.assertTrue(ns['_rescue_importer']())
            self.assertIsNone(finder.find_spec_in_patch_encrypt('anything'))
            self.assertEqual(finder.find_spec_in_encrypt('anything'), 'FOUND')
        finally:
            sys.meta_path[:] = saved
        self.assertTrue(any('importer rescued' in l for l in logs), logs)

    def test_ctypes_is_imported_eagerly(self):
        source = SOURCE.read_text(encoding='utf-8-sig')
        head = source.split('def _root_dir')[0]
        self.assertIn('import ctypes', head,
                      'ctypes must be loaded before the engine importer can break it')
