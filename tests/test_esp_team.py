"""Team label numbering and per-team tint colours (src/esp_team.hpp).

Header-only and free of ImGui/Windows, so the exact per-match label assignment
and colour generation can be compiled and executed directly.
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
#include <unordered_set>

#include "esp_team.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    // ---- labels: compact, stable, per match --------------------------------
    teamtint::Labels labels;
    CHECK(labels.NumberFor(2001) == 1);
    CHECK(labels.NumberFor(2003) == 2);
    CHECK(labels.NumberFor(2001) == 1);   // stable
    CHECK(labels.NumberFor(2003) == 2);
    CHECK(labels.NumberFor(0) == 0);      // no team -> no label
    CHECK(labels.NumberFor(-5) == 0);     // invalid ids never consume a number
    CHECK(labels.size() == 2);

    // ---- colours: generated once, then held -------------------------------
    std::unordered_set<std::uint32_t> reserved;
    reserved.insert(0xFF5050FFu);          // the user's own enemy colour (red)

    const std::uint32_t a1 = labels.ColourFor(2001, reserved);
    const std::uint32_t a2 = labels.ColourFor(2001, reserved);
    const std::uint32_t b1 = labels.ColourFor(2003, reserved);
    CHECK(a1 != 0);
    CHECK(a1 == a2);                       // never flickers frame to frame
    CHECK(a1 != b1);                       // different teams differ

    // Never hand out a colour the user already uses for enemies/teammates.
    CHECK(a1 != 0xFF5050FFu);
    CHECK(b1 != 0xFF5050FFu);

    // Everything issued is fully opaque and comes from the candidate palette.
    auto is_candidate = [](std::uint32_t c) {
        for (int i = 0; i < teamtint::kCandidateCount; ++i)
            if (teamtint::kCandidates[i] == c) return true;
        return false;
    };
    CHECK(is_candidate(a1));
    CHECK(is_candidate(b1));
    CHECK((a1 & 0xFF000000u) == 0xFF000000u);

    // Invalid teams have no colour, so callers fall back to their base colour.
    CHECK(labels.ColourFor(0, reserved) == 0);
    CHECK(labels.ColourFor(-3, reserved) == 0);

    // Every team gets a distinct colour until the palette is exhausted. One
    // candidate is reserved, so only kCandidateCount-1 distinct colours exist.
    teamtint::Labels many;
    std::unordered_set<std::uint32_t> used;
    for (int i = 0; i < teamtint::kCandidateCount - 1; ++i) {
        std::uint32_t c = many.ColourFor(100 + i, reserved);
        CHECK(c != 0);
        CHECK(used.count(c) == 0);         // distinct
        CHECK(c != 0xFF5050FFu);
        used.insert(c);
    }
    // Exhausting the palette must still return a usable colour rather than 0.
    CHECK(many.ColourFor(100 + teamtint::kCandidateCount, reserved) != 0);

    // ---- lifetime ---------------------------------------------------------
    // Empty frames do not clear immediately...
    for (int i = 0; i < 119; ++i) labels.Frame(false);
    CHECK(labels.size() == 2);
    // ...and one populated frame resets the counter.
    labels.Frame(true);
    for (int i = 0; i < 60; ++i) labels.Frame(false);
    CHECK(labels.size() == 2);
    // A sustained empty stretch starts the next match from Team 1.
    for (int i = 0; i < 120; ++i) labels.Frame(false);
    CHECK(labels.size() == 0);
    CHECK(labels.NumberFor(2007) == 1);

    // Reset is immediate (match end).
    labels.Reset();
    CHECK(labels.size() == 0);
    CHECK(labels.NumberFor(2007) == 1);

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
                    msvc.extend(glob.glob(str(Path(directory) /
                                             'Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat')))
            if msvc:
                batch = folder / 'test.cmd'
                batch.write_text('@call "' + sorted(msvc)[-1] + '" >nul\n'
                                 '@cl /nologo /std:c++20 /EHsc test.cpp /Fe:test.exe\n'
                                 '@if errorlevel 1 exit /b 1\n@test.exe\n', encoding='ascii')
                result = subprocess.run(['cmd', '/d', '/c', str(batch)], cwd=folder,
                                        capture_output=True, text=True)
            else:
                compiler = shutil.which('g++') or shutil.which('clang++')
                if not compiler:
                    self.skipTest('no C++ compiler available')
                result = subprocess.run([compiler, '-std=c++20', '-O0', 'test.cpp', '-o', 'test'],
                                        cwd=folder, capture_output=True, text=True)
                if result.returncode == 0:
                    result = subprocess.run([str(folder / 'test')], cwd=folder,
                                            capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
