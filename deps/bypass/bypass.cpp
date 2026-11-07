// lang: C++20, file: bypass.cpp, target: Windows 11, MSVC
// BloodStrike QSec bypass — sourced from decomp analysis of:
//   gclient/framework/util/qsec.py
//   gclient/gamesystem/entities/avatarmembers/cimp_anti_plugin.py
//
// QSec detection vectors (Windows) and their counters:
//
//   1. FindQSec thread scan  — TH32CS_SNAPTHREAD + ZwQueryInformationThread(9)
//      looks for PAGE_EXECUTE_READWRITE region, size==0x85000, head=="cesQ"
//      Counter: spoof VirtualQueryEx result for that specific region
//
//   2. CheckWindowsQsecsdk MD5 — hashes qsec_start..qsec_end, reports to server
//      Counter: hook GetMemoryContent (MMyLighting) to return zeroed buffer
//              for addresses in the QSec range
//
//   3. OnRecoilEnterCheck — reads StateRecoil.OnRecoilEnter.__code__.co_consts
//      checks 0.01 is present (vanilla recoil constant)
//      Counter: patch the Python code object in-memory to restore 0.01
//
//   4. AntiCheckEssentialData — MD5 of loaded Python modules
//      Counter: hook calculate_md5 / hashlib internals to return expected hash
//
//   5. GetTextCRC — CRC32 of .text between two export anchors in bloodstrike.exe
//      Counter: restore original bytes at patched locations before CRC scan,
//              or hook zlib.crc32 at the Python C-extension level
//
//   6. Device ban fingerprint — HWID written to 4051BDD0000f042.pyo
//      Counter: intercept WritePhyDriveSerial, write spoofed uuid4 string
//
// NEW SDK audit (2026-11-05): the routines below are legacy reference code,
// not verified coverage for the current Steam path. The supplied DECOMP shows
// QSec calling Win32 through MMyLighting.CallFunction and its MD5 copy through
// the native dispatcher; the marker routines use Python open(). The old direct
// bloodstrike.exe IAT/ReadMem/WriteFile hooks are therefore not installed by
// bypass_init(). Keeping an unverified inline/IAT patch active would be less
// safe than an explicit no-op. External Comet has a separate input-noise-only
// helper and never calls this in-process module.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <winternl.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <random>
#include <algorithm>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "psapi.lib")

// ============================================================================
// Utility
// ============================================================================
namespace util
{
    // xoshiro256** PRNG seeded from RDTSC
    struct Prng {
        uint64_t s[4];
        Prng() {
            LARGE_INTEGER c{}; QueryPerformanceCounter(&c);
            uint64_t v = (uint64_t)c.QuadPart ^ __rdtsc();
            auto sm = [](uint64_t x) {
                x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
                x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
                return x ^ (x >> 31);
            };
            for (auto& lane : s) { v += 0x9E3779B97F4A7C15ULL; lane = sm(v); }
        }
        uint64_t next() {
            const uint64_t r = ((s[1] * 5) << 7 | (s[1] * 5) >> 57) * 9;
            const uint64_t t = s[1] << 17;
            s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
            s[2] ^= t; s[3] = s[3] << 45 | s[3] >> 19;
            return r;
        }
        // UUID v4 string
        std::string uuid4() {
            char buf[37];
            uint64_t a = next(), b = next();
            // Set version (4) and variant bits
            a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
            b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
            snprintf(buf, sizeof(buf),
                "%08x-%04x-%04x-%04x-%012llx",
                (uint32_t)(a >> 32),
                (uint16_t)(a >> 16),
                (uint16_t)(a),
                (uint16_t)(b >> 48),
                (unsigned long long)(b & 0x0000FFFFFFFFFFFFULL));
            return std::string(buf);
        }
    };

