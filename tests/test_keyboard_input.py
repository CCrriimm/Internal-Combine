"""Exercise the actual overlay WndProc on a disposable hidden window."""
from pathlib import Path
import subprocess
import unittest


class KeyboardInputTests(unittest.TestCase):
    def test_game_keyboard_survives_menu_transitions(self):
        project = Path(__file__).resolve().parents[1]
        executable = project / 'build/Release/bs_menu_preview.exe'
        if not executable.is_file():
            self.skipTest('Build bs_menu_preview with BS_MENU_PREVIEW=ON first')
        result = subprocess.run([str(executable), '--check-keyboard'], cwd=project,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Keyboard routing checks passed', result.stdout)
