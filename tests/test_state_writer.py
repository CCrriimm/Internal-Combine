"""State file fallback is asynchronous and keeps only the newest snapshot."""
import ast
from pathlib import Path
import threading
import time
import types
import unittest


class StateWriterTests(unittest.TestCase):
    def test_slow_disk_does_not_block_ticks_and_coalesces(self):
        source = Path(__file__).resolve().parents[1] / 'pyexec/bs_runtime.py'
        tree = ast.parse(source.read_text(encoding='utf-8-sig'))
        nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef)
                 and n.name in ('_write', '_state_writer_loop')]
        entered, release, written = threading.Event(), threading.Event(), []
        def slow_write(path, data):
            written.append(bytes(data))
            if len(written) == 1:
                entered.set()
                release.wait(2)
        ns = {'threading': threading, 'time': time, 'STATE_PATH': 'state',
              '_shm_state_p': None, '_init_ctypes_shm': lambda: None,
              '_state_file_t': 0, '_state_writer_thread': None,
              '_state_writer_event': threading.Event(), '_state_writer_lock': threading.Lock(),
              '_state_writer_pending': None, '_write_file': slow_write}
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), ns)
        try:
            self.assertTrue(ns['_write']('state', b'first'))
            self.assertTrue(entered.wait(1))
            start = time.monotonic()
            self.assertTrue(ns['_write']('state', b'old'))
            self.assertTrue(ns['_write']('state', b'new'))
            self.assertLess(time.monotonic() - start, 0.2)
            release.set()
            deadline = time.monotonic() + 2
            while len(written) < 2 and time.monotonic() < deadline:
                time.sleep(0.005)
            self.assertEqual(written, [b'first', b'new'])
        finally:
            release.set()


    def test_shared_memory_updates_each_tick_while_disk_is_blocked(self):
        import ctypes
        source = Path(__file__).resolve().parents[1] / 'pyexec/bs_runtime.py'
        tree = ast.parse(source.read_text(encoding='utf-8-sig'))
        nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef)
                 and n.name in ('_write', '_state_writer_loop')]
        entered, release = threading.Event(), threading.Event()
        def slow_write(path, data):
            entered.set()
            release.wait(2)
        buffer = ctypes.create_string_buffer(16)
        now = [100.0]
        ns = {'threading': threading, 'time': types.SimpleNamespace(time=lambda: now[0]),
              'STATE_PATH': 'state', '_shm_state_p': ctypes.addressof(buffer),
              '_init_ctypes_shm': lambda: None, '_state_file_t': 0,
              '_state_writer_thread': None, '_state_writer_event': threading.Event(),
              '_state_writer_lock': threading.Lock(), '_state_writer_pending': None,
              '_write_file': slow_write}
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), ns)
        try:
            self.assertTrue(ns['_write']('state', b'first'))
            self.assertTrue(entered.wait(1))
            now[0] += 0.005
            self.assertTrue(ns['_write']('state', b'newer'))
            self.assertEqual(buffer.raw[:5], b'newer')
        finally:
            release.set()


if __name__ == '__main__':
    unittest.main()
