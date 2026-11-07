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


def load_function(name):
    tree = ast.parse(SOURCE.read_text(encoding='utf-8-sig'))
    node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == name)
    namespace = {'sys': sys, '_sys_log': lambda *a, **k: None, '_import_order_moved': False}
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), 'exec'), namespace)
    return namespace[name]


class BrokenFinder:
    """Stands in for MImporter.finder: raises for anything not in the patch."""

    def find_spec(self, name, path=None, target=None):
        raise TypeError("unsupported operand type(s) for +: 'NoneType' and 'str'")


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

        self.assertIs(sys.meta_path[-1], finder)
        self.assertEqual(len(sys.meta_path), before, 'no finder may be removed')
        self.assertNotIn(finder, sys.meta_path[:-1])

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
        sys.meta_path.insert(0, BrokenFinder())
        sys.meta_path.insert(0, working)
        repair()
        self.assertIsInstance(sys.meta_path[-1], BrokenFinder)
        self.assertLess(sys.meta_path.index(working), len(sys.meta_path) - 1)

    def test_the_module_calls_the_repair_before_using_ctypes(self):
        text = SOURCE.read_text(encoding='utf-8-sig')
        repair_call = text.index('_repair_import_order()\n')
        first_ctypes_use = text.index('windll.kernel32')
        self.assertLess(repair_call, first_ctypes_use)


if __name__ == '__main__':
    unittest.main()