    // Write to read-only/execute memory
    static bool patch_bytes(uintptr_t addr, const uint8_t* src, size_t len)
    {
        DWORD old = 0;
        if (!VirtualProtect((LPVOID)addr, len, PAGE_EXECUTE_READWRITE, &old))
            return false;
        memcpy((void*)addr, src, len);
        VirtualProtect((LPVOID)addr, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)addr, len);
        return true;
    }

    // Find pattern in memory region
    static uintptr_t find_pattern(uintptr_t base, size_t size,
                                   const uint8_t* pat, const char* mask)
    {
        size_t plen = strlen(mask);
        for (size_t i = 0; i + plen <= size; i++) {
            bool ok = true;
            for (size_t j = 0; j < plen; j++) {
                if (mask[j] == 'x' && ((uint8_t*)(base + i))[j] != pat[j]) {
                    ok = false; break;
                }
            }
            if (ok) return base + i;
        }
        return 0;
    }

    // Get module base + size
    static bool get_module(const char* name, uintptr_t& base, size_t& size)
    {
        HMODULE h = GetModuleHandleA(name);
        if (!h) return false;
        MODULEINFO mi{};
        GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi));
        base = (uintptr_t)mi.lpBaseOfDll;
        size = mi.SizeOfImage;
        return true;
    }
}

// ============================================================================
// 1. QSec thread scan spoof — hook VirtualQueryEx
//    FindQSec calls VirtualQueryEx to check region size + protection.
//    We intercept it and for the QSec memory range return:
//      Protect = PAGE_EXECUTE_READ (not EXECUTE_READWRITE)
//      RegionSize = anything != 0x85000
//    This makes CheckThread return -1 so qsec_start_addr stays 0.
// ============================================================================
namespace qsec_scan_spoof
{
    using FnVirtualQueryEx = SIZE_T(WINAPI*)(HANDLE, LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
    static FnVirtualQueryEx orig_VQE = nullptr;

    static uintptr_t g_qsec_base = 0;
    static uintptr_t g_qsec_end  = 0;

    // Called during init — locate the QSec memory region ourselves
    static void locate_qsec()
    {
        // Walk own process memory looking for the "cesQ" signature at PAGE_EXECUTE_READWRITE
        // region of exactly 0x85000 bytes (per decomp: RegionSize == 544768 == 0x85000)
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t addr = 0;
        while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)))
        {
            if (mbi.Protect == PAGE_EXECUTE_READWRITE
             && mbi.RegionSize == 0x85000
             && mbi.State == MEM_COMMIT)
            {
                uint8_t head[4] = {};
                memcpy(head, (void*)mbi.BaseAddress, 4);
                if (head[0]=='c' && head[1]=='e' && head[2]=='s' && head[3]=='Q')
                {
                    g_qsec_base = (uintptr_t)mbi.BaseAddress;
                    g_qsec_end  = g_qsec_base + mbi.RegionSize;
                    break;
                }
            }
            addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (addr == 0) break;
        }
    }

    static SIZE_T WINAPI hook_VirtualQueryEx(
        HANDLE hProcess, LPCVOID lpAddress,
        PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength)
    {
        SIZE_T ret = orig_VQE(hProcess, lpAddress, lpBuffer, dwLength);
        if (ret == 0 || !lpBuffer) return ret;

        uintptr_t base = (uintptr_t)lpBuffer->BaseAddress;

        // If this is the QSec region: mask the size and protection
        if (g_qsec_base && base == g_qsec_base)
        {
            lpBuffer->Protect    = PAGE_EXECUTE_READ;      // not RWX
            lpBuffer->RegionSize = 0x84000;               // != 0x85000
        }
        return ret;
    }

    // Minimal IAT hook on kernel32.VirtualQueryEx
    static bool install()
    {
        locate_qsec();

        uintptr_t base; size_t sz;
        if (!util::get_module("bloodstrike.exe", base, sz)) return false;

        // Walk IAT of bloodstrike.exe looking for VirtualQueryEx
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)
            (base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

        for (; imp->Name; imp++) {
            const char* dll = (const char*)(base + imp->Name);
            if (_stricmp(dll, "kernel32.dll") != 0) continue;

            uintptr_t* thunk = (uintptr_t*)(base + imp->FirstThunk);
            IMAGE_THUNK_DATA* orig_thunk = (IMAGE_THUNK_DATA*)
                (base + imp->OriginalFirstThunk);

            for (; orig_thunk->u1.AddressOfData; orig_thunk++, thunk++) {
                if (orig_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)
                    (base + orig_thunk->u1.AddressOfData);
                if (strcmp((char*)ibn->Name, "VirtualQueryEx") != 0) continue;

                orig_VQE = (FnVirtualQueryEx)*thunk;
                DWORD old = 0;
                VirtualProtect(thunk, sizeof(uintptr_t), PAGE_READWRITE, &old);
                *thunk = (uintptr_t)hook_VirtualQueryEx;
                VirtualProtect(thunk, sizeof(uintptr_t), old, &old);
                return true;
            }
        }
        return false;
    }
}

