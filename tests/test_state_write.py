"""The state snapshot write: one write syscall, and never a repeated import.

`rest=` in [sys:tick] was 0.23-0.35 ms - a fifth of the tick - because every
snapshot was padded out to the whole 64 KB file and flushed through a buffered
handle on the game thread. Nothing needs the padding: the overlay reads the
entity count from the header and ignores everything past it.

The first attempt at fixing that used `mmap`, imported lazily inside the write.
Where the module is not available the import is retried - and fails - on every
tick, which is a full importer search across sys.path: `rest=` went to 6.2-7.9 ms
in the user's log and halved the frame rate. So: no imports in the write path,
an unbuffered handle that stays open, and any failure remembered rather than
retried.

These run the real functions against a temporary file; no game, no hooks.
"""

import ast
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'pyexec' / 'bs_runtime.py'
FILE_SIZE = 65536
NAMES = ('_write_state_inplace', '_open_state_sink')


def load(path):
    text = SOURCE.read_text(encoding='utf-8-sig')
    nodes = [n for n in ast.parse(text).body
             if isinstance(n, ast.FunctionDef) and n.name in NAMES]
    ns = {'os': os, 'STATE_PATH': str(path), 'STATE_FILE_SIZE': FILE_SIZE,
          '_state_fh': None, '_state_open_failed': False}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec'), ns)
    return ns


class StateWriteTests(unittest.TestCase):

    def test_snapshot_lands_at_offset_zero(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'state.bin'
            ns = load(path)
            self.assertTrue(ns['_write_state_inplace'](b'first snapshot'))
            data = path.read_bytes()
            self.assertEqual(data[:14], b'first snapshot')
            self.assertEqual(len(data), FILE_SIZE, 'the overlay maps a fixed size')

    def test_the_next_snapshot_overwrites_in_place(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'state.bin'
            ns = load(path)
            ns['_write_state_inplace'](b'A' * 9000)
            ns['_write_state_inplace'](b'B' * 4000)
            data = path.read_bytes()
            self.assertEqual(data[:4000], b'B' * 4000)
            # Shorter snapshots do not need zeroing: the overlay stops at the
            # entity count in the header. The file keeps its size for the mapping.
            self.assertEqual(len(data), FILE_SIZE)

    def test_a_large_snapshot_still_fits(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'state.bin'
            ns = load(path)
            big = b'C' * 40000
            self.assertTrue(ns['_write_state_inplace'](big))
            self.assertEqual(path.read_bytes()[:40000], big)

    def test_the_handle_is_opened_once_and_unbuffered(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'state.bin'
            ns = load(path)
            ns['_write_state_inplace'](b'x')
            first = ns['_state_fh']
            ns['_write_state_inplace'](b'y')
            self.assertIs(ns['_state_fh'], first, 'no open/close per tick')
            import io
            self.assertIsInstance(first, io.FileIO, 'raw handle: a write is one syscall')

    def test_a_failed_open_is_not_retried(self):
        """The 6 ms regression: a failing attempt repeated on every tick."""
        ns = load(Path('/nonexistent-directory/state.bin'))
        self.assertFalse(ns['_write_state_inplace'](b'x'))
        self.assertTrue(ns['_state_open_failed'])
        attempts = []
        ns['os'] = type('os', (), {'path': os.path, 'fstat': os.fstat,
                                   'exists': lambda p: attempts.append(p) or False})()
        self.assertFalse(ns['_write_state_inplace'](b'x'))
        self.assertEqual(attempts, [], 'the failure is remembered')

    def test_nothing_is_imported_or_padded_in_the_write_path(self):
        text = SOURCE.read_text(encoding='utf-8-sig')
        nodes = [n for n in ast.parse(text).body
                 if isinstance(n, ast.FunctionDef) and n.name in NAMES]
        body = '\n'.join(ast.get_source_segment(text, n) for n in nodes)
        self.assertNotIn('import mmap', body)
        self.assertNotIn('flush()', body)
        self.assertNotIn("buf + b'\\x00'", body)


if __name__ == '__main__':
    unittest.main()
