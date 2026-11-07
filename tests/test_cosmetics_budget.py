"""Cosmetics refresh must be incremental without changing visual frame cadence."""
import ast
from pathlib import Path
import types
import unittest


class CosmeticsBudgetTests(unittest.TestCase):
    def test_incremental_scan_preserves_entries_and_predicates(self):
        source = Path(__file__).resolve().parents[1] / 'pyexec/bs_runtime.py'
        names = {'_iter_data_entries', '_eval_cosmetic_entry', '_sync_cosmetics_budget',
                 '_iter_cosmetic_predicates', '_refresh_runtime_cosmetics'}
        nodes = [n for n in ast.parse(source.read_text(encoding='utf-8-sig')).body
                 if isinstance(n, ast.FunctionDef) and n.name in names]
        clock = [1000.0]
        def now():
            clock[0] += 0.0001
            return clock[0]
        def is_unlocked():
            return False
        rows = [{'owned': 0}, 4, {'locked': 1}]
        modules = {'other': object(), 'gclient.data.example': types.SimpleNamespace(data=rows),
                   'gclient.test': types.SimpleNamespace(is_unlocked=is_unlocked)}
        logs = []
        ns = {'sys': types.SimpleNamespace(modules=modules), 'time': types.SimpleNamespace(time=now),
              'traceback': __import__('traceback'), '_sys_log': logs.append,
              'UNLOCK_TRUE': {'owned'}, 'UNLOCK_FALSE': {'locked'}, '_unlock_iter': None,
              '_unlock_pred_iter': None, '_unlock_done': False, '_unlock_step_t': 0.0,
              '_last_unlock': 0.0, '_unlock_scan_t': 0.0, '_unlock_changed': 0,
              '_wrapped_preds': set()}
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), ns)
        ns['_refresh_runtime_cosmetics']()
        self.assertFalse(ns['_unlock_done'])
        saved_iter = ns['_unlock_iter']
        ns['_refresh_runtime_cosmetics']()
        self.assertIs(ns['_unlock_iter'], saved_iter)  # no second step inside 16 ms
        for _ in range(100):
            clock[0] += .017
            ns['_refresh_runtime_cosmetics']()
            if ns['_unlock_done']:
                break
        self.assertTrue(ns['_unlock_done'])
        self.assertEqual(rows[0]['owned'], 1)
        self.assertEqual(rows[2]['locked'], 0)
        self.assertTrue(modules['gclient.test'].is_unlocked())
        self.assertEqual(ns['_unlock_changed'], 2)
        self.assertEqual(len(logs), 1)
        ns['_refresh_runtime_cosmetics']()
        self.assertEqual(len(logs), 1)


if __name__ == '__main__':
    unittest.main()