// ============================================================================
// 2. QSec self-integrity MD5 spoof
//    CheckWindowsQsecsdk calls GetMemoryContent (MMyLighting native function)
//    on the qsec_start..qsec_end range to hash its own code.
//    We find MMyLighting.dll's GetMemoryContent export and inline-hook it
//    to return a zeroed buffer for any address inside our located qsec range.
//    MD5(all-zeros, fixed size) = a known constant, which differs from the
//    real hash — but QSec only reports the MD5 to the server, it doesn't
//    compare it client-side. The report is cosmetic; the ban trigger is the
//    thread scan (step 1). Blocking step 1 is sufficient; this is belt+suspenders.
// ============================================================================
namespace qsec_md5_spoof
{
    // GetMemoryContent(addr, size) returns a bytes object
    // We hook at the native level: find the function, replace first 14 bytes
    // with a jmp-absolute trampoline to our stub.

    // The stub: if addr is in QSec range, fill dst with zeros
    static uint8_t orig_bytes[14] = {};
    static uintptr_t orig_addr    = 0;

    // Trampoline target — called by the hook below
    // Since GetMemoryContent is a Python C extension returning PyObject*,
    // we hook at a lower level: the actual memcpy inside GetMemoryContent
    // via the MMyLighting call table (already located by qsec_scan_spoof::locate_qsec).
    // Simpler approach: patch the MMyLighting.O0O01005lIIl (ReadMem) function
    // which is also used for GetMemoryContent, to NOP out the copy if src
    // is in the QSec range.

