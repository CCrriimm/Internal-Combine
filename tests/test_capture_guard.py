"""Compile and run the OBS capture guard against stubbed Win32 entry points.

The guard is header-only and takes an explicit millisecond clock so the whole
apply/verify/re-apply lifecycle can be driven deterministically without a real
window, a D3D device or a game process.
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

// --- stub Win32 surface, defined before the header is included --------------
using HWND = void*;
using BOOL = int;
using DWORD = unsigned long;
static const DWORD WDA_NONE_S = 0x00000000ul;
static const DWORD WDA_EXCL_S = 0x00000011ul;
static const DWORD ERROR_SUCCESS_S = 0ul;
static const DWORD ERROR_GEN_FAILURE_S = 31ul;
#define ERROR_SUCCESS ERROR_SUCCESS_S
#define ERROR_GEN_FAILURE ERROR_GEN_FAILURE_S
#define FALSE 0
#define TRUE 1

static void* g_window = (void*)0x1000;
static void* g_valid_window = (void*)0x1000;
static DWORD g_affinity = WDA_NONE_S;
static BOOL g_set_ok = 1;
static DWORD g_last_error = 0;
static int g_set_calls = 0;
static int g_get_calls = 0;

static void SetLastError(DWORD value) { g_last_error = value; }
static DWORD GetLastError() { return g_last_error; }
static BOOL IsWindow(HWND window) { return window != nullptr && window == g_valid_window; }
static BOOL SetWindowDisplayAffinity(HWND, DWORD affinity) {
    ++g_set_calls;
    if (!g_set_ok) { g_last_error = 5; return 0; } // ERROR_ACCESS_DENIED
    g_affinity = affinity;
    return 1;
}
static BOOL GetWindowDisplayAffinity(HWND, DWORD* out) {
    ++g_get_calls;
    if (!out) return 0;
    *out = g_affinity;
    return 1;
}
// ---------------------------------------------------------------------------

#include "capture_guard.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    using namespace comet_capture;
    Blackout s;
    const HWND win = (void*)0x1000;

    // 1. Off by default: nothing is called.
    Apply(win, false, s, 0);
    CHECK(s.status == Status::Off);
    CHECK(g_set_calls == 0);

    // 2. Enabling applies WDA_EXCLUDEFROMCAPTURE exactly once.
    Apply(win, true, s, 1000);
    CHECK(s.status == Status::Active);
    CHECK(s.active);
    CHECK(g_affinity == WDA_EXCL_S);
    CHECK(g_set_calls == 1);

    // 3. Frames inside the verify window do no work at all.
    Apply(win, true, s, 1100);
    CHECK(g_set_calls == 1 && g_get_calls == 0);

    // 4. Verify cadence: affinity still in place -> no re-apply, no counter bump.
    Apply(win, true, s, 1600);
    CHECK(g_get_calls == 1);
    CHECK(g_set_calls == 1);
    CHECK(s.reapplies == 0);

    // 5. The game clears the affinity -> the guard notices and re-asserts it.
    g_affinity = WDA_NONE_S;
    Apply(win, true, s, 2100);
    CHECK(g_set_calls == 2);
    CHECK(s.reapplies == 1);
    CHECK(s.status == Status::Active);
    CHECK(g_affinity == WDA_EXCL_S);

    // 6. API failure is reported and backed off, not hammered.
    g_set_ok = 0; g_last_error = 5;
    g_affinity = WDA_NONE_S;
    Apply(win, true, s, 2600);      // verify sees it cleared, re-apply fails
    CHECK(s.status == Status::Denied);
    CHECK(s.error == 5);
    const int after_failure = g_set_calls;
    Apply(win, true, s, 2700);      // inside the 2 s backoff
    Apply(win, true, s, 3000);
    CHECK(g_set_calls == after_failure);
    Apply(win, true, s, 4700);      // past the backoff -> retried once
    CHECK(g_set_calls == after_failure + 1);

    // 7. Recovery clears the error and reports active again.
    g_set_ok = 1;
    Apply(win, true, s, 7000);
    CHECK(s.status == Status::Active);
    CHECK(s.error == 0);
    CHECK(g_affinity == WDA_EXCL_S);

    // 8. A replaced window is protected again instead of trusting stale state.
    g_window = (void*)0x2000; g_valid_window = (void*)0x2000;
    g_affinity = WDA_NONE_S;
    Apply((void*)0x2000, true, s, 7100);
    CHECK(s.status == Status::Active);
    CHECK(g_affinity == WDA_EXCL_S);

    // 9. Lost window is reported, and the request survives for the next one.
    g_valid_window = nullptr;
    Apply((void*)0x2000, true, s, 8000);
    CHECK(s.status == Status::WindowLost);
    CHECK(s.requested); // still asking, so a recreated window is protected
    g_valid_window = (void*)0x3000;
    g_affinity = WDA_NONE_S;
    Apply((void*)0x3000, true, s, 8100);
    CHECK(s.status == Status::Active);
    CHECK(g_affinity == WDA_EXCL_S);

    // 10. Disabling restores WDA_NONE and settles on Off.
    Apply((void*)0x3000, false, s, 8200);
    CHECK(s.status == Status::Off);
    CHECK(g_affinity == WDA_NONE_S);
    const int after_restore = g_set_calls;
    Apply((void*)0x3000, false, s, 8300); // idempotent, no API churn
    CHECK(g_set_calls == after_restore);

    // 11. Panic restore is unconditional and ignores the throttle.
    Restore((void*)0x3000, s);
    CHECK(s.status == Status::Off);
    CHECK(g_affinity == WDA_NONE_S);

    if (failures) { std::printf("%d capture guard checks failed\n", failures); return 1; }
    std::printf("capture guard checks passed\n");
    return 0;
}
'''


class CaptureGuardTests(unittest.TestCase):
    def test_capture_guard_lifecycle(self):
        header = (Path(__file__).resolve().parents[1] / 'src/capture_guard.hpp').as_posix()
        source = SOURCE.replace('#include "capture_guard.hpp"',
                                '#include "' + header + '"')
        with tempfile.TemporaryDirectory(prefix='comet-capture-test-') as temp:
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
                command = [compiler, '-std=c++20', '-O1', 'test.cpp', '-o', 'test']
                build = subprocess.run(command, cwd=folder, capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                command = [str(folder / 'test')]
            result = subprocess.run(command, cwd=folder, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('capture guard checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
