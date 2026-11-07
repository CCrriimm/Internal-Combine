"""Max-armor tier colours (src/esp_armor_tier.hpp).

Ported boundary behaviour from the reference Comet build: <=50 grey, 75 blue,
100 purple, >=125 gold, with linear blends between adjacent tiers and a stable
result for NaN/unknown input.
"""
import glob
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = r'''
#include <cmath>
#include <cstdio>
#include <limits>

#include "esp_armor_tier.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

// Named close_enough: `near` is an empty macro in the Windows SDK windef.h.
static bool close_enough(float a, float b) { return std::fabs(a - b) < 0.002f; }

int main() {
    using namespace armortier;

    // Exact tier anchors.
    CHECK(close_enough(Tier(50).r, Grey50.r) && close_enough(Tier(50).g, Grey50.g));
    CHECK(close_enough(Tier(75).b, Blue75.b));
    CHECK(close_enough(Tier(100).g, Purple100.g));
    CHECK(close_enough(Tier(125).r, Gold125.r));
    CHECK(close_enough(Tier(200).r, Gold125.r));   // above the top tier stays gold

    // Blends between adjacent tiers rather than snapping.
    const Rgba mid = Tier(62.5f);
    CHECK(close_enough(mid.r, (Grey50.r + Blue75.r) * 0.5f));
    CHECK(close_enough(mid.b, (Grey50.b + Blue75.b) * 0.5f));
    CHECK(close_enough(Tier(87.5f).r, (Blue75.r + Purple100.r) * 0.5f));
    CHECK(close_enough(Tier(112.5f).b, (Purple100.b + Gold125.b) * 0.5f));

    // Unknown / degenerate inputs fall back to the grey tier, never NaN.
    CHECK(close_enough(Tier(0).r, Grey50.r));
    CHECK(close_enough(Tier(-10).r, Grey50.r));
    CHECK(close_enough(Tier(std::numeric_limits<float>::quiet_NaN()).r, Grey50.r));
    CHECK(close_enough(Tier(std::numeric_limits<float>::infinity()).r, Gold125.r));

    // Caller palette: the menu's colours are what the label actually uses, and
    // the same blend rules apply to them.
    const Rgba g{0.1f, 0.1f, 0.1f, 1.0f}, b{0.2f, 0.4f, 0.6f, 1.0f};
    const Rgba p{0.4f, 0.2f, 0.8f, 1.0f}, o{0.9f, 0.7f, 0.1f, 1.0f};
    CHECK(close_enough(Tier(10, g, b, p, o).r, g.r));
    CHECK(close_enough(Tier(75, g, b, p, o).r, b.r));
    CHECK(close_enough(Tier(200, g, b, p, o).r, o.r));
    CHECK(close_enough(Tier(62.5f, g, b, p, o).r, (g.r + b.r) * 0.5f));
    // Round-trip through the config's packed IM_COL32 so Unpack feeds Pack back.
    const uint32_t cfg_colour = 0xF5FF8C33u; // A=245 B=255 G=140 R=51
    const Rgba back = Unpack(cfg_colour);
    CHECK(close_enough(back.r, 51.0f / 255.0f) && close_enough(back.g, 140.0f / 255.0f));
    CHECK(close_enough(back.b, 1.0f) && close_enough(back.a, 245.0f / 255.0f));
    CHECK(Pack(back) == cfg_colour);

    // Packed colour is IM_COL32 order (AABBGGRR) and fully opaque.
    const uint32_t packed = Packed(100);
    CHECK((packed >> 24) == 255u);
    CHECK((packed & 0xFFu) == 173u);         // red channel of the purple tier
    CHECK(((packed >> 16) & 0xFFu) == 255u); // blue channel of the purple tier
    CHECK(Packed(50) != Packed(125));

    // The overlay derives cfg defaults from these anchors; if a tier colour
    // changes, saved/migrated configs must not silently change meaning.
    const uint32_t cfg_grey = (Packed(50) & 0x00FFFFFFu) | (245u << 24);
    const uint32_t cfg_gold = (Packed(125) & 0x00FFFFFFu) | (245u << 24);
    CHECK(cfg_grey == 4120810636u);
    CHECK(cfg_gold == 4112499455u);

    if (failures) { std::printf("%d armor tier checks failed\n", failures); return 1; }
    std::printf("armor tier checks passed\n");
    return 0;
}
'''


class ArmorTierTests(unittest.TestCase):
    def test_armor_tiers(self):
        header = (Path(__file__).resolve().parents[1] / 'src/esp_armor_tier.hpp').as_posix()
        source = SOURCE.replace('#include "esp_armor_tier.hpp"', '#include "' + header + '"')
        with tempfile.TemporaryDirectory(prefix='comet-armor-test-') as temp:
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
                self.assertIn('armor tier checks passed', build.stdout)
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
            self.assertIn('armor tier checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