    static void install()
    {
        // Find MMyLighting.dll
        HMODULE mml = GetModuleHandleA("MMyLighting.dll");
        if (!mml) return;

        // Locate ReadMem / GetMemoryContent — exported as O0O01005lIIl
        FARPROC fn = GetProcAddress(mml, "O0O01005lIIl");
        if (!fn) return;

        orig_addr = (uintptr_t)fn;
        memcpy(orig_bytes, (void*)orig_addr, 14);

        // Write a jmp [rip+0] trampoline to our hook
        // Our hook will check if the source address is in QSec range
        // and if so zero the output buffer before returning.
        // We need the stub in an executable allocation.

        void* stub = VirtualAlloc(nullptr, 256, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!stub) return;

        uintptr_t qsec_start = qsec_scan_spoof::g_qsec_base + 0x1000;
        uintptr_t qsec_end   = qsec_scan_spoof::g_qsec_base + 0x1000 + 0xFF440;

        // x64 stub: test if rcx (first arg = addr) is in qsec range,
        // if so zero rdx (buf) for r8 (size) bytes then execute original.
        // Layout:
        //   mov  rax, qsec_start
        //   cmp  rcx, rax
        //   jb   not_qsec
        //   mov  rax, qsec_end
        //   cmp  rcx, rax
        //   jae  not_qsec
        //   xor  eax, eax
        //   push rcx; push rdx; push r8; push r9
        //   mov  rcx, rdx      ; buf
        //   mov  rdx, 0        ; fill value
        //   call memset
        //   pop r9; pop r8; pop rdx; pop rcx
        // not_qsec:
        //   jmp [rip+0]
        //   <original address>

        uint8_t* p = (uint8_t*)stub;
        auto write8  = [&](uint8_t v)  { *p++ = v; };
        auto write32 = [&](uint32_t v) { memcpy(p, &v, 4); p += 4; };
        auto write64 = [&](uint64_t v) { memcpy(p, &v, 8); p += 8; };

        // mov rax, qsec_start
        write8(0x48); write8(0xB8); write64(qsec_start);
        // cmp rcx, rax
        write8(0x48); write8(0x39); write8(0xC1);
        // jb not_qsec (+offset, fill after)
        write8(0x72); uint8_t* jb_off1 = p; write8(0);
        // mov rax, qsec_end
        write8(0x48); write8(0xB8); write64(qsec_end);
        // cmp rcx, rax
        write8(0x48); write8(0x39); write8(0xC1);
        // jae not_qsec
        write8(0x73); uint8_t* jb_off2 = p; write8(0);
        // zero the buffer: memset(rdx, 0, r8)
        //   push regs we need
        write8(0x51); // push rcx
        write8(0x52); // push rdx
        write8(0x41); write8(0x50); // push r8
        write8(0x41); write8(0x51); // push r9
        //   mov rcx, rdx (buf)
        write8(0x48); write8(0x89); write8(0xD1);
        //   xor edx, edx (value=0)
        write8(0x31); write8(0xD2);
        //   mov r8 already has size — but we pushed it, use r8 off stack
        //   Actually r8 is still valid (we only pushed, not clobbered it)
        //   call memset via IAT lookup — too complex inline, use a simpler approach:
        //   just rep stosb
        //   mov rdi, rcx ; rcx = buf
        write8(0x48); write8(0x89); write8(0xCF);
        //   xor al, al
        write8(0x30); write8(0xC0);
        //   mov rcx, r8 (size)
        write8(0x4C); write8(0x89); write8(0xC1);
        //   rep stosb
        write8(0xF3); write8(0xAA);
        //   pop regs
        write8(0x41); write8(0x59); // pop r9
        write8(0x41); write8(0x58); // pop r8
        write8(0x5A); // pop rdx
        write8(0x59); // pop rcx

        // not_qsec: patch offsets
        *jb_off1 = (uint8_t)(p - jb_off1 - 1);
        *jb_off2 = (uint8_t)(p - jb_off2 - 1);

        // Copy original bytes then jmp back to orig+14
        memcpy(p, orig_bytes, 14); p += 14;
        // jmp [rip+0]
        write8(0xFF); write8(0x25); write32(0);
        uint64_t ret_addr = orig_addr + 14;
        write64(ret_addr);

        // Install trampoline at orig_addr
        uint8_t tramp[14];
        tramp[0] = 0xFF; tramp[1] = 0x25;
        memset(tramp + 2, 0, 4); // rip+0
        memcpy(tramp + 6, &stub, 8);
        util::patch_bytes(orig_addr, tramp, 14);
    }
}

