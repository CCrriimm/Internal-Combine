"""The state snapshot write is a memcpy now, not a 64 KB pad-and-flush.

`rest=` in [sys:tick] was 0.23-0.35 ms - a fifth of the tick - because every
snapshot was padded out to the whole 64 KB file and flushed through a buffered
handle on the game thread. Nothing needs the padding: the overlay reads the
entity count from the header and ignores everything past it. The runtime maps the
same file the overlay maps, so a tick costs a copy of the ~7 KB actually packed.

These run the real function against a temporary file; no game, no hooks.
"""

import ast
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'pyexec' / 'bs_runtime.py'
FILE_SIZE = 65536


def load(path):
    node = next(n for n in ast.parse(SOURCE.read_text(encoding='utf-8-sig')).body
                if isinstance(n, ast.FunctionDef) and n.name == '_write_state_inplace')
    ns = {'os': os, 'STATE_PATH': str(path), 'STATE_FILE_SIZE': FILE_SIZE,
          '_state_fh': None, '_state_map': None}
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), 'exec'), ns)
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
            self.assertIsNotNone(ns['_state_map'], 'the runtime should map it, not write it')

    def test_the_next_snapshot_overwrites_in_place(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'state.bin'
            ns = load(path)
            ns['_write_state_inplace'](b'A' * 9000)
            ns['_write_state_inplace'](b'B' * 4000)
            data = path.read_bytes()
            self.assertEqual(data[:4000], b'B' * 4000)
            # Shorter snapshots do not need zeroing: the overlay stops at the
            # entity count in the header.
            self.assertEqual(len(data), FILE_SIZE)

    def test_a_large_snapshot_still_fits(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'state.bin'
            ns = load(path)
            big = b'C' * 40000
            self.assertTrue(ns['_write_state_inplace'](big))
            self.assertEqual(path.read_bytes()[:40000], big)

    def test_the_padding_and_flush_are_gone_from_the_common_path(self):
        text = SOURCE.read_text(encoding='utf-8-sig')
        node = next(n for n in ast.parse(text).body
                    if isinstance(n, ast.FunctionDef) and n.name == '_write_state_inplace')
        body = ast.get_source_segment(text, node)
        self.assertIn('_state_map[:len(buf)] = buf', body)
        # The padded handle write may only survive as the fallback.
        self.assertLess(body.index('_state_map[:len(buf)] = buf'),
                        body.index('buf + b\'\\x00\''))


if __name__ == '__main__':
    unittest.main()
