"""The state file must be readable by the overlay without three syscalls a frame,
and writing it must never block the game thread.

From the user's logs:
  state_avg_ms = 3.3-5.8   (the overlay opens/reads/closes state.bin every frame)
  [sys:perf] total=5.981 state=5.9424 write=5.9421   (one tick spent 5.9 s in
                                                      that same write, while the
                                                      game thread waited)

The runtime now writes the snapshot into the same file at a fixed size (no rename),
and the overlay maps that file once and reads frames out of the view.
"""

import ast
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'pyexec' / 'bs_runtime.py'
OVERLAY = ROOT / 'src' / 'overlay.cpp'


class RuntimeSideTests(unittest.TestCase):

    def setUp(self):
        self.text = RUNTIME.read_text(encoding='utf-8-sig')

    def test_the_state_file_has_a_fixed_size(self):
        self.assertIn('STATE_FILE_SIZE = 65536', self.text)
        body = self.text[self.text.index('def _write_state_inplace'):]
        body = body[:body.index('# Disk fallback must not block')]
        self.assertIn('STATE_FILE_SIZE', body, 'the file must stay big enough to map')
        self.assertNotIn('replace(', body, 'a rename would invalidate the overlay mapping')

    def test_in_place_write_is_used_when_shared_memory_is_missing(self):
        body = self.text[self.text.index('def _write(path, data):'):]
        body = body[:body.index('\ndef _read(')]
        self.assertIn('_write_state_inplace(data)', body)
        self.assertLess(body.index('_write_state_inplace(data)'),
                        body.index('_state_writer_thread = threading.Thread'),
                        'the file rename path must not be reached first')

    def test_the_game_thread_never_waits_for_the_writer(self):
        body = self.text[self.text.index('def _write(path, data):'):]
        body = body[:body.index('\ndef _read(')]
        self.assertIn('_state_writer_lock.acquire(False)', body,
                      'a contended write took 5.9 s once, with the game thread waiting')


class OverlaySideTests(unittest.TestCase):

    def setUp(self):
        self.text = OVERLAY.read_text(encoding='utf-8')

    def test_the_state_file_is_mapped_once(self):
        self.assertIn('CreateFileMappingW(file, nullptr, PAGE_READONLY', self.text)
        self.assertIn('MapViewOfFile(mapping, FILE_MAP_READ', self.text)

    def test_a_half_written_snapshot_is_rejected(self):
        body = self.text[self.text.index('static bool ReadMappedState()'):]
        body = body[:body.index('static bool StateFileChanged()')]
        self.assertIn('seq_before', body)
        self.assertIn('head->seq != seq_before', body,
                      'the sequence is checked after the copy')
        self.assertIn('return false', body)

    def test_the_mapping_is_tried_before_the_file_read(self):
        start = self.text.index('static bool LoadState()')
        body = self.text[start:start + 4200]
        self.assertIn('ReadMappedState()', body)
        self.assertLess(body.index('ReadMappedState()'), body.index('StateFileChanged()'),
                        'the file read is the expensive fallback, not the first choice')

    def test_a_new_frame_does_not_trigger_a_file_read(self):
        start = self.text.index('static bool LoadState()')
        body = self.text[start:start + 4200]
        self.assertIn('if (g_state_view) return true;', body,
                      'mapped but no new snapshot: keep the current state, do not read the file')


if __name__ == '__main__':
    unittest.main()
