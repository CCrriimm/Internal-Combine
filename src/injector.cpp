#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace {

typedef LONG NTSTATUS;

struct ProcessBasicInformation {
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
};

struct UnicodeStringRef {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct LdrEntry {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UnicodeStringRef FullDllName;
    UnicodeStringRef BaseDllName;
};

using NtQueryInformationProcessFn = NTSTATUS(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtCreateThreadExFn = NTSTATUS(WINAPI*)(PHANDLE, ACCESS_MASK, PVOID, HANDLE, PVOID,
                                             PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);
using NtReadVirtualMemoryFn = NTSTATUS(WINAPI*)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);

std::wstring g_log_path;
NtReadVirtualMemoryFn g_nt_read = nullptr;

bool InitNtRead() {
    if (g_nt_read) return true;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    g_nt_read = reinterpret_cast<NtReadVirtualMemoryFn>(
        GetProcAddress(ntdll, "NtReadVirtualMemory"));
    return g_nt_read != nullptr;
}

bool ReadBlock(HANDLE process, uintptr_t address, void* buffer, SIZE_T size, SIZE_T* got) {
    if (!g_nt_read) return false;
    SIZE_T local = 0;
    NTSTATUS status = g_nt_read(process, reinterpret_cast<PVOID>(address), buffer, size,
                                got ? got : &local);
    return status == 0;
}

void Log(const char* fmt, ...) {
    char body[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[2200];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, body);
    std::fputs(line, stdout);
    if (!g_log_path.empty()) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_log_path.c_str(), L"a") == 0 && f) {
            std::fputs(line, f);
            std::fclose(f);
        }
    }
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n) return L".";
    std::wstring s(path, n);
    size_t pos = s.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : s.substr(0, pos);
}

template <typename T>
bool ReadMem(HANDLE process, uintptr_t address, T& out) {
    SIZE_T got = 0;
    return ReadBlock(process, address, &out, sizeof(T), &got) && got == sizeof(T);
}

template <typename T>
bool WriteMem(HANDLE process, uintptr_t address, const T& in) {
    SIZE_T put = 0;
    return WriteProcessMemory(process, reinterpret_cast<LPVOID>(address), &in, sizeof(T), &put) &&
           put == sizeof(T);
}

std::wstring BaseName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

DWORD FindProcess(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

DWORD WaitForProcess(const wchar_t* name, int seconds) {
    int tries = seconds > 0 ? seconds * 2 : 1;
    for (int i = 0; i < tries; ++i) {
        DWORD pid = FindProcess(name);
        if (pid) return pid;
        Sleep(500);
    }
    return 0;
}

bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid)) {
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(token);
    return ok && err == ERROR_SUCCESS;
}

uintptr_t QueryPeb(HANDLE process) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return 0;
    auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!query) return 0;
    ProcessBasicInformation info{};
    ULONG size = 0;
    NTSTATUS status = query(process, 0, &info, sizeof(info), &size);
    return status == 0 ? reinterpret_cast<uintptr_t>(info.PebBaseAddress) : 0;
}

bool ReadRemoteWide(HANDLE process, const UnicodeStringRef& str, std::wstring& out) {
    if (!str.Buffer || str.Length == 0) return false;
    wchar_t buf[512]{};
    SIZE_T bytes = str.Length;
    if (bytes > sizeof(buf) - sizeof(wchar_t)) bytes = sizeof(buf) - sizeof(wchar_t);
    SIZE_T got = 0;
    if (!ReadBlock(process, reinterpret_cast<uintptr_t>(str.Buffer), buf, bytes, &got) || got == 0)
        return false;
    buf[bytes / sizeof(wchar_t)] = 0;
    out.assign(buf);
    return true;
}

uintptr_t FindModuleBase(HANDLE process, uintptr_t peb, const std::wstring& module_name) {
    if (!peb) return 0;
    uintptr_t ldr = 0;
    if (!ReadMem(process, peb + 0x18, ldr) || !ldr) return 0;
    uintptr_t head = ldr + 0x10;
    uintptr_t cursor = 0;
    if (!ReadMem(process, head, cursor)) return 0;
    for (int guard = 0; guard < 2048 && cursor && cursor != head; ++guard) {
        LdrEntry entry{};
        if (!ReadMem(process, cursor - 0x10, entry)) return 0;
        std::wstring name;
        if (ReadRemoteWide(process, entry.BaseDllName, name) &&
            _wcsicmp(name.c_str(), module_name.c_str()) == 0) {
            return reinterpret_cast<uintptr_t>(entry.DllBase);
        }
        uintptr_t next = 0;
        if (!ReadMem(process, cursor, next)) return 0;
        cursor = next;
    }
    return 0;
}

