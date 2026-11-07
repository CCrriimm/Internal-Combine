"""Compile and run the capture-shield policy core without a game or a GPU.

`src/capture_shield.hpp` is deliberately Win32-free: it classifies the loaded
capture modules, decides where our Present detour sits relative to a capture
hook, and owns the bounded clean-frame gate used by the GDI detours. All three
are driven here directly, including the wait timing, which uses an injected
clock instead of sleeping for real.
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
#include <cstring>

#include "capture_shield.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static bool Equals(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

int main() {
    using namespace comet_shield;

    // ---- module inventory -------------------------------------------------
    {
        Modules m;
        CHECK(m.Count() == 0);
        CHECK(!m.Any());
        CHECK(!m.HooksBackBuffer());
        char buf[160];
        CHECK(Describe(m, buf, (int)sizeof(buf)) == 0);
        CHECK(Equals(buf, ""));
        CHECK(Describe(m, nullptr, 0) == 0);

        m.obs_graphics_hook = true;
        CHECK(m.Count() == 1 && m.Any() && m.HooksBackBuffer());
        Describe(m, buf, (int)sizeof(buf));
        CHECK(Equals(buf, "OBS hook"));

        m.steam_overlay = true;
        m.other_hook = true;
        CHECK(m.Count() == 3);
        Describe(m, buf, (int)sizeof(buf));
        CHECK(Equals(buf, "OBS hook, Steam overlay, other hook"));

        // A short buffer truncates instead of overrunning.
        char tiny[10];
        Describe(m, tiny, (int)sizeof(tiny));
        CHECK(std::strlen(tiny) == sizeof(tiny) - 1);
        CHECK(std::strncmp(tiny, "OBS hook,", 9) == 0);

        Modules all{};
        all.obs_graphics_hook = all.obs_hook_dll = all.steam_overlay = true;
        all.nvidia_share = all.amd_relive = all.other_hook = true;
        CHECK(all.Count() == 6);
    }

    // ---- present chain position ------------------------------------------
    {
        int ours = 0, theirs = 0;
        CHECK(ClassifyPresentChain(nullptr, &ours) == Chain::Unknown);
        CHECK(ClassifyPresentChain(&ours, nullptr) == Chain::Unknown);
        CHECK(ClassifyPresentChain(nullptr, nullptr) == Chain::Unknown);
        // Slot still points at the implementation we patched: nobody hooks above.
        CHECK(ClassifyPresentChain(&ours, &ours) == Chain::Topmost);
        // Slot points elsewhere: another detour copies the buffer before we draw.
        CHECK(ClassifyPresentChain(&theirs, &ours) == Chain::UnderCapturer);
        CHECK(std::strlen(Describe(Chain::Topmost)) > 0);
        CHECK(std::strlen(Describe(Chain::UnderCapturer)) > 0);
        CHECK(std::strlen(Describe(Chain::Unknown)) > 0);
    }

    // ---- clean-frame gate -------------------------------------------------
    {
        CleanFrameGate gate;
        CHECK(!gate.Busy());
        CHECK(gate.Frame() == 0);

        gate.Request();
        CHECK(gate.Busy());
        gate.Request();
        gate.Release();
        CHECK(gate.Busy());          // one requester still waiting
        gate.Release();
        CHECK(!gate.Busy());
        gate.Release();              // extra release must not underflow
        CHECK(!gate.Busy());

        // No frame arrives: the wait gives up at the deadline instead of hanging.
        unsigned long long now = 0;
        int yields = 0;
        const bool timed_out = gate.Await(0, kCaptureWaitMs,
                                          [&] { return now; },
                                          [&] { ++yields; now += 7; });
        CHECK(!timed_out);
        CHECK(yields > 0);
        CHECK(now >= kCaptureWaitMs);

        // A frame presented by another thread releases the waiter.
        gate.Request();
        const unsigned long long since = gate.Frame();
        bool presented = false;
        int spins = 0;
        const bool got_frame = gate.Await(since, kCaptureWaitMs,
                                          [&] { return now; },
                                          [&] { ++spins; if (!presented) { presented = true; gate.OnPresented(); } });
        CHECK(got_frame);
        CHECK(presented);
        CHECK(gate.Frame() == since + 1);
        gate.Release();
        CHECK(!gate.Busy());

        // A frame that predates the request does not satisfy the wait.
        CleanFrameGate stale;
        stale.OnPresented();
        stale.OnPresented();
        const unsigned long long before = stale.Frame();
        unsigned long long t = 0;
        CHECK(!stale.Await(before, 20, [&] { return t; }, [&] { t += 4; }));
        CHECK(stale.Await(before - 1, 20, [&] { return t; }, [&] { t += 1; }));
    }

    if (failures) { std::printf("%d capture shield checks failed\n", failures); return 1; }
    std::printf("capture shield checks passed\n");
    return 0;
}
'''


class CaptureShieldTests(unittest.TestCase):
    def test_capture_shield_policy(self):
        header = (Path(__file__).resolve().parents[1] / 'src/capture_shield.hpp').as_posix()
        source = SOURCE.replace('#include "capture_shield.hpp"',
                                '#include "' + header + '"')
        with tempfile.TemporaryDirectory(prefix='comet-shield-test-') as temp:
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
                command = ['cmd', '/d', '/c', str(batch)]
            else:
                compiler = shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
                if not compiler:
                    self.skipTest('No C++ compiler available')
                build = subprocess.run([compiler, '-std=c++20', '-O1', 'test.cpp', '-o', 'test'],
                                       cwd=folder, capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                command = [str(folder / 'test')]
            result = subprocess.run(command, cwd=folder, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('capture shield checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
