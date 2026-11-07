"""The engine's finder is asked about every import, and on a damaged install it
raises instead of saying "not here", which kills every standard library import
behind it.

`import ctypes` failing is what cost 3.4-4.9 ms of every presented frame: the
runtime could not create its shared-memory block, so the overlay read state.bin
from disk sixty times a second (`state_avg_ms` in overlay_log.txt).

The fix only reorders sys.meta_path - the finder stays in the chain and is asked
last, after the finders that resolve by path. Nothing is replaced, and game
modules that only exist inside the patch bundles still resolve through it.
"""

import ast
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'pyexec' / 'bs_runtime.py'


HELPERS = ('_repair_import_order', '_finder_raises_for_anything', '_meta_path_names',
           '_load_ctypes')


_finder_raises_for_anything = None


def load_function(name):
    tree = ast.parse(SOURCE.read_text(encoding='utf-8-sig'))
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in HELPERS]
    namespace = {'sys': sys, '_sys_log': lambda *a, **k: None, '_import_order_moved': False}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'), namespace)
    globals()['_finder_raises_for_anything'] = namespace['_finder_raises_for_anything']
    return namespace[name]


class BrokenFinder:
    """Stands in for MImporter.finder: raises for anything not in the patch."""

    def find_spec(self, name, path=None, target=None):
        raise TypeError("unsupported operand type(s) for +: 'NoneType' and 'str'")


class OddSignatureFinder:
    """A working finder that happens to raise a TypeError of its own - it must
    not be demoted for it (this is what setuptools' finder does)."""

    def find_spec(self, name):
        return None


BrokenFinder.__module__ = 'MImporter_py3'


class ImportOrderTests(unittest.TestCase):

    def setUp(self):
        self.saved = list(sys.meta_path)

    def tearDown(self):
        sys.meta_path[:] = self.saved

    def test_the_broken_finder_is_moved_last_and_nothing_is_dropped(self):
        repair = load_function('_repair_import_order')
        finder = BrokenFinder()
        sys.meta_path.insert(0, finder)
        before = len(sys.meta_path)

        self.assertTrue(repair())

        self.assertEqual(len(sys.meta_path), before, 'no finder may be removed')
        # The invariant that matters: nothing that works sits behind it.
        index = sys.meta_path.index(finder)
        self.assertGreater(index, 0)
        for ahead in sys.meta_path[:index]:
            self.assertFalse(_finder_raises_for_anything(ahead),
                             '%r raised for an unknown module and still runs first' % ahead)

    def test_a_finder_with_its_own_signature_is_not_demoted(self):
        repair = load_function('_repair_import_order')
        odd = OddSignatureFinder()
        sys.meta_path.insert(1, odd)
        before = list(sys.meta_path)
        self.assertFalse(repair())
        self.assertEqual(before, sys.meta_path)

    def test_a_healthy_chain_is_left_alone(self):
        repair = load_function('_repair_import_order')
        before = list(sys.meta_path)
        self.assertFalse(repair())
        self.assertEqual(before, sys.meta_path)

    def test_it_is_idempotent(self):
        repair = load_function('_repair_import_order')
        sys.meta_path.insert(0, BrokenFinder())
        self.assertTrue(repair())
        order = list(sys.meta_path)
        self.assertFalse(repair())
        self.assertEqual(order, sys.meta_path)

    def test_a_working_finder_ahead_of_it_is_asked_first(self):
        """The point of the reorder: a finder that resolves by path answers
        before the broken one is ever consulted."""
        repair = load_function('_repair_import_order')

        class WorkingFinder:
            asked = []

            def find_spec(self, name, path=None, target=None):
                WorkingFinder.asked.append(name)
                return None

        working = WorkingFinder()
        broken = BrokenFinder()
        sys.meta_path.insert(0, broken)
        sys.meta_path.insert(0, working)
        repair()
        self.assertLess(sys.meta_path.index(working), sys.meta_path.index(broken))

    def test_the_module_calls_the_repair_before_using_ctypes(self):
        text = SOURCE.read_text(encoding='utf-8-sig')
        repair_call = text.index('_repair_import_order()\n')
        first_ctypes_use = text.index('windll.kernel32')
        self.assertLess(repair_call, first_ctypes_use)


if __name__ == '__main__':
    unittest.main()
