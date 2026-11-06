#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>
#include "bskdrv_ioctl.h"

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

static DWORD FindPid(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

class Driver {
public:
    ~Driver() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }

    bool Open() {
        h_ = CreateFileW(BSK_WIN32_NAME, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        return h_ != INVALID_HANDLE_VALUE;
    }

    bool Read(u64 addr, void* out, size_t size) {
        if (!size || size > 16ull * 1024 * 1024) return false;
        BSK_REQ req{};
        req.pid = pid_;
        req.addr = addr;
        req.size = (u32)size;
        std::vector<u8> buf(sizeof(BSK_REQ) + size);
        DWORD got = 0;
        if (!DeviceIoControl(h_, IOCTL_BSK_READ, &req, sizeof(req), buf.data(),
                             (DWORD)buf.size(), &got, nullptr)) {
            return false;
        }
        if (got < sizeof(BSK_REQ) + size) return false;
        memcpy(out, buf.data() + sizeof(BSK_REQ), size);
        return true;
    }

    bool Write(u64 addr, const void* data, size_t size) {
        if (!size || size > 16ull * 1024 * 1024) return false;
        BSK_REQ req{};
        req.pid = pid_;
        req.addr = addr;
        req.size = (u32)size;
        std::vector<u8> buf(sizeof(BSK_REQ) + size);
        memcpy(buf.data(), &req, sizeof(req));
        memcpy(buf.data() + sizeof(req), data, size);
        DWORD got = 0;
        return DeviceIoControl(h_, IOCTL_BSK_WRITE, buf.data(), (DWORD)buf.size(),
                               nullptr, 0, &got, nullptr) != FALSE;
    }

    bool Alloc(u64 desired, size_t size, u32 prot, u64& out) {
        BSK_REQ req{};
        req.pid = pid_;
        req.addr = desired;
        req.size = (u32)size;
        req.prot = prot;
        BSK_REQ res{};
        DWORD got = 0;
        if (!DeviceIoControl(h_, IOCTL_BSK_ALLOC, &req, sizeof(req), &res, sizeof(res), &got, nullptr)) {
            return false;
        }
        if (got < sizeof(res)) return false;
        out = res.out1;
        return out != 0;
    }

    bool Free(u64 addr) {
        BSK_REQ req{};
        req.pid = pid_;
        req.addr = addr;
        BSK_REQ res{};
        DWORD got = 0;
        return DeviceIoControl(h_, IOCTL_BSK_FREE, &req, sizeof(req), &res, sizeof(res), &got, nullptr) != FALSE;
    }

    bool Protect(u64 addr, size_t size, u32 prot, u32* old = nullptr) {
        BSK_REQ req{};
        req.pid = pid_;
        req.addr = addr;
        req.size = (u32)size;
        req.prot = prot;
        BSK_REQ res{};
        DWORD got = 0;
        if (!DeviceIoControl(h_, IOCTL_BSK_PROTECT, &req, sizeof(req), &res, sizeof(res), &got, nullptr)) {
            return false;
        }
        if (old) *old = (u32)res.out2;
        return true;
    }

    bool Thread(u64 start, u64 param, u64& tid) {
        BSK_REQ req{};
        req.pid = pid_;
        req.addr = start;
        req.param = param;
        BSK_REQ res{};
        DWORD got = 0;
        if (!DeviceIoControl(h_, IOCTL_BSK_THREAD, &req, sizeof(req), &res, sizeof(res), &got, nullptr)) {
            return false;
        }
        tid = res.out1;
        return true;
    }

    bool ModuleRaw(const std::wstring& name, u64& base, u64& size) {
        BSK_REQ req{};
        req.pid = pid_;
        size_t n = std::min<size_t>(name.size(), 127);
        memcpy(req.name, name.data(), n * sizeof(wchar_t));
        BSK_REQ res{};
        DWORD got = 0;
        if (!DeviceIoControl(h_, IOCTL_BSK_MODULE, &req, sizeof(req), &res, sizeof(res), &got, nullptr)) {
            return false;
        }
        base = res.out1;
        size = res.out2;
        return base != 0;
    }

    bool Module(const std::wstring& name, u64& base, u64& size) {
        if (ModuleRaw(name, base, size)) return true;
        if (name.find(L'.') == std::wstring::npos) {
            return ModuleRaw(name + L".dll", base, size);
        }
        return false;
    }

    void SetPid(DWORD pid) { pid_ = pid; }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    DWORD pid_ = 0;
};

static std::string ReadAscii(Driver& d, u64 addr, size_t max) {
    std::vector<char> buf(max, 0);
    if (!d.Read(addr, buf.data(), max)) return {};
    size_t n = 0;
    while (n < max && buf[n]) n++;
    return std::string(buf.data(), n);
}

static u64 RemoteExport(Driver& d, DWORD pid, u64 base, const char* name, int depth = 0);

static bool LoadRemoteModule(Driver& d, DWORD pid, const std::wstring& name, u64& base) {
    u64 size = 0;
    if (d.Module(name, base, size)) return true;
    u64 k32 = 0, k32sz = 0;
    if (!d.Module(L"kernel32.dll", k32, k32sz)) return false;
    u64 load_lib = RemoteExport(d, pid, k32, "LoadLibraryW");
    if (!load_lib) return false;
    std::wstring full = name;
    size_t bytes = (full.size() + 1) * sizeof(wchar_t);
    u64 mem = 0;
    if (!d.Alloc(0, bytes, PAGE_READWRITE, mem)) return false;
    if (!d.Write(mem, full.c_str(), bytes)) {
        d.Free(mem);
        return false;
    }
    u64 tid = 0;
    if (!d.Thread(load_lib, mem, tid)) {
        d.Free(mem);
        return false;
    }
    bool ok = false;
    for (int i = 0; i < 24; ++i) {
        Sleep(250);
        if (d.Module(name, base, size)) {
            ok = true;
            break;
        }
    }
    d.Free(mem);
    return ok;
}

static u64 RemoteExportRVA(Driver& d, u64 base, const char* name, IMAGE_EXPORT_DIRECTORY* out_exp = nullptr) {
    IMAGE_DOS_HEADER dos{};
    if (!d.Read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 nt{};
    if (!d.Read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) return 0;
    if (nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
    auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;
    IMAGE_EXPORT_DIRECTORY exp{};
    if (!d.Read(base + dir.VirtualAddress, &exp, sizeof(exp))) return 0;
    if (out_exp) *out_exp = exp;
    if (!exp.NumberOfNames || exp.NumberOfNames > 65536 || !exp.NumberOfFunctions ||
        exp.NumberOfFunctions > 65536) {
        return 0;
    }
    std::vector<u32> names(exp.NumberOfNames);
    std::vector<u16> ords(exp.NumberOfNames);
    std::vector<u32> funcs(exp.NumberOfFunctions);
    if (!d.Read(base + exp.AddressOfNames, names.data(), names.size() * sizeof(u32))) return 0;
    if (!d.Read(base + exp.AddressOfNameOrdinals, ords.data(), ords.size() * sizeof(u16))) return 0;
    if (!d.Read(base + exp.AddressOfFunctions, funcs.data(), funcs.size() * sizeof(u32))) return 0;
    for (u32 i = 0; i < exp.NumberOfNames; ++i) {
        std::string n = ReadAscii(d, base + names[i], 256);
        if (n == name) {
            u16 ord = ords[i];
            if (ord < funcs.size()) return funcs[ord];
            return 0;
        }
    }
    return 0;
}

static u64 RemoteExport(Driver& d, DWORD pid, u64 base, const char* name, int depth) {
    if (depth > 8) return 0;
    IMAGE_EXPORT_DIRECTORY exp{};
    u32 rva = (u32)RemoteExportRVA(d, base, name, &exp);
    if (!rva) return 0;
    IMAGE_DOS_HEADER dos{};
    if (!d.Read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 nt{};
    if (!d.Read(base + dos.e_lfanew, &nt, sizeof(nt))) return 0;
    auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) {
        std::string fwd = ReadAscii(d, base + rva, 256);
        size_t dot = fwd.find('.');
        if (dot == std::string::npos) return 0;
        std::wstring mod;
        for (size_t i = 0; i < dot; ++i) mod.push_back((wchar_t)fwd[i]);
        mod += L".dll";
        std::string fn = fwd.substr(dot + 1);
        u64 mb = 0;
        if (!LoadRemoteModule(d, pid, mod, mb)) return 0;
        return RemoteExport(d, pid, mb, fn.c_str(), depth + 1);
    }
    return base + rva;
}

static u64 RemoteOrdinal(Driver& d, u64 base, u16 ord) {
    IMAGE_DOS_HEADER dos{};
    if (!d.Read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 nt{};
    if (!d.Read(base + dos.e_lfanew, &nt, sizeof(nt))) return 0;
    auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return 0;
    IMAGE_EXPORT_DIRECTORY exp{};
    if (!d.Read(base + dir.VirtualAddress, &exp, sizeof(exp))) return 0;
    if (ord < exp.Base) return 0;
    u32 idx = ord - exp.Base;
    if (idx >= exp.NumberOfFunctions) return 0;
    u32 rva = 0;
    if (!d.Read(base + exp.AddressOfFunctions + idx * sizeof(u32), &rva, sizeof(rva))) return 0;
    return rva ? base + rva : 0;
}

static u32 SectionProtect(u32 characteristics) {
    bool exec = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    bool read = (characteristics & IMAGE_SCN_MEM_READ) != 0;
    bool write = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    if (exec && write) return PAGE_EXECUTE_READWRITE;
    if (exec) return read ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (write) return PAGE_READWRITE;
    return PAGE_READONLY;
}

static bool EnsureImportsLoaded(Driver& d, DWORD pid, const std::vector<u8>& image,
                                const IMAGE_NT_HEADERS64& nt) {
    auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return true;
    if (dir.VirtualAddress + dir.Size > image.size()) return false;
    auto desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image.data() + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        if (desc->Name >= image.size()) return false;
        const char* name = reinterpret_cast<const char*>(image.data() + desc->Name);
        std::wstring wide;
        for (const char* p = name; *p; ++p) wide.push_back((wchar_t)(unsigned char)*p);
        u64 base = 0;
        if (!LoadRemoteModule(d, pid, wide, base)) {
            printf("[!] cannot load import module %s\n", name);
            return false;
        }
    }
    return true;
}

static bool ApplyRelocations(std::vector<u8>& image, const IMAGE_NT_HEADERS64& nt, u64 newBase) {
    auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!dir.VirtualAddress || !dir.Size) return newBase == nt.OptionalHeader.ImageBase;
    if (dir.VirtualAddress + dir.Size > image.size()) return false;
    i64 delta = (i64)(newBase - nt.OptionalHeader.ImageBase);
    size_t off = 0;
    while (off + sizeof(IMAGE_BASE_RELOCATION) <= dir.Size) {
        auto block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(image.data() + dir.VirtualAddress + off);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
        size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(u16);
        auto entries = reinterpret_cast<const u16*>(block + 1);
        for (size_t i = 0; i < count; ++i) {
            u16 type = entries[i] >> 12;
            u16 rva = entries[i] & 0x0FFF;
            u64 at = (u64)block->VirtualAddress + rva;
            if (at + sizeof(u64) > image.size()) continue;
            if (type == IMAGE_REL_BASED_DIR64) {
                u64* p = reinterpret_cast<u64*>(image.data() + at);
                *p = (u64)((i64)*p + delta);
            } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                u32* p = reinterpret_cast<u32*>(image.data() + at);
                *p = (u32)((i64)*p + delta);
            }
        }
        off += block->SizeOfBlock;
    }
    return true;
}

static bool ApplyImports(Driver& d, DWORD pid, std::vector<u8>& image, const IMAGE_NT_HEADERS64& nt) {
    auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return true;
    if (dir.VirtualAddress + dir.Size > image.size()) return false;
    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image.data() + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* modName = reinterpret_cast<const char*>(image.data() + desc->Name);
        std::wstring wide;
        for (const char* p = modName; *p; ++p) wide.push_back((wchar_t)(unsigned char)*p);
        u64 modBase = 0, modSize = 0;
        if (!d.Module(wide, modBase, modSize)) {
            printf("[!] import module missing: %s\n", modName);
            return false;
        }
        u32 oft = desc->OriginalFirstThunk;
        u32 ft = desc->FirstThunk;
        if (!oft || !ft) continue;
        if (oft >= image.size() || ft >= image.size()) return false;
        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(image.data() + oft);
        auto iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(image.data() + ft);
        for (; thunk->u1.AddressOfData; ++thunk, ++iat) {
            u64 val = thunk->u1.AddressOfData;
            u64 proc = 0;
            if (val & IMAGE_ORDINAL_FLAG64) {
                proc = RemoteOrdinal(d, modBase, (u16)(val & 0xFFFF));
            } else {
                if (val >= image.size()) return false;
                auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(image.data() + (u32)val);
                proc = RemoteExport(d, pid, modBase, reinterpret_cast<const char*>(ibn->Name));
            }
            if (!proc) {
                printf("[!] unresolved import in %s\n", modName);
                return false;
            }
            iat->u1.Function = proc;
        }
    }
    return true;
}