void ZeroRemoteString(HANDLE process, const UnicodeStringRef& str) {
    if (!str.Buffer || str.Length == 0) return;
    std::vector<BYTE> zeros(str.Length, 0);
    SIZE_T put = 0;
    WriteProcessMemory(process, str.Buffer, zeros.data(), zeros.size(), &put);
}

bool HideModuleFromPeb(HANDLE process, uintptr_t module_base, uintptr_t peb) {
    if (!module_base || !peb) return false;
    uintptr_t ldr = 0;
    if (!ReadMem(process, peb + 0x18, ldr) || !ldr) return false;
    uintptr_t head = ldr + 0x10;
    uintptr_t cursor = 0;
    if (!ReadMem(process, head, cursor)) return false;

    for (int guard = 0; guard < 2048 && cursor && cursor != head; ++guard) {
        uintptr_t entry_address = cursor - 0x10;
        LdrEntry entry{};
        if (!ReadMem(process, entry_address, entry)) return false;
        if (reinterpret_cast<uintptr_t>(entry.DllBase) == module_base) {
            const uintptr_t links[3] = {entry_address, entry_address + 0x10,
                                        entry_address + 0x20};
            for (uintptr_t link_address : links) {
                LIST_ENTRY link{};
                if (!ReadMem(process, link_address, link)) continue;
                uintptr_t prev = reinterpret_cast<uintptr_t>(link.Blink);
                uintptr_t next = reinterpret_cast<uintptr_t>(link.Flink);
                if (!prev || !next) continue;
                WriteMem(process, prev, next);
                WriteMem(process, next + 8, prev);
            }
            ZeroRemoteString(process, entry.BaseDllName);
            ZeroRemoteString(process, entry.FullDllName);
            return true;
        }
        uintptr_t next = 0;
        if (!ReadMem(process, cursor, next)) return false;
        cursor = next;
    }
    return false;
}

HANDLE CreateHiddenThread(HANDLE process, void* start, void* arg) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto create = reinterpret_cast<NtCreateThreadExFn>(
            GetProcAddress(ntdll, "NtCreateThreadEx"));
        if (create) {
            HANDLE thread = nullptr;
            constexpr ULONG kHideFromDebugger = 0x4;
            NTSTATUS status = create(&thread, THREAD_ALL_ACCESS, nullptr, process, start, arg,
                                     kHideFromDebugger, 0, 0, 0, nullptr);
            if (status == 0 && thread) return thread;
            Log("NtCreateThreadEx failed status=0x%lx, falling back", (unsigned long)status);
        }
    }
    return CreateRemoteThread(process, nullptr, 0,
                              reinterpret_cast<LPTHREAD_START_ROUTINE>(start), arg, 0, nullptr);
}

bool ParsePid(const char* text, DWORD& pid) {
    if (!text || !*text) return false;
    char* end = nullptr;
    unsigned long value = std::strtoul(text, &end, 10);
    if (!end || *end || value == 0) return false;
    pid = static_cast<DWORD>(value);
    return true;
}

void Usage() {
    Log("usage: injector <process.exe|pid> <dll-path> [--wait <seconds>] [--no-hide]");
}

struct RightsResult {
    HANDLE handle = nullptr;
    DWORD granted = 0;
    DWORD last_error = 0;
};

RightsResult OpenProcessProgressive(DWORD pid) {
    const DWORD masks[] = {
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
            PROCESS_QUERY_LIMITED_INFORMATION,
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION,
        PROCESS_QUERY_LIMITED_INFORMATION,
    };
    RightsResult result{};
    for (DWORD mask : masks) {
        HANDLE handle = OpenProcess(mask, FALSE, pid);
        if (handle) {
            result.handle = handle;
            result.granted = mask;
            return result;
        }
        result.last_error = GetLastError();
        Log("OpenProcess mask=0x%lX denied (err=%lu), trying lower rights",
            (unsigned long)mask, (unsigned long)result.last_error);
    }
    return result;
}

