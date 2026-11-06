#include <windows.h>
#include <cstdarg>
#include <cstdint>

namespace {
constexpr uintptr_t kPyGILStateEnsure = 0x2ADC1C0;
constexpr uintptr_t kPyGILStateRelease = 0x2ADC290;
constexpr uintptr_t kPyRunSimpleStringFlags = 0x2A9DEE0;

constexpr const char* kLogPath = "bs_cheat_log.txt";

using PyGILStateEnsureFn = int(__cdecl*)();
using PyGILStateReleaseFn = void(__cdecl*)(int);
using PyRunSimpleStringFlagsFn = int(__cdecl*)(const char*, void*);

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    wvsprintfA(buf, fmt, args);
    va_end(args);
    HANDLE h = CreateFileA(kLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD written = 0;
    WriteFile(h, buf, (DWORD)lstrlenA(buf), &written, nullptr);
    WriteFile(h, "\r\n", 2, &written, nullptr);
    CloseHandle(h);
}

DWORD WINAPI Worker(LPVOID) {
    Log("worker started");
    HMODULE game = nullptr;
    for (int i = 0; i < 600 && !game; ++i) {
        game = GetModuleHandleW(L"BloodStrike.exe");
        if (!game) Sleep(100);
    }
    if (!game) {
        Log("BloodStrike.exe not found");
        return 0;
    }
    Log("game base=%p", game);
    Sleep(12000);

    auto ensure = reinterpret_cast<PyGILStateEnsureFn>(reinterpret_cast<uintptr_t>(game) + kPyGILStateEnsure);
    auto release = reinterpret_cast<PyGILStateReleaseFn>(reinterpret_cast<uintptr_t>(game) + kPyGILStateRelease);
    auto run = reinterpret_cast<PyRunSimpleStringFlagsFn>(reinterpret_cast<uintptr_t>(game) + kPyRunSimpleStringFlags);

    Log("calling PyGILState_Ensure @ %p", ensure);
    int state = ensure();
    Log("gil state=%d", state);

    const char* code =
        "import os, sys, tempfile, traceback\n"
        "try:\n"
        "    output = os.path.join(tempfile.gettempdir(), 'bs_python_probe.txt')\n"
        "    with open(output,'w',encoding='utf-8') as f:\n"
        "        f.write('python-ok\\n')\n"
        "        f.write(sys.version + '\\n')\n"
        "        f.write('modules=' + str(len(sys.modules)) + '\\n')\n"
        "except Exception:\n"
        "    pass\n";

    Log("calling PyRun_SimpleStringFlags @ %p", run);
    int rc = run(code, nullptr);
    Log("run rc=%d", rc);

    release(state);
    Log("released");
    return 0;
}
}  // namespace

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Log("dll attached");
        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