static bool MapDll(Driver& d, DWORD pid, const wchar_t* path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        printf("[!] cannot open %ls\n", path);
        return false;
    }
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(f, &li) || li.QuadPart <= 0 || li.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(f);
        printf("[!] bad size %ls\n", path);
        return false;
    }
    std::vector<u8> file((size_t)li.QuadPart);
    DWORD got = 0;
    BOOL ok = ReadFile(f, file.data(), (DWORD)file.size(), &got, nullptr);
    CloseHandle(f);
    if (!ok || got != file.size()) {
        printf("[!] read failed %ls\n", path);
        return false;
    }
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > file.size()) {
        printf("[!] not a PE: %ls\n", path);
        return false;
    }
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        printf("[!] not PE64: %ls\n", path);
        return false;
    }
    size_t imageSize = nt->OptionalHeader.SizeOfImage;
    if (imageSize < 0x1000 || imageSize > 128 * 1024 * 1024) {
        printf("[!] bad image size\n");
        return false;
    }
    std::vector<u8> image(imageSize, 0);
    size_t headerCopy = std::min<size_t>(nt->OptionalHeader.SizeOfHeaders, file.size());
    memcpy(image.data(), file.data(), headerCopy);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        size_t raw = std::min<size_t>(sec->SizeOfRawData, file.size() - std::min<size_t>(sec->PointerToRawData, file.size()));
        size_t virt = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        size_t n = std::min(raw, virt);
        if (!n || sec->VirtualAddress + n > image.size()) continue;
        memcpy(image.data() + sec->VirtualAddress,
               file.data() + sec->PointerToRawData, n);
    }

    if (!EnsureImportsLoaded(d, pid, image, *nt)) return false;

    u64 remote = 0;
    if (!d.Alloc(nt->OptionalHeader.ImageBase, imageSize, PAGE_READWRITE, remote)) {
        if (!d.Alloc(0, imageSize, PAGE_READWRITE, remote)) {
            printf("[!] allocation failed\n");
            return false;
        }
    }
    if (!ApplyRelocations(image, *nt, remote)) {
        printf("[!] relocation failed\n");
        d.Free(remote);
        return false;
    }
    if (!ApplyImports(d, pid, image, *nt)) {
        d.Free(remote);
        return false;
    }
    size_t zero = std::min<size_t>(nt->OptionalHeader.SizeOfHeaders, 0x1000);
    memset(image.data(), 0, zero);

    const size_t chunk = 0x10000;
    for (size_t off = 0; off < imageSize; off += chunk) {
        size_t n = std::min(chunk, imageSize - off);
        if (!d.Write(remote + off, image.data() + off, n)) {
            printf("[!] write failed at %llx\n", (unsigned long long)(remote + off));
            d.Free(remote);
            return false;
        }
    }

    sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        size_t sz = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (!sz) continue;
        sz = (sz + 0xFFF) & ~(size_t)0xFFF;
        if (sec->VirtualAddress + sz > imageSize) sz = imageSize - sec->VirtualAddress;
        d.Protect(remote + sec->VirtualAddress, sz, SectionProtect(sec->Characteristics));
    }
    d.Protect(remote, 0x1000, PAGE_READONLY);

    u64 stub = 0;
    if (!d.Alloc(0, 0x1000, PAGE_EXECUTE_READWRITE, stub)) {
        printf("[!] stub alloc failed\n");
        return false;
    }
    u64 entry = remote + nt->OptionalHeader.AddressOfEntryPoint;
    u8 code[80]{};
    size_t n = 0;
    // sub rsp, 0x28 (align stack and reserve shadow space)
    code[n++] = 0x48; code[n++] = 0x83; code[n++] = 0xEC; code[n++] = 0x28;
    // mov rcx, remote (hinstDLL)
    code[n++] = 0x48; code[n++] = 0xB9;
    memcpy(code + n, &remote, 8); n += 8;
    // mov edx, 1 (DLL_PROCESS_ATTACH)
    code[n++] = 0xBA;
    u32 attach = 1;
    memcpy(code + n, &attach, 4); n += 4;
    // xor r8d, r8d (lpReserved = NULL)
    code[n++] = 0x45; code[n++] = 0x31; code[n++] = 0xC0;
    // mov rax, entry
    code[n++] = 0x48; code[n++] = 0xB8;
    memcpy(code + n, &entry, 8); n += 8;
    // call rax
    code[n++] = 0xFF; code[n++] = 0xD0;
    // mov rax, stub
    code[n++] = 0x48; code[n++] = 0xB8;
    memcpy(code + n, &stub, 8); n += 8;
    // mov byte ptr [rax + 0x80], 1 (signal completion)
    code[n++] = 0xC6; code[n++] = 0x80; code[n++] = 0x80; code[n++] = 0x00;
    code[n++] = 0x00; code[n++] = 0x00; code[n++] = 0x01;
    // xor eax, eax
    code[n++] = 0x31; code[n++] = 0xC0;
    // add rsp, 0x28
    code[n++] = 0x48; code[n++] = 0x83; code[n++] = 0xC4; code[n++] = 0x28;
    // ret
    code[n++] = 0xC3;
    if (!d.Write(stub, code, n)) {
        printf("[!] stub write failed\n");
        d.Free(stub);
        return false;
    }
    u64 tid = 0;
    if (!d.Thread(stub, remote, tid)) {
        printf("[!] thread failed\n");
        d.Free(stub);
        return false;
    }
    bool init_ok = false;
    for (int i = 0; i < 40; ++i) {
        Sleep(50);
        u8 flag = 0;
        if (d.Read(stub + 0x80, &flag, 1) && flag == 1) {
            init_ok = true;
            break;
        }
    }
    if (!init_ok) {
        Sleep(500);
    }
    d.Free(stub);
    printf("[+] mapped %ls -> %llx (entry %llx)\n", path, (unsigned long long)remote,
           (unsigned long long)entry);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && _wcsicmp(argv[1], L"--probe") == 0) {
        Driver d;
        return d.Open() ? 0 : 3;
    }
    if (argc < 3) {
        printf("usage: bsmap.exe <process.exe> <dll> [dll...]\n");
        return 1;
    }
    DWORD pid = FindPid(argv[1]);
    if (!pid) {
        printf("[!] process not found: %ls\n", argv[1]);
        return 2;
    }
    Driver d;
    if (!d.Open()) {
        printf("[!] driver device not found. Load bskdrv.sys with kdmapper first.\n");
        return 3;
    }
    d.SetPid(pid);
    printf("[i] pid=%lu\n", (unsigned long)pid);
    for (int i = 2; i < argc; ++i) {
        if (!MapDll(d, pid, argv[i])) {
            printf("[!] mapping failed: %ls\n", argv[i]);
            return 4;
        }
        Sleep(700);
    }
    printf("[+] done\n");
    return 0;
}
