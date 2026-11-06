"""Compile the geometry used by ESP without starting the overlay or game."""
import glob
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class EspGeometryTests(unittest.TestCase):
    def test_native_geometry(self):
        candidates = []
        for variable in ('ProgramFiles', 'ProgramFiles(x86)'):
            directory = os.environ.get(variable)
            if directory:
                candidates.extend(glob.glob(str(Path(directory) / 'Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat')))
        if not candidates:
            self.skipTest('MSVC required')
        header = (Path(__file__).resolve().parents[1] / 'src/esp_geometry.hpp').as_posix()
        source = '#include "' + header + '"\n' + r'''
#include <cassert>
#include <limits>
int main() {
    assert(esp::LineOrigin(800,600,0).y==600);
    assert(esp::LineOrigin(800,600,1).y==0);
    assert(esp::LineOrigin(800,600,2).y==300);
    assert(esp::LineOrigin(800,600,3).y==600);
    const auto bottom = esp::LineTarget(250,120,440,0);
    const auto top = esp::LineTarget(250,120,440,1);
    const auto mid = esp::LineTarget(250,120,440,2);
    assert(bottom.x==250 && bottom.y==440);
    assert(top.x==250 && top.y==120);
    assert(mid.x==250 && mid.y==120);
    assert(esp::LineTarget(250,120,440,3).y==440);
    assert(esp::LineTarget(250,430,440,1).y==430); // Knocked/short box uses actual top.
    esp::Point tip{}, dir{};
    assert(esp::Arrow(800,600,1,0,tip,dir) && tip.x==772 && tip.y==300);
    assert(esp::Arrow(800,600,0,-1,tip,dir) && tip.x==400 && tip.y==28);
    assert(esp::Arrow(800,600,0,1,tip,dir) && tip.y==572);
    assert(esp::Arrow(800,600,-1,0,tip,dir) && tip.x==28);
    for (float x : {-100.f,-1.f,0.f,1.f,100.f})
        for (float y : {-100.f,-1.f,0.f,1.f,100.f}) {
            if (x==0 && y==0) continue;
            assert(esp::Arrow(2560,1080,x,y,tip,dir));
            assert(tip.x>=27.9f && tip.x<=2532.1f && tip.y>=27.9f && tip.y<=1052.1f);
        }
    assert(!esp::Arrow(800,600,0,0,tip,dir));
    assert(!esp::Arrow(32,32,1,1,tip,dir));
    assert(!esp::Arrow(800,600,std::numeric_limits<float>::infinity(),1,tip,dir));
}
'''
        with tempfile.TemporaryDirectory(prefix='comet-esp-test-') as temp:
            folder = Path(temp)
            (folder / 'test.cpp').write_text(source, encoding='utf-8')
            batch = folder / 'test.cmd'
            batch.write_text('@call "' + sorted(candidates)[-1] + '" >nul\n'
                             '@cl /nologo /std:c++20 /EHsc test.cpp /Fe:test.exe\n'
                             '@if errorlevel 1 exit /b 1\n@test.exe\n', encoding='ascii')
            result = subprocess.run(['cmd','/d','/c',str(batch)],cwd=folder,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
