"""Native assertions exercise the actual C++ config flow with isolated in-memory IO."""
import glob
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


def cpp_function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class ConfigStoreTests(unittest.TestCase):
    def test_native_save_load_migration_and_failures(self):
        project = Path(__file__).resolve().parents[1]
        source = (project / 'src/overlay.cpp').read_text(encoding='utf-8')
        candidates = []
        for variable in ('ProgramFiles', 'ProgramFiles(x86)'):
            directory = os.environ.get(variable)
            if directory:
                candidates.extend(glob.glob(str(Path(directory) / 'Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat')))
        if not candidates:
            self.skipTest('MSVC is required for the native config regression harness')
        functions = '\n'.join(cpp_function(source, signature) for signature in (
            'static void DefaultCfg()', 'static bool LoadCfg()', 'static bool SaveCfg()',
            'bool SaveConfiguration()', 'const char* ConfigurationStatus()'))
        version = re.search(r'static const uint32_t VERSION = (\d+);', source)[1]
        header = str(project / 'src/bs_cfg.hpp').replace('\\', '/')
        prefix = r'''
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <limits>
#include "HEADER_PATH"
#include "IMGUI_PATH"
static constexpr uint32_t CFG_MAGIC=0x42534346, VERSION=VERSION_VALUE;
Cfg g_cfg{};
bool g_cfg_dirty=false, g_cfg_ready=false, g_cfg_writable=true;
std::string g_cfg_error;
Cfg shared{};
void* g_shm_cfg_p=&shared;
std::unordered_map<std::wstring,std::vector<uint8_t>> files;
bool fail_saved=false, fail_ipc=false;
const std::wstring& SavedCfgPath(){static std::wstring p=L"saved";return p;}
const std::wstring& CfgPath(){static std::wstring p=L"ipc";return p;}
DWORD FakeAttributes(const wchar_t* p){return files.count(p)?FILE_ATTRIBUTE_NORMAL:INVALID_FILE_ATTRIBUTES;}
DWORD FakeError(){return ERROR_FILE_NOT_FOUND;}
#define GetFileAttributesW FakeAttributes
#define GetLastError FakeError
bool ReadAll(const wchar_t* p,std::vector<uint8_t>& out){if(!files.count(p))return false;out=files[p];return true;}
bool WriteAll(const wchar_t* p,const void* data,size_t count){
 if((std::wstring(p)==L"saved"&&fail_saved)||(std::wstring(p)==L"ipc"&&fail_ipc))return false;
 auto ptr=static_cast<const uint8_t*>(data);files[p]=std::vector<uint8_t>(ptr,ptr+count);return true;
}
void put(const wchar_t* p,const Cfg& c){auto ptr=reinterpret_cast<const uint8_t*>(&c);files[p]={ptr,ptr+sizeof(c)};}
Cfg get(const wchar_t* p){Cfg c{};assert(files[p].size()==sizeof(c));memcpy(&c,files[p].data(),sizeof(c));return c;}
'''.replace('HEADER_PATH', header).replace('IMGUI_PATH', (project / 'third_party/imgui-master/imgui.h').as_posix()).replace('VERSION_VALUE', version)
        checks = r'''
int main(){
 static_assert(sizeof(Cfg)==160);
 DefaultCfg();g_cfg_ready=g_cfg_dirty=true;
 assert(!(g_cfg.reserved&OPT_AIM_SILENT));
 assert(g_cfg.col_fov==IM_COL32(89,162,255,180));
 g_cfg.col_fov=IM_COL32(220,40,90,200);
 g_cfg.flags|=F_EXIT_CHEAT|F_MENU_OPEN|F_AIM_HUMANIZE;
 g_cfg.apply_seq=17;g_cfg.apply_type=2;g_cfg.apply_item=42;
 g_cfg.aim_bone=4;g_cfg.human_smooth=.8f;g_cfg.pad[1]=3;g_cfg.pad[2]=1;
 g_cfg.esp_style=ESP_OFFSCREEN_ARROW|2u;
 g_cfg.reserved|=OPT_AIM_SILENT;
 assert(SaveCfg());assert(!g_cfg_dirty);assert(shared.apply_seq==17);
 Cfg disk=get(L"saved");
 assert(disk.esp_style==(ESP_OFFSCREEN_ARROW|2u));
 assert(disk.reserved&OPT_AIM_SILENT);
 assert(disk.col_fov==IM_COL32(220,40,90,200));
 assert(!(disk.flags&(F_EXIT_CHEAT|F_MENU_OPEN)) && (disk.flags&F_AIM_HUMANIZE));
 assert(disk.apply_seq==0&&disk.apply_type==0&&disk.apply_item==0);
 put(L"saved",disk);
 assert(get(L"ipc").apply_seq==17);
 Cfg legacy=disk;legacy.aim_smooth=.1f;put(L"ipc",legacy);
 assert(LoadCfg());assert(g_cfg.aim_bone==4&&g_cfg.human_smooth==.8f);
 assert(g_cfg.reserved&OPT_AIM_SILENT);
 assert(g_cfg.col_fov==IM_COL32(220,40,90,200));
 assert(g_cfg.pad[1]==3&&g_cfg.pad[2]==1&&g_cfg.flags&F_MENU_OPEN);
 assert(!(g_cfg.flags&F_EXIT_CHEAT)&&g_cfg.apply_seq==0&&g_cfg_dirty);
 files.erase(L"ipc");assert(LoadCfg());assert(g_cfg.human_smooth==.8f);
 files.erase(L"saved");put(L"ipc",legacy);assert(LoadCfg());assert(g_cfg.aim_smooth==.1f);
 assert(SaveCfg());assert(files.count(L"saved"));
 files[L"saved"]={1,2,3};assert(!LoadCfg());assert(!g_cfg_writable);
 auto damaged=files[L"saved"];assert(!SaveCfg());assert(files[L"saved"]==damaged&&g_cfg_dirty);
 assert(SaveConfiguration());assert(files[L"saved"].size()==sizeof(Cfg));
 auto old=files[L"saved"];g_cfg.aim_smooth=.7f;g_cfg_dirty=true;fail_saved=true;
 assert(!SaveCfg());assert(g_cfg_dirty&&files[L"saved"]==old&&!g_cfg_error.empty());
 fail_saved=false;assert(SaveCfg());assert(get(L"saved").aim_smooth==.7f);
 fail_ipc=true;g_cfg_dirty=true;assert(!SaveCfg());assert(g_cfg_dirty);fail_ipc=false;
 assert(SaveCfg());assert(!g_cfg_dirty&&g_cfg_error.empty());
 disk=get(L"saved");disk.human_smooth=15;disk.aim_smooth=std::numeric_limits<float>::quiet_NaN();
 disk.aim_fov=std::numeric_limits<float>::infinity();put(L"saved",disk);
 assert(LoadCfg());assert(g_cfg.human_smooth==.5f&&g_cfg.aim_smooth==.35f&&g_cfg.aim_fov==8.f);
 disk=get(L"saved");disk.reserved=0;disk.aim_bone=4;disk.flags|=F_AIM_MULTIPOINT;put(L"saved",disk);
 assert(LoadCfg());assert((g_cfg.reserved&OPT_AIM_BONE_MASK)==OPT_AIM_BONE_MASK);
 assert((g_cfg.reserved&OPT_AIM_BONES_SET)&&!(g_cfg.flags&F_AIM_MULTIPOINT));
 disk.reserved=OPT_AIM_BONES_SET|OPT_AIM_NECK|OPT_AIM_IGNORE_KNOCKED;put(L"saved",disk);
 assert(LoadCfg());assert((g_cfg.reserved&OPT_AIM_BONE_MASK)==OPT_AIM_NECK);
 assert(g_cfg.reserved&OPT_AIM_IGNORE_KNOCKED);assert(SaveCfg());
 assert((get(L"saved").reserved&OPT_AIM_BONE_MASK)==OPT_AIM_NECK);
 disk=get(L"saved");disk.reserved&=~OPT_FOV_COLOR_SET;disk.col_fov=5;put(L"saved",disk);
 assert(LoadCfg());assert(g_cfg.col_fov==IM_COL32(89,162,255,180));
 g_cfg.col_fov=0;assert(SaveCfg());assert(LoadCfg());assert(g_cfg.col_fov==0);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='ciye-config-test-') as temp:
            folder = Path(temp)
            cpp = folder / 'config_test.cpp'
            cpp.write_text(prefix + functions + checks, encoding='utf-8')
            batch = folder / 'test.cmd'
            batch.write_text('@call "'+sorted(candidates)[-1]+'" >nul\n'
                             '@cl /nologo /std:c++20 /EHsc config_test.cpp /Fe:config_test.exe\n'
                             '@if errorlevel 1 exit /b 1\n@config_test.exe\n', encoding='ascii')
            result = subprocess.run(['cmd', '/d', '/c', str(batch)], cwd=folder, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
