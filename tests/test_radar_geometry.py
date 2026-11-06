"""Check the radar's native rotation and range without launching the game."""
import glob
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class RadarGeometryTests(unittest.TestCase):
    def test_native_projection(self):
        candidates = []
        for variable in ('ProgramFiles', 'ProgramFiles(x86)'):
            directory = os.environ.get(variable)
            if directory:
                candidates.extend(glob.glob(str(Path(directory) / 'Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat')))
        if not candidates:
            self.skipTest('MSVC required')
        header = (Path(__file__).resolve().parents[1] / 'src/radar_geometry.hpp').as_posix()
        source = '#include "' + header + '"\n' + r'''
#include <cassert>
#include <limits>
int main() {
    float x, y;
    assert(radar::Project(0,50,0,1,100,x,y) && x==0 && y==-.5f);
    assert(radar::Project(50,0,0,1,100,x,y) && x==.5f && y==0);
    assert(radar::Project(0,-50,0,1,100,x,y) && y==.5f);
    assert(radar::Project(-50,0,0,1,100,x,y) && x==-.5f);
    assert(radar::Project(50,0,4,0,100,x,y) && x==0 && y==-.5f);
    assert(radar::Project(0,-50,1,0,100,x,y) && x==.5f && y==0);
    assert(radar::Project(60,80,0,1,100,x,y));
    assert(radar::Project(0,0,0,1,100,x,y) && x==0 && y==0);
    assert(!radar::Project(60,81,0,1,100,x,y));
    assert(!radar::Project(0,0,0,0,100,x,y));
    assert(!radar::Project(0,0,0,1,0,x,y));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    assert(!radar::Project(nan,0,0,1,100,x,y));
    assert(!radar::Project(0,inf,0,1,100,x,y));
    assert(!radar::Project(0,0,nan,1,100,x,y));
    assert(!radar::Project(0,0,0,1,inf,x,y));
}
'''
        with tempfile.TemporaryDirectory(prefix='ciye-radar-test-') as temp:
            folder = Path(temp)
            (folder / 'test.cpp').write_text(source, encoding='utf-8')
            batch = folder / 'test.cmd'
            batch.write_text('@call "' + sorted(candidates)[-1] + '" >nul\n'
                             '@cl /nologo /std:c++20 /EHsc test.cpp /Fe:test.exe\n'
                             '@if errorlevel 1 exit /b 1\n@test.exe\n', encoding='ascii')
            result = subprocess.run(['cmd','/d','/c',str(batch)], cwd=folder, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