void LogProtectionLevel(HANDLE process) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!query) return;
    struct ProtectionInfo {
        BYTE Level;
        BYTE Type;
        BYTE Audit;
        BYTE Signer;
    } info{};
    ULONG size = 0;
    NTSTATUS status = query(process, 61, &info, sizeof(info), &size);
    if (status == 0) {
        Log("process protection level=%u type=%u signer=%u (HyperShield/PPL probe)",
            (unsigned)info.Level, (unsigned)info.Type, (unsigned)info.Signer);
    }
}

}  // namespace

int main(int argc, char** argv) {
    g_log_path = ExeDir() + L"\\inject_log.txt";

    const char* target = nullptr;
    const char* dll_arg = nullptr;
    int wait_seconds = 0;
    bool hide = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-hide") == 0) {
            hide = false;
        } else if (std::strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
            wait_seconds = std::atoi(argv[++i]);
        } else if (!target) {
            target = argv[i];
        } else if (!dll_arg) {
            dll_arg = argv[i];
        }
    }
    if (!target || !dll_arg) {
        Usage();
        return 1;
    }

    Log("injector start target=%s dll=%s hide=%d", target, dll_arg, hide ? 1 : 0);

    DWORD pid = 0;
    if (!ParsePid(target, pid)) {
        std::wstring wide(target, target + std::strlen(target));
        pid = WaitForProcess(wide.c_str(), wait_seconds);
        if (!pid) {
            Log("process not found: %s", target);
            return 2;
        }
    }

    wchar_t dll[MAX_PATH];
    DWORD dll_len = GetFullPathNameW(std::wstring(dll_arg, dll_arg + std::strlen(dll_arg)).c_str(),
                                     MAX_PATH, dll, nullptr);
    if (!dll_len || dll_len >= MAX_PATH) {
        Log("bad dll path: %s", dll_arg);
        return 3;
    }
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        Log("dll does not exist: %ls", dll);
        return 3;
    }

    if (!EnableDebugPrivilege()) {
        Log("warning: SeDebugPrivilege not enabled (not elevated?)");
    }
    if (!InitNtRead()) {
        Log("fatal: NtReadVirtualMemory not available in ntdll");
        return 5;
    }

    RightsResult rights = OpenProcessProgressive(pid);
    HANDLE process = rights.handle;
    if (!process) {
        Log("OpenProcess failed after progressive fallback: %lu", rights.last_error);
        if (rights.last_error == ERROR_ACCESS_DENIED) {
            Log("hint: run as Administrator; HyperShield strips handles for protected processes");
        }
        return 4;
    }
    Log("OpenProcess granted mask=0x%lX", (unsigned long)rights.granted);
    LogProtectionLevel(process);

    const DWORD needed = PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                         PROCESS_VM_READ;
    if ((rights.granted & needed) != needed) {
        Log("fatal: kernel ObCallback stripped required rights (have=0x%lX need=0x%lX)",
            (unsigned long)rights.granted, (unsigned long)needed);
        CloseHandle(process);
        return 4;
    }

    const SIZE_T path_bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, path_bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        Log("VirtualAllocEx failed: %lu", GetLastError());
        CloseHandle(process);
        return 5;
    }
    if (!WriteProcessMemory(process, remote, dll, path_bytes, nullptr)) {
        Log("WriteProcessMemory failed: %lu", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 5;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    void* load_library = reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryW"));
    if (!load_library) {
        Log("LoadLibraryW not found");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 6;
    }

    HANDLE thread = CreateHiddenThread(process, load_library, remote);
    if (!thread) {
        Log("thread creation failed: %lu", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 6;
    }

    DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);

    std::wstring module_name = BaseName(dll);
    uintptr_t peb = QueryPeb(process);
    uintptr_t module_base = FindModuleBase(process, peb, module_name);
    if (!module_base && wait == WAIT_OBJECT_0 && exit_code) {
        module_base = static_cast<uintptr_t>(exit_code);
    }
    if (!module_base) {
        Log("injection failed: module not present (thread_wait=%lu exit=0x%lx)",
            (unsigned long)wait, (unsigned long)exit_code);
        CloseHandle(process);
        return 7;
    }

    bool hidden = false;
    if (hide) {
        hidden = HideModuleFromPeb(process, module_base, peb);
        if (!hidden) Log("warning: PEB hide failed (injection still usable)");
    }
    Log("injected pid=%lu module=0x%llx hidden=%s", (unsigned long)pid,
        (unsigned long long)module_base, hidden ? "yes" : "no");
    CloseHandle(process);
    return 0;
}
