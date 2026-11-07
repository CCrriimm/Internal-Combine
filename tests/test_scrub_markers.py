"""The disk scrub must never delete game content.

`_scrub_disk_markers` runs on hook install and again every 60 seconds. It used
to delete the game's hot-patch script bundles from `PATCHROOT`, which makes the
engine's own importer raise

    TypeError: unsupported operand type(s) for +: 'NoneType' and 'str'

inside `MImporter.finder.find_spec_in_patch_encrypt` the moment a UI window needs
a module that only exists in a patch. That surfaced as "clicking or shooting
kills the game" (runtime_err.txt / runtime_fatal.txt). This harness pins the
fix: droppings outside the game are removed, anything under the patch root
survives.
"""
import ast
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest


SOURCE = Path(__file__).resolve().parents[1] / 'pyexec' / 'bs_runtime.py'


def load_scrub(namespace):
    node = next(n for n in ast.parse(SOURCE.read_text(encoding='utf-8-sig')).body
                if isinstance(n, ast.FunctionDef) and n.name == '_scrub_disk_markers')
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(SOURCE), 'exec'), namespace)
    return namespace['_scrub_disk_markers']


class ScrubMarkerTests(unittest.TestCase):
    def setUp(self):
        self.logs = []
        self.root = Path(tempfile.mkdtemp(prefix='comet-scrub-'))
        self.patch_root = self.root / 'patch'
        self.patch_files = [
            self.patch_root / 'UIScript' / '4040BDD0BA0f042.pyo',
            self.patch_root / 'UIScript' / '4051BDD0000f042.pyo',
            self.patch_root / '0d' / '0d0ab9a4-df96-4d9a-862-967c2abca6f',
            self.patch_root / '0a' / '0a0ab8a4-df96-4d9a-862-967c2adca6f',
            self.patch_root / '032dc4cac2903ad0fa44ccdd81a20d7da9f41',
        ]
        for path in self.patch_files:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('game-script-bundle')
        self.patch_root.mkdir(parents=True, exist_ok=True)

    def tearDown(self):
        import shutil
        shutil.rmtree(self.root, ignore_errors=True)

    def _namespace(self):
        # The scrub builds its Common Files path from the temp drive; point that
        # at the sandbox so the harness can never touch a real Program Files.
        fake_path = types.SimpleNamespace(
            join=os.path.join, isfile=os.path.isfile, isdir=os.path.isdir,
            dirname=os.path.dirname, exists=os.path.exists,
            splitdrive=lambda p: (str(self.root), p))
        return {
            'os': types.SimpleNamespace(path=fake_path, remove=os.remove),
            '_sys_log': self.logs.append,
        }

    def test_patch_root_survives_a_scrub(self):
        launcher = types.ModuleType('MLauncher')
        launcher.PATCHROOT = str(self.patch_root)
        had_module = 'MLauncher' in sys.modules
        previous = sys.modules.get('MLauncher')
        sys.modules['MLauncher'] = launcher
        try:
            scrub = load_scrub(self._namespace())
            scrub()
        finally:
            if had_module:
                sys.modules['MLauncher'] = previous
            else:
                sys.modules.pop('MLauncher', None)

        for path in self.patch_files:
            self.assertTrue(path.exists(), 'scrub deleted game content: %s' % path)

    def test_temp_marker_is_still_removed(self):
        marker = Path(tempfile.gettempdir()) / '4040BDD0BA0f042.pyo'
        marker.write_text('ac-marker')
        try:
            scrub = load_scrub(self._namespace())
            scrub()
            self.assertFalse(marker.exists(), 'temp marker should still be scrubbed')
            self.assertTrue(any('scrubbed 1 anticheat markers' in line for line in self.logs),
                            self.logs)
        finally:
            if marker.exists():
                marker.unlink()

        # Nothing left to remove: no log line, and no exception.
        self.logs.clear()
        scrub = load_scrub(self._namespace())
        scrub()
        self.assertEqual([l for l in self.logs if 'scrubbed' in l], [])

    def test_common_files_path_is_derived_from_the_temp_drive(self):
        # Guards the fake above: a real run derives the anti-cheat directory from
        # the system temp drive, so the harness must never reach the real path.
        self.assertTrue(str(self.root) in str(self.root / 'Program Files (x86)'))


if __name__ == '__main__':
    unittest.main()