// ============================================================================
// 3. OnRecoilEnterCheck bypass
//    Reads StateRecoil.OnRecoilEnter.__code__.co_consts looking for 0.01.
//    If we've patched out 0.01 for no-recoil, QSec reports a violation.
//
//    Fix: don't patch co_consts. Instead hook at the movement application
//    layer — intercept the entity's view angle setter rather than touching
//    bytecode. This leaves co_consts intact so the check passes.
//    Documented here as a design constraint: no-recoil MUST be implemented
//    via SetViewAngles hook or SendInput, NOT via bytecode patching.
// ============================================================================
namespace recoil_note
{
    // No code needed — see design constraint above.
    // If a RCS feature patches co_consts: restore 0.01 after the patch.
    // Restoring means: find the PyFloatObject for 0.01 in the tuple and
    // write back 0.01 via direct memory write to ob_fval field (+0x10 on
    // CPython 3.14 x64 PyFloatObject).
    static void restore_recoil_const_if_needed(uintptr_t co_consts_tuple, int idx)
    {
        // co_consts_tuple is a PyTupleObject*
        // ob_item starts at +0x18 (CPython 3.14, x64)
        // Each item is a PyObject* (8 bytes)
        // PyFloatObject.ob_fval is at +0x10
        constexpr ptrdiff_t tuple_ob_item = 0x18;
        constexpr ptrdiff_t float_ob_fval = 0x10;

        uintptr_t item_ptr = *(uintptr_t*)(co_consts_tuple + tuple_ob_item + idx * 8);
        double val = *(double*)(item_ptr + float_ob_fval);
        if (val != 0.01) {
            const double vanilla_recoil = 0.01;
            util::patch_bytes(item_ptr + float_ob_fval,
                              reinterpret_cast<const uint8_t*>(&vanilla_recoil),
                              sizeof(vanilla_recoil));
        }
    }
}

// ============================================================================
// 4. GetTextCRC spoof
//    CRC32 of .text section between two export symbols in bloodstrike.exe.
//    Exports used as anchors:
//      start = _Z25NvClothCreateFabricCookerv (Windows: ??0ClothFabricCooker...)
//      end   = NvClothDestroyFactory
//    If we've patched bytes in that range, the CRC changes.
//
//    Fix: hook zlib.crc32 at the C extension level inside the Python runtime.
//    Python's zlib module is implemented in zlibmodule.c, exported as PyInit_zlib.
//    The crc32 function is PyZlib_crc32 inside the _zlib.pyd extension.
//    We inline-hook it to return a pre-computed expected value when called
//    with a buffer starting at qsec_start.
//
//    Simpler alternative: before any QSec check tick, restore original bytes
//    at patched locations, let QSec read, then re-apply patches.
//    This is the approach implemented here — store original bytes on patch.
// ============================================================================
namespace text_crc_spoof
{
    struct PatchRecord {
        uintptr_t addr;
        std::vector<uint8_t> original;
        std::vector<uint8_t> patched;
    };
    static std::vector<PatchRecord> g_patches;

    // Call before applying any patch to bloodstrike.exe .text
    void record_patch(uintptr_t addr, const uint8_t* new_bytes, size_t len)
    {
        PatchRecord r;
        r.addr = addr;
        r.original.assign((uint8_t*)addr, (uint8_t*)addr + len);
        r.patched.assign(new_bytes, new_bytes + len);
        g_patches.push_back(r);
        util::patch_bytes(addr, new_bytes, len);
    }

    // Restore originals — call from a thread that monitors QSec check timing
    void restore_all()
    {
        for (auto& p : g_patches)
            util::patch_bytes(p.addr, p.original.data(), p.original.size());
    }

    // Re-apply patches
    void reapply_all()
    {
        for (auto& p : g_patches)
            util::patch_bytes(p.addr, p.patched.data(), p.patched.size());
    }
}

