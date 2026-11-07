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
              '_unlock_interval': 120.0, '_unlock_dir_cache': {},
              '_entry_pending': None, '_wrapped_preds': set(), '_wrapped_logged': False}
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
        self.assertEqual([l for l in logs if l.startswith('unlock pass done')][0].split()[3:4],
                         ['changed=2'])
        self.assertTrue(any(l.startswith('[sys:sec] forced predicates:') for l in logs), logs)
        after = len(logs)
        ns['_refresh_runtime_cosmetics']()
        self.assertEqual(len(logs), after)


if __name__ == '__main__':
    unittest.main()


def _harness(rows, modules, start=1000.0):
    import traceback
    source = Path(__file__).resolve().parents[1] / 'pyexec' / 'bs_runtime.py'
    names = {'_iter_data_entries', '_eval_cosmetic_entry', '_sync_cosmetics_budget',
             '_iter_cosmetic_predicates', '_refresh_runtime_cosmetics'}
    nodes = [n for n in ast.parse(source.read_text(encoding='utf-8-sig')).body
             if isinstance(n, ast.FunctionDef) and n.name in names]
    clock = [start]
    def now():
        clock[0] += 0.0001
        return clock[0]
    logs = []
    ns = {'sys': types.SimpleNamespace(modules=modules), 'time': types.SimpleNamespace(time=now),
          'traceback': traceback, '_sys_log': logs.append,
          'UNLOCK_TRUE': {'owned'}, 'UNLOCK_FALSE': {'locked'},
          '_unlock_iter': None, '_unlock_pred_iter': None, '_unlock_done': True,
          '_unlock_step_t': 0.0, '_last_unlock': 0.0, '_unlock_scan_t': 0.0,
          '_unlock_changed': 0, '_unlock_interval': 120.0, '_unlock_dir_cache': {},
          '_entry_pending': None, '_wrapped_preds': set(), '_wrapped_logged': False}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), ns)
    ns['clock'] = clock
    ns['logs'] = logs
    return ns


class LobbyOnlyTests(unittest.TestCase):
    def test_pass_does_nothing_during_a_match(self):
        rows = [{'owned': 0}]
        ns = _harness(rows, {'gclient.data.example': types.SimpleNamespace(data=rows)})
        for _ in range(50):
            ns['_refresh_runtime_cosmetics'](False)
            ns['clock'][0] += 0.5
        self.assertEqual(rows[0]['owned'], 0, 'the pass must not run mid-match')
        self.assertIsNone(ns['_unlock_iter'])
        self.assertEqual(ns['logs'], [])


class LargeRowTests(unittest.TestCase):
    def test_one_huge_row_is_spread_over_ticks(self):
        """A single data row with 200k keys used to be evaluated in one go: that
        is the 25 ms `unlock=` spike measured inside a 17 ms frame."""
        row = {'owned': 0}
        for i in range(20000):
            row['k%d' % i] = 0
        ns = _harness([row], {'gclient.data.example': types.SimpleNamespace(data=[row])})
        ns['_eval_cosmetic_entry'](row, 0.0005)
        self.assertIsNotNone(ns['_entry_pending'], 'the row must be left partially done')
        self.assertGreater(len(ns['_entry_pending'][1]), 1000)
        # Resuming finishes it and flips the flag exactly once.
        for _ in range(20000):
            ns['_sync_cosmetics_budget'](0.0005)
            if ns['_entry_pending'] is None:
                break
        self.assertIsNone(ns['_entry_pending'], 'row would not finish')
        self.assertEqual(row['owned'], 1)

    def test_a_pass_that_changes_nothing_backs_off(self):
        rows = [{'k%d' % i: 0 for i in range(50)}]
        ns = _harness(rows, {'gclient.data.example': types.SimpleNamespace(data=rows)})
        for _ in range(500):
            ns['_refresh_runtime_cosmetics']()
            ns['clock'][0] += 0.017
            if any('unlock pass done' in line for line in ns['logs']):
                break
        self.assertTrue(any('unlock pass done changed=0' in line for line in ns['logs']),
                        ns['logs'])
        self.assertEqual(ns['_unlock_interval'], 240.0)
