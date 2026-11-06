"""Team label numbering and team tint rules (src/esp_team.hpp).

The header is header-only and free of ImGui/Windows, so the exact per-match
label assignment and palette selection can be compiled and executed directly.
"""
import glob
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = r'''
#include <cstdint>
#include <cstdio>

#include "esp_team.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    // The palette is the user's four configurable slots, exactly as the overlay
    // passes cfg.col_team_box / cfg.col_team_line.
    const std::uint32_t palette[4] = {0xFFAA0001u, 0xFFBB0002u, 0xFFCC0003u, 0xFFDD0004u};
    // Same raw id -> same slot (a team keeps its colour across matches).
    CHECK(teamtint::ColourFor(2001, palette) == teamtint::ColourFor(2001, palette));
    CHECK(teamtint::ColourFor(2001, palette) == palette[2001 % 4]);
    // Different ids land on different slots (mod 4).
    CHECK(teamtint::ColourFor(2001, palette) != teamtint::ColourFor(2002, palette));
    // A full cycle returns to the start.
    CHECK(teamtint::ColourFor(6, palette) == teamtint::ColourFor(2, palette));
    // Unknown / negative ids fall back to slot 0 instead of indexing out of range.
    CHECK(teamtint::ColourFor(0, palette) == palette[0]);
    CHECK(teamtint::ColourFor(-5, palette) == palette[0]);
    // Every slot is reachable and the value is returned unmodified, so the menu
    // colour the user picked is the colour that is drawn.
    for (int team = -16; team < 64; ++team) {
        std::uint32_t c = teamtint::ColourFor(team, palette);
        CHECK(c == palette[0] || c == palette[1] || c == palette[2] || c == palette[3]);
    }

    // Labels are compact, stable, and per-match.
    teamtint::Labels labels;
    CHECK(labels.NumberFor(2001) == 1);
    CHECK(labels.NumberFor(2003) == 2);
    CHECK(labels.NumberFor(2001) == 1);   // stable
    CHECK(labels.NumberFor(2003) == 2);
    CHECK(labels.NumberFor(0) == 0);      // no team -> no label
    CHECK(labels.NumberFor(-5) == 0);     // invalid ids never consume a number
    CHECK(labels.size() == 2);

    // Empty frames do not clear immediately...
    for (int i = 0; i < 119; ++i) labels.Frame(false);
    CHECK(labels.NumberFor(2001) == 1);
    // ...but a sustained empty stretch starts the next match at Team 1.
    labels.Frame(false);
    CHECK(labels.size() == 0);
    CHECK(labels.NumberFor(2003) == 1);
    // A player frame resets the empty counter.
    for (int i = 0; i < 50; ++i) labels.Frame(false);
    labels.Frame(true);
    for (int i = 0; i < 50; ++i) labels.Frame(false);
    CHECK(labels.size() == 1);
    labels.Reset();
    CHECK(labels.size() == 0);

    if (failures) { std::printf("%d team tint checks failed\n", failures); return 1; }
    std::printf("team tint checks passed\n");
    return 0;
}
'''


class EspTeamTests(unittest.TestCase):
    def test_team_labels_and_tints(self):
        header = (Path(__file__).resolve().parents[1] / 'src/esp_team.hpp').as_posix()
        source = SOURCE.replace('#include "esp_team.hpp"', '#include "' + header + '"')
        with tempfile.TemporaryDirectory(prefix='comet-team-test-') as temp:
            folder = Path(temp)
            (folder / 'test.cpp').write_text(source, encoding='utf-8')
            msvc = []
            for variable in ('ProgramFiles', 'ProgramFiles(x86)'):
                directory = os.environ.get(variable)
                if directory:
                    msvc.extend(glob.glob(str(Path(directory) / 'Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat')))
            if msvc:
                batch = folder / 'test.cmd'
                batch.write_text('@call "' + sorted(msvc)[-1] + '" >nul\n'
                                 '@cl /nologo /std:c++20 /EHsc test.cpp /Fe:test.exe\n'
                                 '@if errorlevel 1 exit /b 1\n@test.exe\n', encoding='ascii')
                build = subprocess.run(['cmd', '/d', '/c', str(batch)], cwd=folder,
                                       capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                self.assertIn('team tint checks passed', build.stdout)
                return
            compiler = shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
            if not compiler:
                self.skipTest('No C++ compiler available')
            build = subprocess.run([compiler, '-std=c++20', '-O1', 'test.cpp', '-o', 'test'],
                                   cwd=folder, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(folder / 'test')], cwd=folder,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('team tint checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