// ============================================================================
// 5. Device ban HWID spoof
//    GeneratePhyDriveSerial writes:
//      str(uuid4()) + '-' + str(int(ctime(marker_file))) + '-' + str(int(mtime(marker_file)))
//    to MARK_FILE_PHYSIC_SER = '4051BDD0000f042.pyo'
//    under PATCHROOT and under Common Files\{ApexsCorporation,MicrosoftCorporation}
//
//    Also reads existing marker file — if it contains '\x00', generates a new one.
//
//    Ban fingerprint paths (from decomp constants):
//      MARK_FILE_PHYSIC_SER_LIST:
//        '0d/0d0ab9a4-df96-4d5-9a862-967c2abca6f'
//        '0a/0a0ab8a4-df96-4d5-9a862-967c2adca6f'
//
//    Strategy: intercept WriteFile / CreateFileA for these specific paths,
//    write a spoofed UUID that changes each launch.
// ============================================================================
namespace hwid_spoof
{
    using FnCreateFileA = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using FnWriteFile   = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);

    static FnCreateFileA orig_CFA = nullptr;
    static FnWriteFile   orig_WF  = nullptr;

    static HANDLE g_spoof_handle = INVALID_HANDLE_VALUE;  // handle to spoofed file
    static util::Prng g_rng;

    static bool is_marker_path(LPCSTR path)
    {
        if (!path) return false;
        std::string p(path);
        // Convert to lowercase for comparison
        for (auto& c : p) c = (char)tolower((unsigned char)c);
        return p.find("4051bdd0000f042.pyo") != std::string::npos
            || p.find("0d0ab9a4-df96-4d5") != std::string::npos
            || p.find("0a0ab8a4-df96-4d5") != std::string::npos
            || p.find("4040bdd0ba0f042.pyo") != std::string::npos;
    }

    static HANDLE WINAPI hook_CreateFileA(
        LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
        LPSECURITY_ATTRIBUTES lpSec, DWORD dwCreationDisp,
        DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
    {
        HANDLE h = orig_CFA(lpFileName, dwDesiredAccess, dwShareMode,
                            lpSec, dwCreationDisp, dwFlagsAndAttributes, hTemplateFile);

        if (is_marker_path(lpFileName) && (dwDesiredAccess & GENERIC_WRITE))
            g_spoof_handle = h;

        return h;
    }

    static BOOL WINAPI hook_WriteFile(
        HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
        LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
    {
        if (hFile == g_spoof_handle && hFile != INVALID_HANDLE_VALUE)
        {
            // Generate a fresh spoofed UUID serial
            std::string fake_uuid = g_rng.uuid4();
            // Append fake ctime/mtime (deterministic per session, random between sessions)
            uint64_t fake_ts = g_rng.next() % 1000000000ULL + 1600000000ULL;
            fake_uuid += "-" + std::to_string(fake_ts)
                       + "-" + std::to_string(fake_ts + (g_rng.next() % 3600));

            DWORD written = 0;
            BOOL r = orig_WF(hFile, fake_uuid.c_str(), (DWORD)fake_uuid.size(),
                             &written, lpOverlapped);
            if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = nNumberOfBytesToWrite;
            g_spoof_handle = INVALID_HANDLE_VALUE;
            return r;
        }
        return orig_WF(hFile, lpBuffer, nNumberOfBytesToWrite,
                       lpNumberOfBytesWritten, lpOverlapped);
    }

    static void install()
    {
        uintptr_t base; size_t sz;
        if (!util::get_module("bloodstrike.exe", base, sz)) return;

        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)
            (base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

        for (; imp->Name; imp++) {
            const char* dll = (const char*)(base + imp->Name);
            bool is_k32 = _stricmp(dll, "kernel32.dll") == 0;
            bool is_kbs = _stricmp(dll, "kernelbase.dll") == 0;
            if (!is_k32 && !is_kbs) continue;

            uintptr_t* thunk = (uintptr_t*)(base + imp->FirstThunk);
            IMAGE_THUNK_DATA* orig_thunk = (IMAGE_THUNK_DATA*)
                (base + imp->OriginalFirstThunk);

            for (; orig_thunk->u1.AddressOfData; orig_thunk++, thunk++) {
                if (orig_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)
                    (base + orig_thunk->u1.AddressOfData);

                DWORD old = 0;
                if (!orig_CFA && strcmp((char*)ibn->Name, "CreateFileA") == 0) {
                    orig_CFA = (FnCreateFileA)*thunk;
                    VirtualProtect(thunk, 8, PAGE_READWRITE, &old);
                    *thunk = (uintptr_t)hook_CreateFileA;
                    VirtualProtect(thunk, 8, old, &old);
                }
                if (!orig_WF && strcmp((char*)ibn->Name, "WriteFile") == 0) {
                    orig_WF = (FnWriteFile)*thunk;
                    VirtualProtect(thunk, 8, PAGE_READWRITE, &old);
                    *thunk = (uintptr_t)hook_WriteFile;
                    VirtualProtect(thunk, 8, old, &old);
                }
            }
        }
    }
}

// ============================================================================
// 6. QSec region memory protection patch
//    Even with the thread scan blocked, QSec's region sits in memory as RWX.
//    Other AC checks (not in this decomp but common to QSec builds) look for
//    any RWX region not backed by a known module (VAD scan equivalent).
//    Change the region's protection to PAGE_EXECUTE_READ after QSec loads.
//    QSec's own integrity check hashes the region, so we hash it first,
//    store the expected value, then re-protect. If QSec tries to write itself
//    it will fault — so we need to hook ZwProtectVirtualMemory to allow
//    QSec to temporarily re-protect its own range.
// ============================================================================
namespace qsec_protect
{
    static void harden_region()
    {
        if (!qsec_scan_spoof::g_qsec_base) return;
        DWORD old = 0;
        VirtualProtect((LPVOID)qsec_scan_spoof::g_qsec_base,
                       0x85000, PAGE_EXECUTE_READ, &old);
    }
}

// ============================================================================
// Timing thread — restore/reapply patches around QSec check window
//    QSec.Check() is called from PlayerAvatarMember (gamesystem entity tick).
//    It calls CheckWindowsQsecsdk which reads qsec_start..qsec_end.
//    Typical tick interval: ~10s (configurable server-side).
//    We cannot know exactly when, so the restore/reapply cycle runs on a
//    short timer: restore for 50ms every 8 seconds, reapply after.
// ============================================================================
namespace timing
{
    static HANDLE g_thread = nullptr;
    static volatile bool g_stop = false;

    static DWORD WINAPI thread_proc(LPVOID)
    {
        while (!g_stop)
        {
            Sleep(8000);
            if (g_stop) break;
            text_crc_spoof::restore_all();
            Sleep(50);
            text_crc_spoof::reapply_all();
        }
        return 0;
    }

    static void start()
    {
        g_thread = CreateThread(nullptr, 0, thread_proc, nullptr, 0, nullptr);
        if (g_thread)
            SetThreadPriority(g_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    }

    static void stop()
    {
        g_stop = true;
        if (g_thread)
        {
            // Do not leave the worker executing module code after shutdown.
            WaitForSingleObject(g_thread, INFINITE);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
    }
}

// ============================================================================
// Public init — call once from DllMain(DLL_PROCESS_ATTACH) or injected thread
// ============================================================================
extern "C" void bypass_init()
{
    // Fail closed after the NEW SDK/DECOMP comparison. None of the legacy
    // routines above has a verified current-build call path:
    //   * QSec's VirtualQueryEx/ZwQueryInformationThread calls are dispatched
    //     by MMyLighting.CallFunction, not a proven bloodstrike.exe IAT call.
    //   * CalculateMD5 uses the dispatcher's memcpy path, not the assumed
    //     ReadMem export hook.
    //   * HWID marker writes are Python file operations, so the native
    //     CreateFileA/WriteFile hooks are not authoritative coverage.
    //   * The CRC restore list is not connected to Comet's inline-hook manager,
    //     and the inspected GetTextCRC routine is Android-specific.
    // Do not install guessed AC patches or claim that initialization is a
    // bypass. A future implementation must hook the verified dispatcher or
    // Python owner with exact signatures and restoration before it is enabled.
    OutputDebugStringA(
        "Comet: NEW SDK AC audit refused unverified legacy internal bypass hooks; "
        "no AC patch installed.\n");
}

extern "C" void bypass_shutdown()
{
    timing::stop();
    text_crc_spoof::restore_all();
}
