#ifndef WIN32_LEAN_AND_MEAN

#define WIN32_LEAN_AND_MEAN

#endif

#ifndef NOMINMAX

#define NOMINMAX

#endif

#include <windows.h>

#include <d3d11.h>

#include <dxgi.h>

#include <dxgi1_2.h>

#include <cstdint>

#include <cstddef>

#include <cstdio>

#include <cstdarg>

#include <cfloat>

#include <cstring>

#include <cmath>

#include <string>

#include <vector>

#include <unordered_map>

#include <unordered_set>

#include <algorithm>

#include "imgui.h"
#include "menu_theme.h"
// capture_overlay / stream-proof removed.

#include "imgui_impl_dx11.h"

#include "imgui_impl_win32.h"

#include "MinHook.h"



#include "bs_cfg.hpp"
#include "esp_geometry.hpp"
#include "radar_geometry.hpp"
#include "esp_team.hpp"
#include "esp_armor_tier.hpp"
#include "capture_guard.hpp"




#pragma pack(push, 1)
// Cfg struct is declared in bs_cfg.hpp (shared with Menu34).



struct Ent {

    uint64_t id;

    char name[40];

    char gun[32];

    float head[3];

    float feet[3];

    float head_screen[2];

    float feet_screen[2];

    float distance;

    float hp;

    float max_hp;

    float armor;

    float max_armor;

    int32_t team;

    uint8_t visible;

    uint8_t is_team;

    uint8_t is_monster;

    uint8_t alive;

    uint32_t is_knocked;

    float bones[16][2]; // 16 bones in normalized screen coords [0..1]

    // Comet additions. Appended at the tail so every earlier offset in the
    // serialized record - and therefore every existing reader - is unchanged.
    uint32_t hero_id;   // replicated hero_id, 0 when unknown
    uint32_t is_dying;  // runtime DYING/downed state, published for diagnostics;
                        // the KNOCKED label still uses is_knocked above.

};

// Ground loot published after the player array. Owner type is an exact
// allowlist in the runtime (CombatItemEntity / Supply); no item name is
// invented, so `kind` only distinguishes the two captured container types.
struct LootEnt {
    uint64_t id;
    float    screen[2];   // normalized [0..1]
    float    distance;
    uint32_t kind;        // 0 = dropped item, 1 = supply container
};



struct State {

    uint32_t magic;

    uint32_t version;

    uint32_t seq;

    uint32_t count;

    uint64_t tick_ms;

    float screen_w;

    float screen_h;

    float local_pos[3];

    float cam_pos[3];

    float cam_forward[3];

    float yaw;

    float pitch;

    float fov;

    uint8_t in_match;

    uint8_t local_valid;

    uint16_t local_hero_id;

};



struct CatHead {

    uint32_t magic;

    uint32_t version;

    uint32_t hero_count;

    uint32_t gun_count;

};



struct CatEntry {

    uint32_t a;

    uint32_t b;

    uint32_t c;

    uint32_t d;

    char name[48];

};

#pragma pack(pop)



static_assert(sizeof(Cfg) == 228, "Cfg size");

// The migrations in LoadCfg() hard-code these boundaries. Pin them so a future
// layout edit cannot silently corrupt an old config instead of failing the
// build.
static_assert(offsetof(Cfg, col_team_box) == 148, "v7 legacy prefix changed");
static_assert(offsetof(Cfg, aim_key2) == 224, "v8 layout changed");

static_assert(sizeof(Ent) == 288, "Ent size");

static_assert(sizeof(LootEnt) == 24, "LootEnt size");

static_assert(sizeof(State) == 84, "State size");

static_assert(sizeof(CatHead) == 16, "CatHead size");

static_assert(sizeof(CatEntry) == 64, "CatEntry size");



static const uint32_t CFG_MAGIC = 0x42534346;

static const uint32_t STATE_MAGIC = 0x42535331;

static const uint32_t CATALOG_MAGIC = 0x42534354;

static const uint32_t VERSION = 9;



static bool ReadOverlayRootHint(std::wstring& root) {

    wchar_t tmp[MAX_PATH]{};

    if (!GetTempPathW(MAX_PATH, tmp)) return false;

    std::wstring path = std::wstring(tmp) + L"Comet_BSLoader\\root.txt";

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD size = GetFileSize(h, nullptr);

    if (size < 2 || size > 4096) {

        CloseHandle(h);

        return false;

    }

    std::wstring buf(size / sizeof(wchar_t), L'\0');

    DWORD read = 0;

    BOOL ok = ReadFile(h, &buf[0], size, &read, nullptr);

    CloseHandle(h);

    if (!ok || read < 2) return false;

    buf.resize(read / sizeof(wchar_t));

    if (GetFileAttributesW((buf + L"\\ipc").c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    root = buf;

    return true;

}


static const std::wstring& PathBase() {

    static std::wstring base;

    if (base.empty()) {

        HMODULE self = nullptr;

        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,

                           reinterpret_cast<LPCWSTR>(&PathBase), &self);

        wchar_t dll[MAX_PATH]{};

        DWORD n = GetModuleFileNameW(self, dll, MAX_PATH);

        std::wstring dir(dll, n ? n : 0);

        size_t p = dir.find_last_of(L"\\/");

        if (p != std::wstring::npos) dir.resize(p);

        std::wstring local = dir + L"\\ipc";

        std::wstring hint;

        if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) base = dir;

        else if (ReadOverlayRootHint(hint)) base = hint;

        else base = dir + L"\\..\\..";

    }

    return base;

}



static const std::wstring& IpcDir() {

    static std::wstring p;

    if (p.empty()) {

        p = PathBase() + L"\\ipc";

        CreateDirectoryW(p.c_str(), nullptr);

    }

    return p;

}



static const std::wstring& CfgPath() { static std::wstring p = IpcDir() + L"\\cfg.bin"; return p; }

static const std::wstring& SavedCfgPath() {
    static const std::wstring path = [] {
        wchar_t local[32768]{};
        const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (!size || size >= 32768) return IpcDir() + L"\\settings.bin";
        std::wstring dir = std::wstring(local) + L"\\Comet";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\BloodStrike";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\config.bin";
    }();
    return path;
}

static const std::wstring& StatePath() { static std::wstring p = IpcDir() + L"\\state.bin"; return p; }

static const std::wstring& CatalogPath() { static std::wstring p = IpcDir() + L"\\catalog.bin"; return p; }

static const std::wstring& LogPath() { static std::wstring p = PathBase() + L"\\overlay_log.txt"; return p; }



// F_* flag enums are declared in bs_cfg.hpp.



static const ImU32 ACCENT      = IM_COL32(164, 143, 246, 255);

static const ImU32 ACCENT_SOFT = IM_COL32(164, 143, 246, 120);

static const ImU32 COL_VIS     = IM_COL32(96, 235, 155, 240);

static const ImU32 COL_HID     = IM_COL32(240, 90, 90, 235);

static const ImU32 COL_TEAM    = IM_COL32(90, 170, 255, 220);

static const ImU32 COL_MON     = IM_COL32(255, 200, 80, 235);

static const ImVec4 ACCENT4    = ImVec4(.643f, .561f, .965f, 1.f);

static const ImVec4 DIM4       = ImVec4(0.62f, 0.63f, 0.67f, 1.0f);



using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);



static HMODULE g_hmodule = nullptr;

static bool g_unloading = false;

static PresentFn g_present = nullptr;

static Present1Fn g_present1 = nullptr;
static bool g_imgui_ready = false;
static thread_local bool g_inside_present = false;
struct PresentScope {
    PresentScope() {g_inside_present=true;}
    ~PresentScope() {g_inside_present=false;}
};

static ResizeBuffersFn g_resize = nullptr;

static WNDPROC g_wndproc = nullptr;

static HWND g_hwnd = nullptr;
static bool g_input_attached=false;
static bool g_menu_owns_capture=false;
static UINT MenuInputCloseMessage() {
    static const UINT message=RegisterWindowMessageW(L"Comet.MenuInput.Close");
    return message;
}

#ifndef WDA_EXCLUDEFROMCAPTURE

#define WDA_EXCLUDEFROMCAPTURE 0x00000011

#endif

#ifndef WDA_NONE

#define WDA_NONE 0x00000000

#endif



static IDXGISwapChain* g_swapchain = nullptr;

static ID3D11Device* g_device = nullptr;

static ID3D11DeviceContext* g_context = nullptr;

static ID3D11RenderTargetView* g_rtv = nullptr;

static UINT g_width = 0;

static UINT g_height = 0;

static UINT g_client_w = 0;

static UINT g_client_h = 0;

static bool g_display_dirty = false;

static bool g_reset_menu_pos = false;



static bool g_init_failed = false;



static HANDLE g_shm_state_h = nullptr;

static void*  g_shm_state_p = nullptr;

static HANDLE g_shm_cfg_h   = nullptr;

static void*  g_shm_cfg_p   = nullptr;



static void InitSharedMemory() {

    if (!g_shm_state_h) {

        g_shm_state_h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 65536, "Local\\BS_IPC_STATE");

        if (g_shm_state_h) {

            g_shm_state_p = MapViewOfFile(g_shm_state_h, FILE_MAP_ALL_ACCESS, 0, 0, 65536);

        }

    }

    if (!g_shm_cfg_h) {

        g_shm_cfg_h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Cfg), "Local\\BS_IPC_CFG");

        if (g_shm_cfg_h) {

            g_shm_cfg_p = MapViewOfFile(g_shm_cfg_h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Cfg));

        }

    }

}





Cfg g_cfg{};

static bool g_cfg_ready = false;
static bool g_cfg_writable = true;
static std::string g_cfg_error;

bool g_cfg_dirty = false;

static ULONGLONG g_last_state_poll = 0;

static ULONGLONG g_last_cfg_save = 0;

static State g_state{};

static std::vector<Ent> g_ents;

static std::vector<LootEnt> g_loot;

static teamtint::Labels g_team_labels;

static comet_capture::Blackout g_obs{};

static FILETIME g_state_write{};

static ULONGLONG g_state_size = 0;

static bool g_state_seen = false;

static std::vector<CatEntry> g_heroes;

static std::vector<CatEntry> g_guns;

static std::vector<uint32_t> g_hero_ids;

static std::vector<uint32_t> g_gun_ids;

static std::vector<uint32_t> g_knife_ids;

static std::vector<uint32_t> g_weapon_ids;

static std::unordered_map<uint32_t, std::vector<int>> g_hero_skins;

static std::unordered_map<uint32_t, std::vector<int>> g_gun_skins;

static std::unordered_map<uint32_t, std::string> g_gun_label;

static std::unordered_map<uint32_t, uint32_t> g_gun_kind;

static std::unordered_map<uint32_t, uint32_t> g_gun_default;

static bool g_catalog_ready = false;

static std::vector<uint8_t> g_buffer;

static ImFont* g_font_big = nullptr;
static ImFont* g_font_esp = nullptr;



static int g_bind_slot = 0;

static bool g_bind_armed = false;

static int g_hero_sel = 0;

static int g_knife_sel = 0;

static int g_weapon_sel = 0;

static char g_search[64] = {};

static int g_tab = 0;



// Has/SetF are declared inline in bs_cfg.hpp.



static void Log(const char* fmt, ...) {

    char msg[1024];

    va_list ap;

    va_start(ap, fmt);

    vsnprintf(msg, sizeof(msg), fmt, ap);

    va_end(ap);

    OutputDebugStringA(msg);

    OutputDebugStringA("\n");

    FILE* f = nullptr;

    if (_wfopen_s(&f, LogPath().c_str(), L"a") == 0 && f) {

        SYSTEMTIME st{};

        GetLocalTime(&st);

        fprintf(f, "%02u:%02u:%02u %s\n", st.wHour, st.wMinute, st.wSecond, msg);

        fclose(f);

    }

}

// ---------------------------------------------------------------------------
// Fault containment.
//
// Every hook body runs on the game's render thread, inside the game's own
// Present call. An exception that escapes here is not "an overlay bug" - it is
// an unhandled exception inside the game process, which terminates it. So each
// hook body is wrapped in SEH: the frame is skipped, the fault is logged, and
// after a few faults the overlay disables itself and passes Present straight
// through. A broken overlay should cost the ESP, never the game.
static int g_fault_count = 0;
static volatile bool g_render_disabled = false;

static void NoteFault(const char* where, unsigned code) {
    if (g_fault_count < 5) Log("%s fault code=%08x (fault %d)", where, code, g_fault_count + 1);
    if (++g_fault_count >= 4) {
        if (!g_render_disabled) Log("%s: repeated faults, overlay drawing disabled", where);
        g_render_disabled = true;
    }
}

// SEH on MSVC; off Windows the same bodies are type-checked with no-op macros,
// so the g++ syntax pass and the menu preview keep working and the non-Windows
// build never swallows an exception it does not understand.
#if defined(_MSC_VER)
  #define COMET_TRY __try
  #define COMET_EXCEPT(filter) __except (filter)
#else
  #define COMET_TRY {
  #define COMET_EXCEPT(filter) }
  #define GetExceptionCode() 0u
#endif


// Defined much later, next to the rest of the render path; declared here so the
// guarded wrapper below can call them.
static bool InitOverlay(IDXGISwapChain* sc);
static void RenderFrame(IDXGISwapChain* sc);

// Note on the wrappers below: MSVC refuses __try/__except in any function that
// needs C++ object unwinding (error C2712), which rules out RAII locals and
// std::string in the same body. These two wrappers therefore keep *no*
// destructible locals - just the call and the exception code - and every caller
// keeps its own RAII scope and does its logging outside.
static unsigned RenderFrameGuarded(IDXGISwapChain* sc) {
    unsigned code = 0;
    COMET_TRY {
        if (!g_imgui_ready) InitOverlay(sc);
        RenderFrame(sc);
    }
    COMET_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        code = (unsigned)GetExceptionCode();
    }
    return code;
}






static bool ReadAll(const wchar_t* path, std::vector<uint8_t>& out) {

    HANDLE h = CreateFileW(path, GENERIC_READ,

                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,

                           nullptr, OPEN_EXISTING,

                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};

    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {

        CloseHandle(h);

        return false;

    }

    out.resize(static_cast<size_t>(size.QuadPart));

    DWORD got = 0;

    BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);

    CloseHandle(h);

    if (!ok) {

        out.clear();

        return false;

    }

    out.resize(got);

    return true;

}



static bool StateFileChanged() {

    WIN32_FILE_ATTRIBUTE_DATA fad{};

    if (!GetFileAttributesExW(StatePath().c_str(), GetFileExInfoStandard, &fad)) return false;

    ULONGLONG size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

    if (g_state_seen && size == g_state_size &&

        fad.ftLastWriteTime.dwLowDateTime == g_state_write.dwLowDateTime &&

        fad.ftLastWriteTime.dwHighDateTime == g_state_write.dwHighDateTime) {

        return false;

    }

    g_state_seen = true;

    g_state_size = size;

    g_state_write = fad.ftLastWriteTime;

    return true;

}



static bool WriteAll(const wchar_t* path, const void* data, size_t size) {

    std::wstring tmp = std::wstring(path) + L".tmp";

    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,

                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);

    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD wrote = 0;

    BOOL ok = WriteFile(h, data, static_cast<DWORD>(size), &wrote, nullptr);

    CloseHandle(h);

    if (!ok || wrote != size) {

        DeleteFileW(tmp.c_str());

        return false;

    }

    if (!MoveFileExW(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {

        DeleteFileW(tmp.c_str());

        return false;

    }

    return true;

}



// Derived from the tier palette in esp_armor_tier.hpp (the anchors 50/75/100/125
// with 245 alpha), so the defaults and the tier maths share one definition.
static constexpr uint32_t kDefaultArmorTier[4] = {
    (armortier::Packed(50.0f) & 0x00FFFFFFu) | (245u << 24),
    (armortier::Packed(75.0f) & 0x00FFFFFFu) | (245u << 24),
    (armortier::Packed(100.0f) & 0x00FFFFFFu) | (245u << 24),
    (armortier::Packed(125.0f) & 0x00FFFFFFu) | (245u << 24),
};
static const uint32_t kDefaultLoot = (245u << 24) | (255u << 16) | (230u << 8) | 51u;

static void DefaultCfg() {

    ZeroMemory(&g_cfg, sizeof(g_cfg));

    g_cfg.magic = CFG_MAGIC;

    g_cfg.version = VERSION;

    g_cfg.flags = F_MENU_OPEN | F_ESP | F_ESP_BOX | F_ESP_SKELETON | F_ESP_NAME | F_ESP_DISTANCE |

                 F_ESP_HEALTH | F_ESP_ARMOR | F_ESP_HEADDOT | F_AIM_FOV_CIRCLE | F_AIM_PREDICT |

                 F_UNLOCK_EFFECTS | F_BLOCK_REPORT |

                 F_RECOIL | F_ESP_COUNT | F_ESP_OUTLINE | F_ESP_GRADIENT | F_ESP_SHADOW;

    g_cfg.aim_key = VK_RBUTTON;

    g_cfg.aim_key2 = 0; // unbound by default

    g_cfg.col_fov = IM_COL32(89,162,255,180);

    g_cfg.menu_key = VK_INSERT;

    g_cfg.aim_fov = 8.0f;

    g_cfg.aim_smooth = 0.35f;

    g_cfg.aim_max_dist = 400.0f;

    g_cfg.human_smooth = 0.5f;
    g_cfg.pad[0] = CFG_UI_TAG;
    g_cfg.pad[1] = 2;

    g_cfg.humanize = 0.0f;  // 0 = pure memory damping (pyexec); raise to add mouse counter-drag.

    g_cfg.aim_bone = 0;
    g_cfg.reserved = OPT_AIM_BONES_SET | OPT_AIM_HEAD | OPT_FOV_COLOR_SET;

    g_cfg.esp_max_dist = 400;

    g_cfg.col_box = IM_COL32(96, 235, 155, 240);

    g_cfg.col_box_hidden = IM_COL32(240, 90, 90, 235);

    g_cfg.col_team = IM_COL32(90, 170, 255, 220);

    g_cfg.col_monster = IM_COL32(255, 200, 80, 235);

    g_cfg.col_skel = IM_COL32(96, 235, 155, 235);

    g_cfg.col_line = IM_COL32(89, 113, 162, 190);

    g_cfg.col_name = IM_COL32(236, 237, 241, 255);

    g_cfg.col_gun = IM_COL32(214, 216, 222, 235);

    g_cfg.col_dist = IM_COL32(200, 205, 215, 240);

    g_cfg.col_health = IM_COL32(96, 235, 155, 240);

    g_cfg.col_armor = IM_COL32(80, 180, 255, 240);

    g_cfg.col_headdot = IM_COL32(255, 255, 255, 235);

    g_cfg.col_count = IM_COL32(96, 235, 155, 255);

    g_cfg.esp_style = 0;

    g_cfg.box_thickness = 1.5f;

    g_cfg.skin_hero_item = 0;

    g_cfg.skin_gun_item = 0;

    g_cfg.skin_knife_item = 0;

    g_cfg.auto_skin = 1;

    // Team box / line (slots 1-4), armor tiers (50 grey, 75 blue, 100 purple,
    // 125 gold) and the loot marker. Shared with the v7 -> v8 migration above.
    for (int i = 0; i < 4; ++i) {
        g_cfg.col_armor_tier[i] = kDefaultArmorTier[i];
    }

    g_cfg.col_loot = kDefaultLoot;

    g_cfg.opts = OPT2_OBS_PROTECT;

    g_cfg.loot_max_dist = 120.0f;

    g_cfg.noise_strength = 0.0016f;

}



static bool LoadCfg() {
    std::vector<uint8_t> buf;
    g_cfg_error.clear();
    const DWORD attributes = GetFileAttributesW(SavedCfgPath().c_str());
    const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    const bool missing = attributes == INVALID_FILE_ATTRIBUTES && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
    const bool read = ReadAll((missing ? CfgPath() : SavedCfgPath()).c_str(), buf);
    // v7 -> v8 migration. The Comet tail was appended after col_armor, so the
    // first 148 bytes of a v7 file are byte-identical to the new layout and the
    // old pad[3] simply moves to the end. Without this, growing Cfg would throw
    // away every saved preference once - including the user's keybinds and
    // colours - which is not an acceptable cost for adding features.
    bool migrated = false;
    if (read && buf.size() == 224) {
        // v8 -> v9: only aim_key2 was appended. Everything else is identical.
        uint32_t old_magic = 0, old_version = 0;
        memcpy(&old_magic, buf.data(), sizeof(old_magic));
        memcpy(&old_version, buf.data() + sizeof(old_magic), sizeof(old_version));
        if (old_magic == CFG_MAGIC && old_version == 8) {
            Cfg m{};
            memcpy(&m, buf.data(), 224);
            m.version = VERSION;
            buf.assign(reinterpret_cast<const uint8_t*>(&m),
                       reinterpret_cast<const uint8_t*>(&m) + sizeof(m));
            migrated = true;
        }
    }
    if (read && buf.size() == 160) {
        uint32_t old_magic = 0, old_version = 0;
        memcpy(&old_magic, buf.data(), sizeof(old_magic));
        memcpy(&old_version, buf.data() + sizeof(old_magic), sizeof(old_version));
        if (old_magic == CFG_MAGIC && old_version == 7) {
            constexpr size_t kV7LegacyBytes = 148; // everything up to col_armor
            Cfg m{};
            memcpy(&m, buf.data(), kV7LegacyBytes);
            // v7's trailing pad[3] (UI tag / theme / reduced motion) is at 148.
            memcpy(m.pad, buf.data() + kV7LegacyBytes, sizeof(m.pad));
            // The new tail keeps the same defaults a fresh install would get, so
            // a migrated config cannot end up with colours switched off.
            for (int i = 0; i < 4; ++i) {
                m.col_armor_tier[i] = kDefaultArmorTier[i];
            }
            m.col_loot = kDefaultLoot;
            m.opts = OPT2_OBS_PROTECT;
            m.loot_max_dist = 120.0f;
            m.noise_strength = 0.0016f;
            m.version = VERSION;
            buf.assign(reinterpret_cast<const uint8_t*>(&m),
                       reinterpret_cast<const uint8_t*>(&m) + sizeof(m));
            migrated = true;
        }
    }
    if (!read || buf.size() != sizeof(Cfg)) {
        DefaultCfg();
        g_cfg_ready = g_cfg_dirty = true;
        g_cfg_writable = missing;
        if (!missing) g_cfg_error = "Config read failed. Save to replace.";
        return false;
    }
    Cfg c{};
    memcpy(&c, buf.data(), sizeof(c));
    if (c.magic != CFG_MAGIC || c.version != VERSION) {
        DefaultCfg();
        g_cfg_ready = g_cfg_dirty = true;
        g_cfg_writable = missing;
        if (!missing) g_cfg_error = "Invalid config. Save to replace.";
        return false;
    }
    if (!std::isfinite(c.aim_fov) || c.aim_fov < 1.0f || c.aim_fov > 180.0f) c.aim_fov = 8.0f;

    if (!std::isfinite(c.aim_smooth) || c.aim_smooth < 0.01f || c.aim_smooth > 1.0f) c.aim_smooth = 0.35f;

    if (!std::isfinite(c.aim_max_dist) || c.aim_max_dist < 10.0f || c.aim_max_dist > 2000.0f) c.aim_max_dist = 400.0f;

    if (!std::isfinite(c.humanize) || c.humanize < 0.0f || c.humanize > 2.0f) c.humanize = 0.0f;

    if (c.esp_max_dist < 10 || c.esp_max_dist > 2000) c.esp_max_dist = 400;

    if (!std::isfinite(c.box_thickness) || c.box_thickness < 0.5f || c.box_thickness > 5.0f) c.box_thickness = 1.5f;

    if (c.col_box == 0) c.col_box = IM_COL32(96, 235, 155, 240);

    if (c.col_box_hidden == 0) c.col_box_hidden = IM_COL32(240, 90, 90, 235);

    if (c.col_team == 0) c.col_team = IM_COL32(90, 170, 255, 220);

    if (c.col_monster == 0) c.col_monster = IM_COL32(255, 200, 80, 235);

    if (c.col_skel == 0) c.col_skel = IM_COL32(96, 235, 155, 235);

    if (c.col_line == 0) c.col_line = IM_COL32(89, 113, 162, 190);

    if (c.col_name == 0) c.col_name = IM_COL32(236, 237, 241, 255);

    if (c.col_gun == 0) c.col_gun = IM_COL32(214, 216, 222, 235);

    if (c.col_dist == 0) c.col_dist = IM_COL32(200, 205, 215, 240);

    if (c.col_health == 0) c.col_health = IM_COL32(96, 235, 155, 240);

    if (c.col_armor == 0) c.col_armor = IM_COL32(80, 180, 255, 240);

    if (c.col_headdot == 0) c.col_headdot = IM_COL32(255, 255, 255, 235);

    if (c.col_count == 0) c.col_count = IM_COL32(96, 235, 155, 255);

    // Only the armor tier palette is user data now; team tint colours are
    // generated per team at runtime (src/esp_team.hpp).
    for (int i = 0; i < 4; ++i) {
        if (c.col_armor_tier[i] == 0) c.col_armor_tier[i] = kDefaultArmorTier[i];
    }
    if (c.col_loot == 0) c.col_loot = IM_COL32(51, 230, 255, 245);
    if (!std::isfinite(c.loot_max_dist) || c.loot_max_dist < 10.0f || c.loot_max_dist > 500.0f) c.loot_max_dist = 120.0f;
    if (!std::isfinite(c.noise_strength) || c.noise_strength < 0.0f || c.noise_strength > 0.02f) c.noise_strength = 0.0016f;

    if (c.aim_bone < 0 || c.aim_bone > 4) c.aim_bone = 0;

    if (c.aim_key < 0 || c.aim_key > 255) c.aim_key = VK_RBUTTON;

    // 0 means "no second key", which is a valid state (unlike aim_key).
    if (c.aim_key2 < 0 || c.aim_key2 > 255) c.aim_key2 = 0;

    if (c.menu_key < 0 || c.menu_key > 255) c.menu_key = VK_INSERT;

    if (!(c.reserved & OPT_AIM_BONES_SET)) {
        uint32_t bones = (c.flags & F_AIM_MULTIPOINT) || c.aim_bone == 4 ? OPT_AIM_BONE_MASK :
                         c.aim_bone == 0 ? OPT_AIM_HEAD : c.aim_bone == 1 ? OPT_AIM_NECK : OPT_AIM_BODY;
        c.reserved = (c.reserved & ~OPT_AIM_BONE_MASK) | OPT_AIM_BONES_SET | bones;
    }
    if (!(c.reserved & OPT_AIM_BONE_MASK)) c.reserved |= OPT_AIM_HEAD;
    if(!(c.reserved&OPT_FOV_COLOR_SET)) {
        c.col_fov=IM_COL32(89,162,255,180);c.reserved|=OPT_FOV_COLOR_SET;
    }
    c.flags &= ~F_AIM_MULTIPOINT;
    if (!std::isfinite(c.human_smooth) || c.human_smooth < 0.01f || c.human_smooth > 1.f) c.human_smooth = 0.5f;
    if (c.pad[0] != CFG_UI_TAG || c.pad[1] > 4) {
        c.pad[0] = CFG_UI_TAG; c.pad[1] = 2; c.pad[2] = 0;
    }
    c.pad[2] = c.pad[2] ? 1u : 0u;
    // Session commands must never be replayed when loading persistent preferences.
    c.flags = (c.flags & ~F_EXIT_CHEAT) | F_MENU_OPEN;
    c.apply_seq = c.apply_item = 0; c.apply_type = 0;
    g_cfg = c;
    // Loaded configs stay dirty so the next autosave rewrites them in the
    // current layout; that is also what flushes a v7 -> v8 migration to disk.
    g_cfg_ready = g_cfg_dirty = g_cfg_writable = true;
    if (migrated) Log("config migrated from layout v7 (settings preserved)");
    return true;
}

static bool SaveCfg() {
    if (!g_cfg_ready) return false;
    g_cfg.magic = CFG_MAGIC; g_cfg.version = VERSION;
    if (++g_cfg.seq == 0) ++g_cfg.seq;
    if (g_shm_cfg_p) memcpy(g_shm_cfg_p, &g_cfg, sizeof(g_cfg));
    const bool published = WriteAll(CfgPath().c_str(), &g_cfg, sizeof(g_cfg));
    if (!g_cfg_writable) return false;
    Cfg saved = g_cfg;
    saved.flags &= ~(F_MENU_OPEN | F_EXIT_CHEAT);
    saved.apply_seq = saved.apply_item = 0; saved.apply_type = 0;
    if (!WriteAll(SavedCfgPath().c_str(), &saved, sizeof(saved))) {
        g_cfg_error = "Config save failed. Check permissions.";
        return false;
    }
    if (!published) {
        g_cfg_error = "Saved; runtime update pending.";
        return false;
    }
    g_cfg_dirty = false;
    g_cfg_error.clear();
    return true;
}

bool SaveConfiguration() {
    g_cfg_dirty = true;
    g_cfg_writable = true;
    return SaveCfg();
}

const char* ConfigurationStatus() {
    if (!g_cfg_error.empty()) return g_cfg_error.c_str();
    return g_cfg_dirty ? "Pending save..." : "Saved automatically";
}

static void ApplyMenuPreferences() {
    const bool valid = g_cfg.pad[0] == CFG_UI_TAG && g_cfg.pad[1] <= 4;
    CometUi::SetTheme(valid ? g_cfg.pad[1] : 2);
    CometUi::ReducedMotion = valid && g_cfg.pad[2] != 0;
    CometUi::ApplyStyle();
}

static void ResetCfg() {

    DefaultCfg();

    g_cfg_ready = true;

    g_cfg_dirty = true;

    ApplyMenuPreferences();
    SaveConfiguration();

}



enum : uint32_t {

    OPT_AIM_TOGGLE = 1u << 0,

};



static bool OptAimToggle() {

    return (g_cfg.reserved & OPT_AIM_TOGGLE) != 0;

}



static void SetOptAimToggle(bool on) {

    if (on) g_cfg.reserved |= OPT_AIM_TOGGLE;

    else g_cfg.reserved &= ~OPT_AIM_TOGGLE;

    g_cfg_dirty = true;

}







// Ground loot trailer: [u32 count][LootEnt * count] immediately after the
// player array. A record that does not fit is dropped rather than read past the
// mapped buffer, so a stale/shorter packet can never produce a phantom marker.
static void ParseLoot(const uint8_t* base, size_t avail, uint32_t ents) {
    g_loot.clear();
    const size_t offset = sizeof(State) + (size_t)ents * sizeof(Ent);
    if (!base || avail < offset + sizeof(uint32_t)) return;
    uint32_t count = 0;
    memcpy(&count, base + offset, sizeof(count));
    const size_t room = (avail - offset - sizeof(uint32_t)) / sizeof(LootEnt);
    if (count > room) count = (uint32_t)room;
    if (count > 128) count = 128;
    if (!count) return;
    g_loot.resize(count);
    memcpy(g_loot.data(), base + offset + sizeof(uint32_t), (size_t)count * sizeof(LootEnt));
}

// Keeps SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) in force while the
// Settings toggle is on. The guard re-asserts the affinity if the game clears
// it and backs off after an API failure instead of retrying every frame.
static void ApplyObsProtection() {
    if (!g_hwnd) {
        comet_capture::Apply(nullptr, false, g_obs, GetTickCount64());
        return;
    }
    const comet_capture::Status before = g_obs.status;
    comet_capture::Apply(g_hwnd, HasOpt2(OPT2_OBS_PROTECT), g_obs, GetTickCount64());
    if (g_obs.status != before) {
        if (g_obs.status == comet_capture::Status::Denied)
            Log("obs protection denied (win32 error %lu)", g_obs.error);
        else
            Log("obs protection %s", comet_capture::Describe(g_obs));
    }
}

static bool LoadState() {

    // 1. Try shared memory first (sub-microsecond memory copy, zero disk lag)

    if (g_shm_state_p) {

        const State* s = reinterpret_cast<const State*>(g_shm_state_p);

        if (s->magic == STATE_MAGIC && s->version == VERSION) {

            if (s->seq != g_state.seq) {

                g_state = *s;

                uint32_t count = s->count;

                if (count > 128) count = 128;

                g_ents.resize(count);

                if (count > 0) {

                    const Ent* src = reinterpret_cast<const Ent*>(reinterpret_cast<const uint8_t*>(g_shm_state_p) + sizeof(State));

                    memcpy(g_ents.data(), src, count * sizeof(Ent));

                }
                ParseLoot(reinterpret_cast<const uint8_t*>(g_shm_state_p), 65536, count);

            }

            return true;

        }

    }

    // 2. Disk file fallback

    if (!StateFileChanged()) return true;

    if (!ReadAll(StatePath().c_str(), g_buffer) || g_buffer.size() < sizeof(State)) return false;

    State s{};

    memcpy(&s, g_buffer.data(), sizeof(s));

    if (s.magic != STATE_MAGIC || s.version != VERSION) return false;

    size_t max_count = (g_buffer.size() - sizeof(State)) / sizeof(Ent);

    uint32_t count = s.count;

    if (count > max_count) count = static_cast<uint32_t>(max_count);

    if (count > 128) count = 128;

    g_state = s;

    g_state.count = count;

    g_ents.resize(count);

    if (count) {

        memcpy(g_ents.data(), g_buffer.data() + sizeof(State), count * sizeof(Ent));

    }

    ParseLoot(g_buffer.data(), g_buffer.size(), count);

    return true;

}



static std::string NameOf(const CatEntry& e) {

    char buf[49];

    memcpy(buf, e.name, 48);

    buf[48] = 0;

    return std::string(buf);

}



static const char* HeroNameById(uint32_t id) {

    switch (id) {

        case 101: return "Ethan";

        case 102: return "Jet";

        case 103: return "Nova";

        case 104: return "Val";

        case 105: return "Hank";

        case 106: return "Spike";

        case 107: return "E.M.";

        case 108: return "Krak";

        case 110: return "Ran";

        case 111: return "Nach";

        case 112: return "Blas";

        case 113: return "Zero";

        case 114: return "Volt";

        case 115: return "Emma";

        case 116: return "Kain";

        case 117: return "Katy";

        case 119: return "Jack";

        case 122: return "Luci";

        case 124: return "Alon";

        case 125: return "Vox";

        default: return nullptr;

    }

}



static std::string HeroLabel(uint32_t id, const CatEntry* e) {

    if (const char* n = HeroNameById(id)) return std::string(n);

    if (e) {

        char tag[5] = {};

        uint32_t d = e->d;

        tag[0] = (char)(d & 0xFF);

        tag[1] = (char)((d >> 8) & 0xFF);

        tag[2] = (char)((d >> 16) & 0xFF);

        tag[3] = (char)((d >> 24) & 0xFF);

        for (int i = 0; i < 4; ++i) {

            if (tag[i] < 32 || tag[i] > 126) tag[i] = 0;

        }

        for (int i = 3; i >= 0; --i) {

            if (tag[i] == ' ' || tag[i] == 0) tag[i] = 0;

            else break;

        }

        if (tag[0]) return std::string(tag);

    }

    char buf[32];

    snprintf(buf, sizeof(buf), "Hero %u", id);

    return std::string(buf);

}


// Hero name for an ESP chip. Unknown/missing replicated ids print nothing rather
// than "Hero 0": a target without hero data must not gain a fake label.
static std::string EnemyHeroLabel(uint32_t hero_id) {

    if (!hero_id) return std::string();

    const char* known = HeroNameById(hero_id);

    if (known) return std::string(known);

    char buf[24];

    snprintf(buf, sizeof(buf), "Hero %u", hero_id);

    return std::string(buf);

}


// Per-team tint for a raw team id. The colour is generated once per match and
// held (see teamtint::Labels), so boxes and snaplines never flicker while you
// look around. `reserved` holds the user's own enemy/teammate colours, which the
// generated palette must not reuse, so a tinted team never looks untinted.
// Returns 0 when the team is unknown; callers keep their base colour.
// The set of colours the user already uses for enemies / teammates / monsters.
// Built once per frame: TeamTint is called per target, and a fresh hash set per
// call was a heap allocation inside the present hook.
using ReservedColours = std::unordered_set<uint32_t>;

static ReservedColours BuildReservedColours() {

    ReservedColours reserved;

    reserved.reserve(3);

    reserved.insert(g_cfg.col_box & 0x00FFFFFFu);

    reserved.insert(g_cfg.col_team & 0x00FFFFFFu);

    reserved.insert(g_cfg.col_monster & 0x00FFFFFFu);

    return reserved;

}

static ImU32 TeamTint(int team, const ReservedColours& reserved) {

    if (team <= 0) return 0;

    // Alpha comes from the user's own enemy colour so a team tint reads at the
    // same weight as every other box.

    const uint32_t alpha = g_cfg.col_box & 0xFF000000u;

    return (g_team_labels.ColourFor(team, reserved) & 0x00FFFFFFu) | alpha;

}

static ImU32 TeamTint(int team) {

    return TeamTint(team, BuildReservedColours());

}



static std::string CurrentHeroLabel() {

    uint32_t id = g_state.local_hero_id;

    if (!id) return std::string("unknown");

    auto it = g_hero_skins.find(id);

    const CatEntry* e = nullptr;

    if (it != g_hero_skins.end() && !it->second.empty()) e = &g_heroes[(size_t)it->second[0]];

    return HeroLabel(id, e);

}



static void LoadCatalog() {

    if (!ReadAll(CatalogPath().c_str(), g_buffer) || g_buffer.size() < sizeof(CatHead)) return;

    CatHead h{};

    memcpy(&h, g_buffer.data(), sizeof(h));

    if (h.magic != CATALOG_MAGIC || h.version != VERSION) return;

    if (h.gun_count < 100) return;

    size_t need = sizeof(CatHead) + (size_t)h.hero_count * sizeof(CatEntry) + (size_t)h.gun_count * sizeof(CatEntry);

    if (g_buffer.size() < need) return;

    g_heroes.resize(h.hero_count);

    g_guns.resize(h.gun_count);

    const uint8_t* p = g_buffer.data() + sizeof(CatHead);

    if (h.hero_count) memcpy(g_heroes.data(), p, (size_t)h.hero_count * sizeof(CatEntry));

    p += (size_t)h.hero_count * sizeof(CatEntry);

    if (h.gun_count) memcpy(g_guns.data(), p, (size_t)h.gun_count * sizeof(CatEntry));



    g_hero_skins.clear();

    g_gun_skins.clear();

    g_gun_label.clear();

    g_gun_kind.clear();

    g_gun_default.clear();

    g_hero_ids.clear();

    g_gun_ids.clear();

    g_knife_ids.clear();

    g_weapon_ids.clear();



    for (size_t i = 0; i < g_heroes.size(); ++i) {

        const auto& e = g_heroes[i];

        auto& v = g_hero_skins[e.a];

        if (v.empty()) g_hero_ids.push_back(e.a);

        v.push_back((int)i);

    }

    for (size_t i = 0; i < g_guns.size(); ++i) {

        const auto& e = g_guns[i];

        auto& v = g_gun_skins[e.b];

        if (v.empty()) {

            g_gun_ids.push_back(e.b);

            g_gun_label[e.b] = NameOf(e);

            g_gun_kind[e.b] = e.d;

        }

        if (g_gun_default.find(e.b) == g_gun_default.end() && e.c == 0) {

            g_gun_default[e.b] = e.a;

        }

        v.push_back((int)i);

    }

    for (uint32_t gid : g_gun_ids) {

        if (g_gun_default.find(gid) == g_gun_default.end()) {

            auto it = g_gun_skins.find(gid);

            if (it != g_gun_skins.end() && !it->second.empty()) {

                g_gun_default[gid] = g_guns[(size_t)it->second[0]].a;

            }

        }

    }

    for (uint32_t gid : g_gun_ids) {

        uint32_t k = g_gun_kind[gid];

        if (k >= 2) g_knife_ids.push_back(gid);

        else g_weapon_ids.push_back(gid);

    }

    std::sort(g_hero_ids.begin(), g_hero_ids.end());

    std::sort(g_gun_ids.begin(), g_gun_ids.end());

    std::sort(g_knife_ids.begin(), g_knife_ids.end());

    std::sort(g_weapon_ids.begin(), g_weapon_ids.end());

    g_catalog_ready = true;

    Log("catalog loaded heroes=%u guns=%u knife_guns=%u weapon_guns=%u",

        h.hero_count, h.gun_count, (unsigned)g_knife_ids.size(), (unsigned)g_weapon_ids.size());

}



static bool ContainsNoCase(const std::string& hay, const char* needle) {

    if (!needle || !needle[0]) return true;

    size_t n = strlen(needle);

    if (n > hay.size()) return false;

    for (size_t i = 0; i + n <= hay.size(); ++i) {

        bool ok = true;

        for (size_t j = 0; j < n; ++j) {

            unsigned char a = (unsigned char)hay[i + j];

            unsigned char b = (unsigned char)needle[j];

            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');

            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');

            if (a != b) {

                ok = false;

                break;

            }

        }

        if (ok) return true;

    }

    return false;

}



static const char* KeyName(int32_t vk) {

    static char buf[64];

    if (vk == 0) return "None";

    if (vk == VK_LBUTTON) return "Mouse1";

    if (vk == VK_RBUTTON) return "Mouse2";

    if (vk == VK_MBUTTON) return "Mouse3";

    if (vk == VK_XBUTTON1) return "Mouse4";

    if (vk == VK_XBUTTON2) return "Mouse5";

    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC) << 16;
    if(vk==VK_INSERT||vk==VK_DELETE||vk==VK_HOME||vk==VK_END||vk==VK_PRIOR||vk==VK_NEXT||
       vk==VK_LEFT||vk==VK_RIGHT||vk==VK_UP||vk==VK_DOWN||vk==VK_DIVIDE||vk==VK_NUMLOCK) sc|=1u<<24;

    if (GetKeyNameTextA((LONG)sc, buf, sizeof(buf)) > 0) return buf;

    snprintf(buf, sizeof(buf), "VK%u", (unsigned)vk);

    return buf;

}



static void BeginBind(int slot) {

    g_bind_slot = slot;

    g_bind_armed = false;

}



static SHORT RawAsyncKeyState(int vk);



static void CaptureBind(int vk) {

    if (vk <= 0 || vk > 255) return;

    if (g_bind_slot == 1) g_cfg.aim_key = vk;

    else if (g_bind_slot == 2) g_cfg.aim_key2 = vk;

    else if (g_bind_slot == 3) g_cfg.menu_key = vk;

    g_bind_slot = 0;

    g_bind_armed = false;

    g_cfg_dirty = true;

}



static void ProcessBind() {

    if (!g_bind_slot) return;

    int found = 0;

    for (int vk = 1; vk < 256; ++vk) {

        if ((RawAsyncKeyState(vk) & 0x8000) != 0) {

            found = vk;

            break;

        }

    }

    if (!found) {

        g_bind_armed = true;

        return;

    }

    if (!g_bind_armed) return;

    CaptureBind(found);

}



static bool CheckU8(const char* label, uint8_t& v) {

    bool b = v != 0;

    if (CometUi::Toggle(label, &b, ImGui::GetContentRegionAvail().x)) {

        v = b ? 1 : 0;

        g_cfg_dirty = true;

        return true;

    }

    return false;

}



static bool FlagCheck(const char* label, uint32_t flag) {

    bool b = Has(flag);

    if (CometUi::Toggle(label, &b, ImGui::GetContentRegionAvail().x)) {

        SetF(flag, b);

        return true;

    }

    return false;

}



static bool SliderI32(const char* label, int32_t& v, int32_t lo, int32_t hi) {

    int tmp = v;

    if (CometUi::Slider(label, &tmp, lo, hi, "%d")) {

        v = tmp;

        g_cfg_dirty = true;

        return true;

    }

    return false;

}



static bool SliderF32(const char* label, float& v, float lo, float hi, const char* fmt) {

    float tmp = v;

    if (CometUi::Slider(label, &tmp, lo, hi, fmt)) {

        v = tmp;

        g_cfg_dirty = true;

        return true;

    }

    return false;

}



// Inventory wire format: <III> magic/version/count, then CatEntry records.
// Entry a=type, b=hero/weapon group, c=selection ID, d=selected for auto apply.
static constexpr uint32_t SKIN_INVENTORY_MAGIC = 0x42534956;
static constexpr size_t MAX_SKIN_INVENTORY = 4096;
static std::vector<CatEntry> g_skin_inventory;
static bool g_inventory_loaded = false;
static bool g_inventory_exists = false;
static bool g_inventory_writable = true;
static std::string g_inventory_error;

static const std::wstring& SkinInventoryPath() {
    static const std::wstring path = [] {
        wchar_t local[32768]{};
        DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        if (!count || count >= 32768) return IpcDir() + L"\\skin_inventory.bin";
        std::wstring folder = std::wstring(local) + L"\\Comet";
        CreateDirectoryW(folder.c_str(), nullptr);
        folder += L"\\BloodStrike";
        CreateDirectoryW(folder.c_str(), nullptr);
        return folder + L"\\skin_inventory.bin";
    }();
    return path;
}

static bool SaveSkinInventory(const std::vector<CatEntry>& entries);

static void LoadSkinInventory(bool reload = false) {
    if (g_inventory_loaded && !reload) return;
    g_inventory_loaded = true;
    g_inventory_error.clear();
    if (GetFileAttributesW(SkinInventoryPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            g_inventory_exists = false;
            g_inventory_writable = true;
            g_skin_inventory.clear();
            return;
        }
    }
    g_inventory_exists = true;
    g_inventory_writable = false;
    std::vector<uint8_t> bytes;
    if (!ReadAll(SkinInventoryPath().c_str(), bytes) || bytes.size() < 12) {
        g_inventory_error = "Cannot read Inventory. Your saved file has not been changed.";
        return;
    }
    uint32_t header[3]{};
    memcpy(header, bytes.data(), sizeof(header));
    if (header[0] != SKIN_INVENTORY_MAGIC || header[1] != 1 || header[2] > MAX_SKIN_INVENTORY ||
        bytes.size() != sizeof(header) + size_t(header[2]) * sizeof(CatEntry)) {
        g_inventory_error = "Inventory format is invalid. Your saved file has not been changed.";
        return;
    }
    std::vector<CatEntry> entries(header[2]);
    if (!entries.empty()) memcpy(entries.data(), bytes.data() + sizeof(header), entries.size() * sizeof(CatEntry));
    std::unordered_map<uint64_t, bool> ids, active_slots;
    for (auto& entry : entries) {
        const uint64_t id = (uint64_t(entry.a) << 32) | entry.c;
        const uint64_t slot = (uint64_t(entry.a) << 32) | entry.b;
        if (entry.a < 1 || entry.a > 3 || !entry.b || !entry.c || entry.d > 1 || ids.count(id) ||
            (entry.d && active_slots.count(slot))) {
            g_inventory_error = "Inventory contains invalid entries. Your saved file has not been changed.";
            return;
        }
        ids[id] = true;
        if (entry.d) active_slots[slot] = true;
        entry.name[47] = 0;
    }
    const auto old_size = entries.size();
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const CatEntry& entry) {
        return !entry.d;
    }), entries.end());
    g_skin_inventory.swap(entries);
    g_inventory_writable = true;
    if (g_skin_inventory.size() != old_size) SaveSkinInventory(g_skin_inventory);
}

static bool SaveSkinInventory(const std::vector<CatEntry>& entries) {
    if (!g_inventory_writable) return false;
    if (entries.size() > MAX_SKIN_INVENTORY) {
        g_inventory_error = "Inventory is full. Remove a saved skin before adding another.";
        return false;
    }
    uint32_t header[] = {SKIN_INVENTORY_MAGIC, 1, static_cast<uint32_t>(entries.size())};
    std::vector<uint8_t> bytes(sizeof(header) + entries.size() * sizeof(CatEntry));
    memcpy(bytes.data(), header, sizeof(header));
    if (!entries.empty()) memcpy(bytes.data() + sizeof(header), entries.data(), entries.size() * sizeof(CatEntry));
    if (!WriteAll(SkinInventoryPath().c_str(), bytes.data(), bytes.size())) {
        g_inventory_error = "Could not save Inventory. Check folder permissions and select the skin again.";
        return false;
    }
    g_skin_inventory = entries;
    g_inventory_exists = true;
    g_inventory_error.clear();
    return true;
}

static bool RememberSkin(uint32_t type, uint32_t selection) {
    LoadSkinInventory();
    if (!g_inventory_writable) return false;
    CatEntry saved{};
    bool found = false;
    if (type == 1) {
        for (const auto& entry : g_heroes) if (entry.c == selection) {
            saved = entry; saved.b = entry.a; found = true; break;
        }
    } else if (type == 2 || type == 3) {
        for (const auto& entry : g_guns) if (entry.a == selection && (entry.d >= 2) == (type == 3)) {
            saved = entry; found = true; break;
        }
    }
    if (!found) for (const auto& entry : g_skin_inventory) if (entry.a == type && entry.c == selection) {
        saved = entry; found = true; break;
    }
    if (!found || !saved.b || !selection) {
        g_inventory_error = "This skin is not in the catalog or Inventory. Reload the catalog and try again.";
        return false;
    }
    saved.a = type; saved.c = selection; saved.d = 1;
    auto entries = g_skin_inventory;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const CatEntry& entry) {
        return !entry.d || (entry.a == type && (entry.b == saved.b || entry.c == selection));
    }), entries.end());
    // Keep one equipped skin per slot; file order preserves selection recency.
    entries.push_back(saved);
    return SaveSkinInventory(entries);
}

static void MigrateSkinInventory() {
    LoadSkinInventory();
    if (g_inventory_exists || !g_inventory_writable || !g_catalog_ready) return;
    // Preserve the three selections from configurations saved before Inventory.
    const uint32_t previous[] = {g_cfg.skin_hero_item, g_cfg.skin_gun_item, g_cfg.skin_knife_item};
    for (uint32_t type = 1; type <= 3; ++type) {
        if (previous[type - 1]) RememberSkin(type, previous[type - 1]);
    }
}


static void ApplyItem(int type, uint32_t item) {
    MigrateSkinInventory();
    if (!RememberSkin(type, item)) return;

    g_cfg.apply_type = type;

    g_cfg.apply_item = item;

    if (type == 1) g_cfg.skin_hero_item = item;

    else if (type == 2) g_cfg.skin_gun_item = item;

    else if (type == 3) g_cfg.skin_knife_item = item;

    g_cfg.apply_seq++;

    if (g_cfg.apply_seq == 0) g_cfg.apply_seq = 1;

    g_cfg_dirty = true;
    SaveCfg();

}



static bool Section(const char* title) {
    ImGui::Dummy(ImVec2(0,8));
    if (g_font_big) ImGui::PushFont(g_font_big,14);
    ImGui::TextUnformatted(title);
    if (g_font_big) ImGui::PopFont();
    ImGui::Dummy(ImVec2(0,6));
    return true;
}

static bool BeginCard(const char* id, const ImVec2& size) {

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.082f,.106f,.145f,1));

    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(CometUi::Border));

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 10.0f));

    bool r = ImGui::BeginChild(id, ImVec2(size.x, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

    ImGui::PopStyleVar(3);

    ImGui::PopStyleColor(2);

    return r;

}



static void EndCard() {

    ImGui::EndChild();

}



static bool TabButton(const char* label, bool active, float width) {
    const ImVec2 p=ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(CometUi::Elevated));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(CometUi::Elevated));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(active?CometUi::Text:CometUi::Muted));
    const bool pressed=ImGui::Button(label,ImVec2(width,30));
    ImGui::PopStyleColor(4);
    if(active) ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x,p.y+32),ImVec2(p.x+width,p.y+34),CometUi::Accent);
    return pressed;
}

static void BindRow(const char* title, int32_t vk, int slot) {

    ImGui::TextUnformatted(title);

    ImGui::SameLine(std::max(0.f, ImGui::GetContentRegionAvail().x - 110.f));

    char buf[80];

    snprintf(buf, sizeof(buf), "%s##b%d", KeyName(vk), slot);

    if (ImGui::Button(buf, ImVec2(110.0f, 0.0f))) BeginBind(slot);

}



static ImVec4 ColF(uint32_t v) {

    return ImVec4(((v >> 0) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f,

                  ((v >> 16) & 0xFF) / 255.0f, ((v >> 24) & 0xFF) / 255.0f);

}



static uint32_t PackC(const float* c) {

    auto q = [](float x) { return (uint32_t)(std::min(1.0f, std::max(0.0f, x)) * 255.0f + 0.5f); };

    return q(c[0]) | (q(c[1]) << 8) | (q(c[2]) << 16) | (q(c[3]) << 24);

}



static bool ColorRow(const char* label, uint32_t& v) {

    float c[4];

    ImVec4 f = ColF(v);

    c[0] = f.x; c[1] = f.y; c[2] = f.z; c[3] = f.w;

    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(std::max(0.f, ImGui::GetContentRegionAvail().x - 28.f));
    const bool changed = ImGui::ColorEdit4("##color", c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
    ImGui::PopID();
    if (changed) {

        v = PackC(c);

        g_cfg_dirty = true;

        return true;

    }

    return false;

}



// Ground-loot ESP. The runtime already resolved and projected each captured
// CombatItemEntity/Supply; this only draws what the reader published, so a
// layout change upstream shows nothing instead of guessing at a position.
//
// Kept deliberately plain so it belongs to the same visual language as the rest
// of the overlay: a small tinted dot with a dark rim, no text, no icons. Text
// here was both the loudest thing on screen and the flicker source - a label
// anchored to a projected point jitters by a pixel every frame the camera moves,
// which makes glyphs shimmer. A dot cannot shimmer.
static void DrawLootESP() {
    if (!g_state.in_match || !HasOpt2(OPT2_ESP_LOOT) || g_loot.empty()) return;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ds.x <= 1.0f || ds.y <= 1.0f) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float limit = g_cfg.loot_max_dist > 0.0f ? g_cfg.loot_max_dist : 120.0f;
    const ImU32 colour = g_cfg.col_loot & 0x00FFFFFFu;
    const ImU32 fill = colour | 0xF5000000u;
    const ImU32 rim = IM_COL32(0, 0, 0, 190);
    int drawn = 0;
    for (const auto& item : g_loot) {
        if (drawn >= 96) break; // hard cap: 64 is what the reader sends
        if (!std::isfinite(item.distance) || item.distance <= 0.0f || item.distance > limit) continue;
        if (!std::isfinite(item.screen[0]) || !std::isfinite(item.screen[1])) continue;
        if (item.screen[0] < 0.0f || item.screen[0] > 1.0f ||
            item.screen[1] < 0.0f || item.screen[1] > 1.0f) continue;
        const ImVec2 p(item.screen[0] * ds.x, item.screen[1] * ds.y);
        // Containers read slightly larger than loose drops. 12-gon rather than
        // 24: half the vertices, indistinguishable at this size.
        const float r = item.kind ? 3.2f : 2.2f;
        dl->AddCircleFilled(p, r + 1.0f, rim, 12);
        dl->AddCircleFilled(p, r, fill, 12);
        ++drawn;
    }
}

static void DrawESP() {
    if (!Has(F_ESP) || !g_state.in_match) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ds.x <= 1.0f || ds.y <= 1.0f) return;
    float W = ds.x;
    float H = ds.y;

    // Read the per-frame settings once. Has() is cheap, but it is called per
    // target per element, and the branch-heavy reads were measurably worse than
    // a handful of locals.
    const uint32_t flags = g_cfg.flags;
    const uint32_t opts = g_cfg.opts;
    const bool esp_show_visible = (flags & F_ESP_VISIBLE) != 0;
    const bool shadow = (flags & F_ESP_SHADOW) != 0;
    const bool team_colors_on = (opts & OPT2_TEAM_COLORS) != 0;
    const bool team_label_on = (opts & OPT2_TEAM_LABEL) != 0;
    const bool hero_label_on = (opts & OPT2_ESP_HERO) != 0;
    const float max_dist = (float)g_cfg.esp_max_dist;

    // Team tints are generated per team, but the "do not reuse these" set only
    // changes when the user edits a colour, so build it once per frame.
    ReservedColours reserved_cols = BuildReservedColours();

    if (Has(F_ESP_COUNT)) {
        int enemies = 0, knocked = 0, bots = 0, mates = 0;
        for (const auto& ce : g_ents) {
            if (ce.distance > (float)g_cfg.esp_max_dist) continue;
            if (ce.is_team && !Has(F_ESP_TEAM)) continue;
            if (ce.is_monster) bots++;
            else if (ce.is_team) mates++;
            else if (ce.is_knocked != 0) knocked++;
            else enemies++;
        }
        struct Chip { const char* label; int value; ImU32 color; };
        Chip chips[4] = {
            { "ENEMY",   enemies, IM_COL32(240,  95,  95, 255) },
            { "KNOCKED", knocked, IM_COL32(255, 170,  45, 255) },
            { "BOT",     bots,    IM_COL32(255, 205,  95, 255) },
            { "TEAM",    mates,   IM_COL32( 95, 170, 255, 255) },
        };
        float cx = 18.0f, cy = 18.0f;
        for (const auto& c : chips) {
            if (c.value == 0 && (&c - chips) != 0) continue;  // keep ENEMY always visible
            char lv[16]; snprintf(lv, sizeof(lv), "%d", c.value);
            ImVec2 lblSz = ImGui::CalcTextSize(c.label);
            ImVec2 valSz = ImGui::CalcTextSize(lv);
            float chipW = 14.0f + lblSz.x + 10.0f + valSz.x + 14.0f;
            float chipH = 26.0f;
            ImVec2 a(cx, cy), b(cx + chipW, cy + chipH);
            dl->AddRectFilled(a, b, IM_COL32(14, 16, 22, 225), 8.0f);
            dl->AddRect(a, b, IM_COL32(45, 52, 68, 220), 8.0f, 0, 1.0f);
            dl->AddCircleFilled(ImVec2(cx + 10.0f, cy + chipH * 0.5f), 3.5f, c.color);
            dl->AddText(ImVec2(cx + 20.0f, cy + (chipH - lblSz.y) * 0.5f),
                        IM_COL32(205, 210, 225, 235), c.label);
            dl->AddText(ImVec2(cx + 20.0f + lblSz.x + 10.0f, cy + (chipH - valSz.y) * 0.5f),
                        c.color, lv);
            cx += chipW + 8.0f;
        }
    }

    for (auto it = g_ents.rbegin(); it != g_ents.rend(); ++it) {
        const auto& e = *it;
        if (e.distance > max_dist) continue;
        if (e.is_team && !(flags & F_ESP_TEAM)) continue;

        auto ok = [](const float* s) {
            return std::isfinite(s[0]) && std::isfinite(s[1]) && !(s[0] == -1.f && s[1] == -1.f);
        };
        bool head_ok = ok(e.head_screen);
        bool feet_ok = ok(e.feet_screen);
        bool on_screen = (head_ok && e.head_screen[0] >= 0 && e.head_screen[0] <= 1 && e.head_screen[1] >= 0 && e.head_screen[1] <= 1) ||
                         (feet_ok && e.feet_screen[0] >= 0 && e.feet_screen[0] <= 1 && e.feet_screen[1] >= 0 && e.feet_screen[1] <= 1);
        if (head_ok && feet_ok) {
            on_screen = std::max(e.head_screen[0],e.feet_screen[0]) >= 0 && std::min(e.head_screen[0],e.feet_screen[0]) <= 1 &&
                        std::max(e.head_screen[1],e.feet_screen[1]) >= 0 && std::min(e.head_screen[1],e.feet_screen[1]) <= 1;
        }
        if (!on_screen && (g_cfg.esp_style & ESP_OFFSCREEN_ARROW)) {
            float dx, dy;
            if (head_ok || feet_ok) {
                const float* p = head_ok ? e.head_screen : e.feet_screen;
                dx = (p[0] - .5f) * W;
                dy = (p[1] - .5f) * H;
            } else {
                float x = e.feet[0] - g_state.cam_pos[0], z = e.feet[2] - g_state.cam_pos[2];
                // Behind-camera targets use horizontal camera bearing: rear points down.
                dx = x * g_state.cam_forward[2] - z * g_state.cam_forward[0];
                dy = -(x * g_state.cam_forward[0] + z * g_state.cam_forward[2]);
            }
            esp::Point tip{}, dir{};
            if (esp::Arrow(W, H, dx, dy, tip, dir)) {
                ImU32 color = e.is_team ? g_cfg.col_team : e.is_knocked ? IM_COL32(255,160,40,245) : g_cfg.col_line;
                ImVec2 a(tip.x, tip.y), b(tip.x-dir.x*16-dir.y*7, tip.y-dir.y*16+dir.x*7),
                       c(tip.x-dir.x*16+dir.y*7, tip.y-dir.y*16-dir.x*7);
                auto* front = ImGui::GetForegroundDrawList();
                front->AddTriangleFilled(a,b,c,color);
                front->AddTriangle(a,b,c,IM_COL32(0,0,0,220),1.5f);
            }
        }
        if (!head_ok && !feet_ok) continue;

        float hx = e.head_screen[0] * W;
        float hy = e.head_screen[1] * H;
        float fx = e.feet_screen[0] * W;
        float fy = e.feet_screen[1] * H;

        bool is_knocked = (e.is_knocked != 0);
        float est_scale = is_knocked ? 600.0f : 1200.0f;
        float est = std::max(6.0f, std::min(H * 0.95f, est_scale / std::max(1.0f, e.distance)));

        if (!head_ok) {
            hx = fx;
            hy = fy - est;
        }
        if (!feet_ok) {
            fx = hx;
            fy = hy + est;
        }

        if ((hx < 0 && fx < 0) || (hx > W && fx > W)) continue;
        if (fy < hy) std::swap(hy, fy);
        // Fully above or below the viewport: nothing of this target can be seen,
        // so skip all of its chip/bar/box work. This is the common case with a
        // wide ESP distance and a camera looking at the sky.
        if (fy < 0.0f || hy > H) continue;
        float h = fy - hy;

        // Dynamic minimum box height to guarantee visibility at 150m-200m+
        if (h < 6.0f) {
            float mid = (fy + hy) * 0.5f;
            hy = mid - 3.0f;
            fy = mid + 3.0f;
            h = 6.0f;
        }

        float cx = (hx + fx) * 0.5f;
        float w = std::max(6.0f, std::min(300.0f, h * (is_knocked ? 0.70f : 0.42f)));

        bool hidden = esp_show_visible && !e.visible;
        ImU32 col = hidden ? g_cfg.col_box_hidden : g_cfg.col_box;
        ImU32 scol = hidden ? g_cfg.col_box_hidden : g_cfg.col_skel;

        if (is_knocked) {
            col = IM_COL32(255, 160, 40, 245);
            scol = col;
        } else if (e.is_team) {
            col = g_cfg.col_team;
            scol = g_cfg.col_team;
        } else if (e.is_monster) {
            col = g_cfg.col_monster;
            scol = g_cfg.col_monster;
        }

        // Per-team tints (Comet port). Knocked, teammate and monster colours
        // keep priority, and a hidden-by-visibility entity keeps the hidden
        // colour, so the tint only ever presents a living, visible enemy.
        const bool team_tint = team_colors_on && !is_knocked && !e.is_team &&
                               !e.is_monster && !hidden && e.team > 0;
        if (team_tint) {
            col = TeamTint(e.team, reserved_cols);
            scol = col;
        }

        float th = g_cfg.box_thickness;
        ImVec2 tl(cx - w * 0.5f, hy);
        ImVec2 br(cx + w * 0.5f, fy);

        auto seg = [&](ImVec2 a, ImVec2 b, ImU32 c, float t) {
            if (shadow) dl->AddLine(a, b, IM_COL32(0, 0, 0, 170), t + 1.4f);
            dl->AddLine(a, b, c, t);
        };

        ImFont* esp_font = g_font_esp ? g_font_esp : ImGui::GetFont();
        float esp_font_sz = g_font_esp ? 18.0f : 16.0f;

        auto calc_sz = [&](const char* s) -> ImVec2 {
            if (!s || !*s) return ImVec2(0.0f, 0.0f);
            return esp_font->CalcTextSizeA(esp_font_sz, FLT_MAX, 0.0f, s);
        };

        // Two draws, not nine. The old version stamped the glyph eight times for
        // an outline; at 10 targets x 4 chips that is ~320 extra text draw calls
        // per frame for a difference nobody can see at 18 px.
        auto txt = [&](ImVec2 p, ImU32 c, const char* s) {
            if (!s || !*s) return;
            if (shadow) {
                dl->AddText(esp_font, esp_font_sz, ImVec2(p.x + 1.0f, p.y + 1.0f),
                            IM_COL32(0, 0, 0, 235), s);
            }
            dl->AddText(esp_font, esp_font_sz, p, c, s);
        };

        // Chip: dark pill behind one line of text, centered at cx. Returns advance height.
        // Sharp corners (no rounding) are cheap — rounding tessellates 4 arcs per rect
        // and 10 enemies × 3 chips each would cost ~120 extra verts per frame.
        auto chip = [&](float ccx, float cy_top, const char* s, ImU32 fg) -> float {
            if (!s || !*s) return 0.0f;
            ImVec2 sz = calc_sz(s);
            float pad_x = 6.0f, pad_y = 2.0f;
            ImVec2 a(ccx - sz.x * 0.5f - pad_x, cy_top - pad_y);
            ImVec2 b(ccx + sz.x * 0.5f + pad_x, cy_top + sz.y + pad_y);
            dl->AddRectFilled(a, b, IM_COL32(10, 12, 18, 180));
            dl->AddText(esp_font, esp_font_sz, ImVec2(ccx - sz.x * 0.5f, cy_top), fg, s);
            return sz.y + pad_y * 2.0f + 2.0f;
        };

        if (Has(F_ESP_LINE)) {
            const unsigned mode = g_cfg.esp_style & ESP_LINE_ORIGIN_MASK;
            auto origin = esp::LineOrigin(W,H,mode);
            auto target = esp::LineTarget(cx,tl.y,br.y,mode);
            ImVec2 pa(origin.x,origin.y), pb(target.x,target.y);
            auto* front = ImGui::GetForegroundDrawList();
            // Snapline alpha is its own (usually softer) setting.
            const ImU32 team_line_alpha = g_cfg.col_line & 0xFF000000u;
            const ImU32 line_col = team_tint
                ? (TeamTint(e.team, reserved_cols) & 0x00FFFFFFu) | team_line_alpha
                : g_cfg.col_line;
            front->AddLine(pa,pb,IM_COL32(0,0,0,200),3.0f);
            front->AddLine(pa,pb,line_col,1.5f);
        }

        if (Has(F_ESP_BOX)) {
            if (Has(F_ESP_FILLED)) {
                // Subtle inner tint, bottom slightly stronger for grounding
                ImU32 fill_hi = (col & 0x00FFFFFF) | 0x14000000;
                ImU32 fill_lo = (col & 0x00FFFFFF) | 0x30000000;
                dl->AddRectFilledMultiColor(tl, br, fill_hi, fill_hi, fill_lo, fill_lo);
            }
            if (Has(F_ESP_OUTLINE)) dl->AddRect(ImVec2(tl.x - 1.0f, tl.y - 1.0f), ImVec2(br.x + 1.0f, br.y + 1.0f), IM_COL32(0, 0, 0, 215), 0.0f, 0, th + 2.0f);
            if (Has(F_ESP_CORNER)) {
                float cw = std::max(4.0f, std::min(w, h) * 0.24f);
                // Accent fill tint + thin accent-colored corners
                ImU32 hair = (col & 0x00FFFFFF) | 0x66000000;
                // Top-left
                seg(ImVec2(tl.x, tl.y), ImVec2(tl.x + cw, tl.y), col, th);
                seg(ImVec2(tl.x, tl.y), ImVec2(tl.x, tl.y + cw), col, th);
                dl->AddLine(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(tl.x + cw - 2.0f, tl.y + 2.0f), hair, 1.0f);
                // Top-right
                seg(ImVec2(br.x - cw, tl.y), ImVec2(br.x, tl.y), col, th);
                seg(ImVec2(br.x, tl.y), ImVec2(br.x, tl.y + cw), col, th);
                dl->AddLine(ImVec2(br.x - cw + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, tl.y + 2.0f), hair, 1.0f);
                // Bottom-left
                seg(ImVec2(tl.x, br.y - cw), ImVec2(tl.x, br.y), col, th);
                seg(ImVec2(tl.x, br.y), ImVec2(tl.x + cw, br.y), col, th);
                dl->AddLine(ImVec2(tl.x + 2.0f, br.y - 2.0f), ImVec2(tl.x + cw - 2.0f, br.y - 2.0f), hair, 1.0f);
                // Bottom-right
                seg(ImVec2(br.x - cw, br.y), ImVec2(br.x, br.y), col, th);
                seg(ImVec2(br.x, br.y - cw), ImVec2(br.x, br.y), col, th);
                dl->AddLine(ImVec2(br.x - cw + 2.0f, br.y - 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f), hair, 1.0f);
            } else {
                dl->AddRect(tl, br, col, 1.5f, 0, th);
            }
        }

        if (Has(F_ESP_HEADDOT) && head_ok && !is_knocked) {
            // Crosshair-style: outer ring + tiny inner dot + 4 tick marks
            ImU32 dot = g_cfg.col_headdot;
            if (shadow) dl->AddCircle(ImVec2(hx, hy), 5.0f, IM_COL32(0, 0, 0, 180), 16, 1.8f);
            dl->AddCircle(ImVec2(hx, hy), 4.0f, dot, 16, 1.4f);
            dl->AddCircleFilled(ImVec2(hx, hy), 1.3f, dot);
            dl->AddLine(ImVec2(hx - 7.0f, hy), ImVec2(hx - 5.0f, hy), dot, 1.0f);
            dl->AddLine(ImVec2(hx + 5.0f, hy), ImVec2(hx + 7.0f, hy), dot, 1.0f);
            dl->AddLine(ImVec2(hx, hy - 7.0f), ImVec2(hx, hy - 5.0f), dot, 1.0f);
            dl->AddLine(ImVec2(hx, hy + 5.0f), ImVec2(hx, hy + 7.0f), dot, 1.0f);
        }

        if (Has(F_ESP_SKELETON) && !is_knocked) {
            bool has_bones = false;
            for (int bi = 0; bi < 16; ++bi) {
                if (e.bones[bi][0] >= 0.0f && e.bones[bi][1] >= 0.0f) {
                    has_bones = true;
                    break;
                }
            }
            if (has_bones) {
                static const int PAIRS[15][2] = {
                    {0, 1}, {1, 2}, {2, 3},
                    {1, 4}, {4, 5}, {5, 6},
                    {1, 7}, {7, 8}, {8, 9},
                    {3, 10}, {10, 11}, {11, 12},
                    {3, 13}, {13, 14}, {14, 15}
                };
                for (int p = 0; p < 15; ++p) {
                    const float* b1 = e.bones[PAIRS[p][0]];
                    const float* b2 = e.bones[PAIRS[p][1]];
                    if (b1[0] >= 0.0f && b1[1] >= 0.0f && b2[0] >= 0.0f && b2[1] >= 0.0f) {
                        seg(ImVec2(b1[0] * W, b1[1] * H),
                            ImVec2(b2[0] * W, b2[1] * H),
                            scol, 1.8f);
                    }
                }
                // Joint dots make the skeleton read as anatomy, not just lines.
                // 6-segment circles instead of 8 → 2 fewer tris per joint × 16 joints.
                ImU32 joint = (scol & 0x00FFFFFF) | 0xFF000000;
                for (int bi = 0; bi < 16; ++bi) {
                    const float* b = e.bones[bi];
                    if (b[0] < 0.0f || b[1] < 0.0f) continue;
                    ImVec2 jp(b[0] * W, b[1] * H);
                    dl->AddCircleFilled(jp, 1.6f, joint, 6);
                }
            } else if (h >= 14.0f) {
                float sy = hy + h * 0.28f;
                float sx = w * 0.35f;
                seg(ImVec2(hx, hy), ImVec2(hx, sy), scol, 1.3f);
                seg(ImVec2(hx - sx, sy), ImVec2(hx + sx, sy), scol, 1.3f);
                seg(ImVec2(hx, sy), ImVec2(fx, fy), scol, 1.3f);
            }
        }

        float y = hy - (esp_font_sz + 6.0f);
        if (is_knocked) {
            y -= chip(cx, y, "[KNOCKED]", IM_COL32(255, 175, 45, 255));
        }
        if (team_label_on && !e.is_team && !e.is_monster) {
            const int number = g_team_labels.NumberFor(e.team);
            if (number > 0) {
                char tbuf[24];
                snprintf(tbuf, sizeof(tbuf), "Team %d", number);
                y -= chip(cx, y, tbuf, team_tint ? col : g_cfg.col_team);
            }
        }
        // The header above the box carries at most the knock state, the team and
        // the player name: stacking weapon + hero + name there buried the target.
        if (Has(F_ESP_NAME) && e.name[0]) {
            y -= chip(cx, y, e.name, g_cfg.col_name);
        }
        if (Has(F_ESP_GUN) && e.gun[0]) {
            y -= chip(cx, y, e.gun, g_cfg.col_gun);
        }
        // Feet row, top to bottom: distance, then hero name.
        float foot_y = fy + 6.0f;
        if (Has(F_ESP_DISTANCE)) {
            char dbuf[24];
            snprintf(dbuf, sizeof(dbuf), "%.0f m", e.distance);
            foot_y += chip(cx, foot_y, dbuf, g_cfg.col_dist);
        }
        if (hero_label_on && !e.is_team) {
            const std::string hero = EnemyHeroLabel(e.hero_id);
            if (!hero.empty()) foot_y += chip(cx, foot_y, hero.c_str(), g_cfg.col_name);
        }

        bool has_hp = Has(F_ESP_HEALTH) && e.max_hp > 0.0f;
        bool has_armor = Has(F_ESP_ARMOR) && e.max_armor > 0.0f && e.armor > 0.0f;

        // Bars live in their own stacked slot on the left of the box.
        const float bar_w = 4.0f;
        const float bar_gap = 2.0f;
        float slot_count = (has_hp ? 1.0f : 0.0f) + (has_armor ? 1.0f : 0.0f);
        float slot_right = tl.x - 4.0f;
        float first_x = slot_right - (slot_count * bar_w + (slot_count > 0 ? (slot_count - 1.0f) * bar_gap : 0.0f));

        auto draw_bar = [&](float bx, float ratio, ImU32 fg, bool is_armor) {
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
            // Outlined track
            dl->AddRectFilled(ImVec2(bx - 1.0f, hy - 1.0f), ImVec2(bx + bar_w + 1.0f, fy + 1.0f),
                              IM_COL32(0, 0, 0, 220), 2.0f);
            dl->AddRectFilled(ImVec2(bx, hy), ImVec2(bx + bar_w, fy), IM_COL32(28, 32, 42, 255), 1.5f);
            float top = fy - h * ratio;
            if (Has(F_ESP_GRADIENT) && !is_knocked) {
                ImU32 low = (fg & 0x00FFFFFF) | 0x90000000;
                dl->AddRectFilledMultiColor(ImVec2(bx, top), ImVec2(bx + bar_w, fy), fg, fg, low, low);
            } else {
                dl->AddRectFilled(ImVec2(bx, top), ImVec2(bx + bar_w, fy), fg, 1.5f);
            }
            // Tick marks at 25 / 50 / 75 %
            ImU32 tick = IM_COL32(0, 0, 0, 150);
            for (int i = 1; i < 4; ++i) {
                float ty = hy + h * (i / 4.0f);
                dl->AddLine(ImVec2(bx, ty), ImVec2(bx + bar_w, ty), tick, 1.0f);
            }
            // Numeric readout for HP when the box is tall enough and HP is low (<=60%).
            // Sharp chip + skip readout entirely for distant/tiny targets.
            if (!is_armor && !is_knocked && h >= 36.0f && ratio <= 0.60f) {
                char hbuf[16];
                int cur = (int)std::round(e.hp);
                snprintf(hbuf, sizeof(hbuf), "%d", cur);
                ImVec2 sz = ImGui::CalcTextSize(hbuf);
                float ty = std::max(hy, std::min(fy - sz.y, top - sz.y * 0.5f));
                float tx = bx - sz.x - 4.0f;
                dl->AddRectFilled(ImVec2(tx - 3.0f, ty - 1.0f),
                                  ImVec2(tx + sz.x + 3.0f, ty + sz.y + 1.0f),
                                  IM_COL32(10, 12, 18, 200));
                dl->AddText(ImVec2(tx, ty), fg, hbuf);
            }
        };

        if (has_hp) {
            float ratio = e.hp / e.max_hp;
            ImU32 hp_color = is_knocked ? IM_COL32(255, 140, 0, 245) :
                             (ratio > 0.60f ? g_cfg.col_health :
                             (ratio > 0.25f ? IM_COL32(245, 215, 60, 245) : IM_COL32(245, 60, 60, 245)));
            draw_bar(first_x, ratio, hp_color, false);
        }
        if (has_armor) {
            float ratio = e.armor / e.max_armor;
            // The bar is coloured by the target's maximum armor tier, not by a
            // number next to it: 50 grey, 75 blue, 100 purple, 125 gold. This is
            // automatic whenever the armor bar is on - there is nothing to turn
            // on and nothing to type.
            const ImU32 arm_color = is_knocked
                ? IM_COL32(120, 140, 180, 240)
                : armortier::Packed(e.max_armor);
            float bx = has_hp ? (first_x + bar_w + bar_gap) : first_x;
            draw_bar(bx, ratio, arm_color, true);
        }
    }
}

static void DrawFOV() {

    if (!g_state.in_match) return;

    bool draw_aim = Has(F_AIM_FOV_CIRCLE) && Has(F_AIM);

    if (!draw_aim) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    ImVec2 ds = ImGui::GetIO().DisplaySize;

    if (ds.x <= 1.0f || ds.y <= 1.0f) return;

    float W2 = ds.x * 0.5f;

    float H2 = ds.y * 0.5f;

    const float game_fov=g_state.fov>1.f?g_state.fov:40.f;
    const float tan_half=std::max(.01f,std::tan(game_fov*.5f*3.14159265f/180.f));
    const float r_aim=std::max(6.f,std::tan(std::max(1.f,g_cfg.aim_fov)*.5f*3.14159265f/180.f)/tan_half*H2);
    if(r_aim<ds.y*1.5f) dl->AddCircle(ImVec2(W2,H2),r_aim,g_cfg.col_fov,64,1.5f);

    dl->AddCircleFilled(ImVec2(W2, H2), 2.5f, IM_COL32(255, 255, 255, 220), 8);

}



static void DrawRadar() {
    if (!Has(F_ESP) || !(g_cfg.reserved & OPT_ESP_RADAR) ||
        !g_state.in_match || !g_state.local_valid) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x < 260.f || display.y < 260.f) return;
    constexpr float radius = 92.f, range = 100.f;
    const ImVec2 center(display.x - radius - 24.f, radius + 46.f);
    auto* dl = ImGui::GetBackgroundDrawList();
    dl->AddCircleFilled(center, radius + 10.f, IM_COL32(13,18,26,220), 64);
    dl->AddCircle(center, radius, IM_COL32(90,170,210,160), 64, 1.5f);
    dl->AddCircle(center, radius * .5f, IM_COL32(95,125,150,65), 48);
    dl->AddLine(ImVec2(center.x-radius,center.y), ImVec2(center.x+radius,center.y), IM_COL32(95,125,150,65));
    dl->AddLine(ImVec2(center.x,center.y-radius), ImVec2(center.x,center.y+radius), IM_COL32(95,125,150,65));
    dl->AddText(ImVec2(center.x-radius,center.y-radius-29.f), IM_COL32(210,225,240,240), "RADAR / 100 m");
    for (const auto& e : g_ents) {
        if (!e.alive || !std::isfinite(e.distance) || e.distance > g_cfg.esp_max_dist ||
            (e.is_team && !Has(F_ESP_TEAM))) continue;
        float x, y;
        if (!radar::Project(e.feet[0]-g_state.local_pos[0], e.feet[2]-g_state.local_pos[2],
                            g_state.cam_forward[0], g_state.cam_forward[2], range, x, y)) continue;
        const ImVec2 point(center.x+x*(radius-5.f), center.y+y*(radius-5.f));
        const ImU32 color = e.is_team ? g_cfg.col_team : e.is_knocked ?
            IM_COL32(255,175,55,255) : IM_COL32(255,90,100,255);
        dl->AddCircleFilled(point, 4.f, color, 12);
        dl->AddCircle(point, 4.f, IM_COL32(0,0,0,200), 12);
    }
    dl->AddTriangleFilled(ImVec2(center.x,center.y-7.f), ImVec2(center.x-5.f,center.y+5.f),
                          ImVec2(center.x+5.f,center.y+5.f), IM_COL32(225,245,255,255));
}

static void DrawESPTab() {
    if (!ImGui::BeginTable("visuals_columns",2,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_NoPadOuterX)) return;
    ImGui::TableNextColumn();
    BeginCard("visuals_elements",ImVec2(0,0));
    Section("Player overlay");
    FlagCheck("Enable ESP",F_ESP);
    bool radar_on = (g_cfg.reserved & OPT_ESP_RADAR) != 0;
    if (CometUi::Toggle("2D radar (100 m)",&radar_on,ImGui::GetContentRegionAvail().x)) {
        g_cfg.reserved = radar_on ? g_cfg.reserved | OPT_ESP_RADAR : g_cfg.reserved & ~OPT_ESP_RADAR;
        g_cfg_dirty = true;
    }
    ImGui::Dummy(ImVec2(0,8));
    FlagCheck("Bounding box",F_ESP_BOX);
    FlagCheck("Skeleton",F_ESP_SKELETON);
    FlagCheck("Snapline",F_ESP_LINE);
    int line_origin = int(g_cfg.esp_style & ESP_LINE_ORIGIN_MASK);
    if (line_origin > 2) line_origin = 0;
    ImGui::TextUnformatted("Line origin");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##line_origin",&line_origin,"Bottom\0Top\0Mid\0")) {
        g_cfg.esp_style = (g_cfg.esp_style & ~ESP_LINE_ORIGIN_MASK) | uint32_t(line_origin);
        g_cfg_dirty = true;
    }
    bool arrows = (g_cfg.esp_style & ESP_OFFSCREEN_ARROW) != 0;
    if (CometUi::Toggle("Offscreen arrows",&arrows,ImGui::GetContentRegionAvail().x)) {
        g_cfg.esp_style = arrows ? g_cfg.esp_style | ESP_OFFSCREEN_ARROW : g_cfg.esp_style & ~ESP_OFFSCREEN_ARROW;
        g_cfg_dirty = true;
    }
    FlagCheck("Player name",F_ESP_NAME);
    FlagCheck("Weapon name",F_ESP_GUN);
    FlagCheck("Distance",F_ESP_DISTANCE);
    FlagCheck("Health bar",F_ESP_HEALTH);
    FlagCheck("Armor bar",F_ESP_ARMOR);
    FlagCheck("Head marker",F_ESP_HEADDOT);
    FlagCheck("Enemy counter",F_ESP_COUNT);
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("visuals_extras",ImVec2(0,0));
    Section("ESP extras");
    bool hero = HasOpt2(OPT2_ESP_HERO);
    if (CometUi::Toggle("Enemy hero name",&hero,ImGui::GetContentRegionAvail().x)) SetOpt2(OPT2_ESP_HERO,hero);
    bool team_label = HasOpt2(OPT2_TEAM_LABEL);
    if (CometUi::Toggle("Team label",&team_label,ImGui::GetContentRegionAvail().x)) SetOpt2(OPT2_TEAM_LABEL,team_label);
    bool team_colors = HasOpt2(OPT2_TEAM_COLORS);
    if (CometUi::Toggle("Team box + line colors",&team_colors,ImGui::GetContentRegionAvail().x)) SetOpt2(OPT2_TEAM_COLORS,team_colors);
    bool loot = HasOpt2(OPT2_ESP_LOOT);
    if (CometUi::Toggle("Loot ESP",&loot,ImGui::GetContentRegionAvail().x)) SetOpt2(OPT2_ESP_LOOT,loot);
    ImGui::BeginDisabled(!HasOpt2(OPT2_ESP_LOOT));
    SliderF32("Loot distance",g_cfg.loot_max_dist,10.f,500.f,"%.0f m");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Team colours are assigned per team, automatically.");
    ImGui::TextDisabled("Armor bar colour follows max armor: 50 / 75 / 100 / 125.");
    ImGui::TextDisabled("Armour colour is automatic; there is no colour picker.");
    EndCard();
    ImGui::TableNextColumn();
    BeginCard("visuals_filters",ImVec2(0,0));
    Section("Target filters");
    FlagCheck("Show teammates",F_ESP_TEAM);
    FlagCheck("Visibility check",F_ESP_VISIBLE);
    SliderI32("Maximum distance",g_cfg.esp_max_dist,50,2000);
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("visuals_style",ImVec2(0,0));
    Section("Overlay style");
    FlagCheck("Corner boxes",F_ESP_CORNER);
    FlagCheck("Box outline",F_ESP_OUTLINE);
    FlagCheck("Box fill",F_ESP_FILLED);
    FlagCheck("Health gradient",F_ESP_GRADIENT);
    FlagCheck("Text shadow",F_ESP_SHADOW);
    SliderF32("Line thickness",g_cfg.box_thickness,0.5f,5.f,"%.1f px");
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("visuals_colors",ImVec2(0,0));
    if (ImGui::CollapsingHeader("Overlay colors")) {
        ColorRow("Box visible",g_cfg.col_box); ColorRow("Box hidden",g_cfg.col_box_hidden);
        ColorRow("Skeleton",g_cfg.col_skel); ColorRow("Snapline",g_cfg.col_line);
        ColorRow("Player name",g_cfg.col_name); ColorRow("Weapon name",g_cfg.col_gun);
        ColorRow("Distance",g_cfg.col_dist); ColorRow("Health",g_cfg.col_health);
        ColorRow("Head marker",g_cfg.col_headdot);
        ColorRow("Teammates",g_cfg.col_team); ColorRow("Bots",g_cfg.col_monster);
        ColorRow("Counter",g_cfg.col_count);
    if (ImGui::Button("Reset colors", ImVec2(150, 26))) {

        g_cfg.col_box = IM_COL32(96, 235, 155, 240);

        g_cfg.col_box_hidden = IM_COL32(240, 90, 90, 235);

        g_cfg.col_team = IM_COL32(90, 170, 255, 220);

        g_cfg.col_monster = IM_COL32(255, 200, 80, 235);

        g_cfg.col_skel = IM_COL32(96, 235, 155, 235);

        g_cfg.col_line = IM_COL32(89, 113, 162, 190);

        g_cfg.col_name = IM_COL32(236, 237, 241, 255);

        g_cfg.col_gun = IM_COL32(214, 216, 222, 235);

        g_cfg.col_dist = IM_COL32(200, 205, 215, 240);

        g_cfg.col_health = IM_COL32(96, 235, 155, 240);

        g_cfg.col_headdot = IM_COL32(255, 255, 255, 235);

        g_cfg.col_count = IM_COL32(96, 235, 155, 255);

        g_cfg_dirty = true;

    }

    }
    EndCard();
    ImGui::EndTable();
}

static void DrawAimTab() {
    if (!ImGui::BeginTable("aim_columns",2,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_NoPadOuterX)) return;
    ImGui::TableNextColumn();
    BeginCard("aim_targeting",ImVec2(0,0));
    Section("Targeting");
    FlagCheck("Enable aim",F_AIM);
    int aim_mode=(g_cfg.reserved&OPT_AIM_SILENT)?1:0;
    ImGui::TextUnformatted("Aim mode");ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo("##aim_mode",&aim_mode,"Aimbot\0Silent Aim\0")) {
        g_cfg.reserved=aim_mode?g_cfg.reserved|OPT_AIM_SILENT:g_cfg.reserved&~OPT_AIM_SILENT;
        g_cfg_dirty=true;
    }
    ImGui::Dummy(ImVec2(0,8));
    SliderF32("Field of view",g_cfg.aim_fov,1.f,180.f,"%.1f deg");
    SliderF32("Maximum distance",g_cfg.aim_max_dist,10.f,2000.f,"%.0f m");
    const char* bone_names[]={"Head","Neck","Body"};
    const uint32_t bone_flags[]={OPT_AIM_HEAD,OPT_AIM_NECK,OPT_AIM_BODY};
    uint32_t bone_mask=g_cfg.reserved&OPT_AIM_BONE_MASK;
    if(!bone_mask) bone_mask=OPT_AIM_HEAD;
    std::string selected;
    for(int i=0;i<3;++i) if(bone_mask&bone_flags[i]) {
        if(!selected.empty()) selected+=" + "; selected+=bone_names[i];
    }
    ImGui::TextUnformatted("Aim points");
    ImGui::SetNextItemWidth(-1);
    if(ImGui::BeginCombo("##aim_points",selected.c_str())) {
        for(int i=0;i<3;++i) {
            const bool on=(bone_mask&bone_flags[i])!=0;
            if(ImGui::Selectable(bone_names[i],on,ImGuiSelectableFlags_NoAutoClosePopups)) {
                const uint32_t next=bone_mask^bone_flags[i];
                if(next) {
                    bone_mask=next;
                    g_cfg.reserved=(g_cfg.reserved&~OPT_AIM_BONE_MASK)|OPT_AIM_BONES_SET|next;
                    g_cfg.flags&=~F_AIM_MULTIPOINT; g_cfg_dirty=true;
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0,8));
    FlagCheck("Visible targets only",F_AIM_VISIBLE);
    FlagCheck("Allow teammates",F_AIM_TEAM);
    bool skip=(g_cfg.reserved&OPT_AIM_IGNORE_KNOCKED)!=0;
    if(CometUi::Toggle("Ignore knocked targets",&skip,ImGui::GetContentRegionAvail().x)) {
        if(skip) g_cfg.reserved|=OPT_AIM_IGNORE_KNOCKED; else g_cfg.reserved&=~OPT_AIM_IGNORE_KNOCKED;
        g_cfg_dirty=true;
    }
    FlagCheck("Show FOV circle",F_AIM_FOV_CIRCLE);
    ColorRow("FOV color",g_cfg.col_fov);
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("aim_activation",ImVec2(0,0));
    Section("Activation");
    BindRow("Aim key",g_cfg.aim_key,1);
    // Optional second key: either one activates the aimbot, so a user can keep
    // the mouse bind and add a keyboard bind (or the other way round).
    BindRow("Aim key 2",g_cfg.aim_key2,2);
    if (g_cfg.aim_key2 != 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##aim2")) { g_cfg.aim_key2 = 0; g_cfg_dirty = true; }
    } else {
        ImGui::TextDisabled("Unbound - either key activates the aimbot.");
    }
    bool toggle=OptAimToggle();
    if(CometUi::Toggle("Toggle activation",&toggle,ImGui::GetContentRegionAvail().x)) SetOptAimToggle(toggle);
    if(g_bind_slot==1||g_bind_slot==2) ImGui::TextDisabled("Press a key to assign...");
    EndCard();
    ImGui::TableNextColumn();
    BeginCard("aim_tracking",ImVec2(0,0));
    const bool silent=(g_cfg.reserved&OPT_AIM_SILENT)!=0;
    Section(silent?"Shot targeting":"Tracking & motion");
    if(silent) ImGui::TextWrapped("Shot direction only. Camera stays unchanged.");
    else {
    SliderF32("Tracking smooth",g_cfg.aim_smooth,0.01f,1.f,"%.2f");
    FlagCheck("Humanize path",F_AIM_HUMANIZE);
    ImGui::BeginDisabled(!Has(F_AIM_HUMANIZE));
    SliderF32("Human smoothing",g_cfg.human_smooth,0.01f,1.f,"%.2f");
    ImGui::EndDisabled();
    }
    FlagCheck("Movement prediction",F_AIM_PREDICT);
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("aim_input",ImVec2(0,0));
    Section("Input");
    // Input noise is applied inside the aimbot's mouse path only. It never
    // reaches the trigger, the menu, or any other input the user sends.
    bool noise = HasOpt2(OPT2_AIM_NOISE);
    if (CometUi::Toggle("Input noise (aim only)",&noise,ImGui::GetContentRegionAvail().x)) SetOpt2(OPT2_AIM_NOISE,noise);
    ImGui::BeginDisabled(!HasOpt2(OPT2_AIM_NOISE));
    SliderF32("Noise amount",g_cfg.noise_strength,0.0002f,0.008f,"%.4f");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Input method: SendInput");
    ImGui::TextWrapped("Mouse corrections are sent with SendInput only. The noise offset is bounded per correction and fades out at the aim point.");
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("aim_recoil",ImVec2(0,0));
    Section("NO RECOIL");
    FlagCheck("Enable",F_RECOIL);
    Section("RCS");
    {
        bool rcs = g_cfg.humanize > 0.0f;
        if (CometUi::Toggle("Enable##rcs", &rcs, ImGui::GetContentRegionAvail().x)) {
            g_cfg.humanize = rcs ? 1.0f : 0.0f;
            g_cfg_dirty = true;
        }
    }
    EndCard();
    ImGui::EndTable();
}

static void DrawGunPane(const char* table_id, const std::vector<uint32_t>& ids, int& sel, int apply_type) {
    if (ids.empty()) { ImGui::TextDisabled("No skins"); return; }
    sel = std::clamp(sel, 0, (int)ids.size() - 1);
    if (!ImGui::BeginTable(table_id, 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("categories", ImGuiTableColumnFlags_WidthFixed, 175);
    ImGui::TableSetupColumn("skins", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextColumn();
    ImGui::BeginChild("categories", ImVec2(0, 0), ImGuiChildFlags_None);
    for (size_t i = 0; i < ids.size(); ++i) {
        auto found = g_gun_label.find(ids[i]);
        std::string label = found != g_gun_label.end() ? found->second : "Weapon";
        if (apply_type == 3) {
            switch (ids[i] / 100) { case 201: label = "Dagger & blades"; break; case 202: label = "Axe & claymore"; break;
            case 203: label = "Katana"; break; case 206: label = "Bat & club"; break; case 207: label = "Butterfly knife"; break; case 210: label = "Artifacts"; break; }
        } else {
            const auto split = label.find(" - "); if (split != std::string::npos) label.resize(split);
        }
        ImGui::PushID((int)i);
        if (ImGui::Selectable(label.c_str(), sel == (int)i, 0, ImVec2(0, 32))) sel = (int)i;
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::TableNextColumn();
    ImGui::BeginChild("skin_list", ImVec2(0, 0), ImGuiChildFlags_None);
    const bool artifact = apply_type == 3 && ids[(size_t)sel] / 100 == 210;
    auto found = g_gun_skins.find(ids[(size_t)sel]);
    std::vector<int> filtered_indices;
    if (found != g_gun_skins.end()) {
        filtered_indices.reserve(found->second.size());
        for (int index : found->second) {
            const auto& entry = g_guns[(size_t)index];
            const std::string name = NameOf(entry);
            if (ContainsNoCase(name, g_search)) {
                filtered_indices.push_back(index);
            }
        }
    }
    if (!filtered_indices.empty()) {
        ImGuiListClipper clipper;
        clipper.Begin((int)filtered_indices.size());
        while (clipper.Step()) {
            for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
                int index = filtered_indices[k];
                const auto& entry = g_guns[(size_t)index];
                const std::string name = NameOf(entry);
                ImGui::PushID(index);
                const float row_y = ImGui::GetCursorPosY(), width = ImGui::GetContentRegionAvail().x;
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(80.f, width - 84));
                ImGui::BeginGroup();
                const bool equipped = std::any_of(g_skin_inventory.begin(), g_skin_inventory.end(), [&](const CatEntry& saved) {
                    return saved.a == static_cast<uint32_t>(apply_type) && saved.c == entry.a;
                });
                ImGui::BeginDisabled(artifact);
                if (ImGui::Selectable(name.c_str(), equipped, 0, ImVec2(std::max(80.f, width - 84), 0)))
                    ApplyItem(apply_type, entry.a);
                ImGui::EndDisabled();
                ImGui::EndGroup();
                ImGui::PopTextWrapPos();
                const float text_end = ImGui::GetCursorPosY();
                ImGui::SetCursorPos(ImVec2(std::max(0.f, width - 70), row_y + 4));
                ImGui::BeginDisabled(artifact);
                if (ImGui::Button("Apply", ImVec2(66, 30))) ApplyItem(apply_type, entry.a);
                ImGui::EndDisabled();
                ImGui::SetCursorPosY(std::max(row_y + 56, text_end + 8));
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        clipper.End();
    } else {
        ImGui::TextDisabled("No results");
    }
    ImGui::EndChild();
    ImGui::EndTable();
}

static uint32_t g_skin_hero_cfg = 0;

static uint32_t g_skin_knife_cfg = 0;

static uint32_t g_skin_weapon_cfg = 0;

static int FindIdIndex(const std::vector<uint32_t>& ids, uint32_t item) {

    if (!item) return -1;

    for (size_t i = 0; i < ids.size(); ++i) {

        if (ids[i] == item) return (int)i;

    }

    return -1;

}

static int FindHeroSkinSel(uint32_t item) {

    if (!item) return -1;

    for (size_t i = 0; i < g_heroes.size(); ++i) {

        if (g_heroes[i].c == item) {

            int idx = FindIdIndex(g_hero_ids, g_heroes[i].a);

            if (idx >= 0) return idx;

        }

    }

    return -1;

}

static int FindGunSkinSel(const std::vector<uint32_t>& gunIds, uint32_t item) {

    if (!item) return -1;

    for (size_t gi = 0; gi < gunIds.size(); ++gi) {

        auto it = g_gun_skins.find(gunIds[gi]);

        if (it == g_gun_skins.end()) continue;

        for (int idx : it->second) {

            if (g_guns[(size_t)idx].a == item) return (int)gi;

        }

    }

    return -1;

}

static void SyncSkinSelections() {

    if (g_cfg.skin_hero_item && g_cfg.skin_hero_item != g_skin_hero_cfg) {

        int i = FindHeroSkinSel(g_cfg.skin_hero_item);

        if (i >= 0) g_hero_sel = i;

        g_skin_hero_cfg = g_cfg.skin_hero_item;

    }

    if (g_cfg.skin_knife_item && g_cfg.skin_knife_item != g_skin_knife_cfg) {

        int i = FindGunSkinSel(g_knife_ids, g_cfg.skin_knife_item);

        if (i >= 0) g_knife_sel = i;

        g_skin_knife_cfg = g_cfg.skin_knife_item;

    }

    if (g_cfg.skin_gun_item && g_cfg.skin_gun_item != g_skin_weapon_cfg) {

        int i = FindGunSkinSel(g_weapon_ids, g_cfg.skin_gun_item);

        if (i >= 0) g_weapon_sel = i;

        g_skin_weapon_cfg = g_cfg.skin_gun_item;

    }

}

static void DrawSkinInventory() {
    if (g_skin_inventory.empty()) {
        ImGui::TextDisabled("Empty");
        return;
    }
    int remove_index = -1;
    size_t visible = 0;
    const float height = std::max(120.f, ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginTable("inventorylist", 2, ImGuiTableFlags_SizingStretchProp |
                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH, ImVec2(0, height))) {
        ImGui::TableSetupColumn("Skin", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 74);
        for (size_t i = 0; i < g_skin_inventory.size(); ++i) {
            const CatEntry entry = g_skin_inventory[i];
            const std::string name = NameOf(entry);
            if (!ContainsNoCase(name, g_search)) continue;
            ++visible;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 60);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", name.c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!g_inventory_writable);
            if (ImGui::Button("Remove", ImVec2(68, 28))) remove_index = static_cast<int>(i);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (!visible) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("No results");
        }
        ImGui::EndTable();
    }
    // Defer mutations until no table row references the saved vector.
    if (remove_index >= 0) {
        const CatEntry removed = g_skin_inventory[static_cast<size_t>(remove_index)];
        auto entries = g_skin_inventory;
        entries.erase(entries.begin() + remove_index);
        if (SaveSkinInventory(entries)) {
            uint32_t* last = removed.a == 1 ? &g_cfg.skin_hero_item : removed.a == 2 ? &g_cfg.skin_gun_item : &g_cfg.skin_knife_item;
            if (*last == removed.c) { *last = 0; g_cfg_dirty = true; SaveCfg(); }
        }
    }
}


static void DrawSkinTab() {
    LoadSkinInventory();
    MigrateSkinInventory();

    if (!g_catalog_ready) {

        ImGui::TextDisabled("Catalog not loaded.");

        if (ImGui::Button("Load catalog")) LoadCatalog();

    }

    if (g_catalog_ready) SyncSkinSelections();

    uint32_t cur = g_state.local_hero_id;

    bool cur_known = cur != 0 && g_hero_skins.find(cur) != g_hero_skins.end();



    ImGui::SetNextItemWidth(std::max(180.f, ImGui::GetContentRegionAvail().x - 132.f));
    ImGui::InputTextWithHint("##search", "Search skin name", g_search, sizeof(g_search));
    ImGui::SameLine();
    if (ImGui::Button("Reload catalog", ImVec2(122, 0))) LoadCatalog();
    bool only = Has(F_SKIN_CURRENT);
    if (CometUi::Toggle("Current hero only", &only)) SetF(F_SKIN_CURRENT, only);
    ImGui::SameLine();
    bool auto_apply = g_cfg.auto_skin != 0;
    if (CometUi::Toggle("Auto apply saved skins", &auto_apply)) { g_cfg.auto_skin = auto_apply ? 1u : 0u; g_cfg_dirty = true; SaveCfg(); }
    if (!g_inventory_error.empty()) {
        ImGui::TextWrapped("%s", g_inventory_error.c_str());
        if (ImGui::Button("Retry Inventory load")) LoadSkinInventory(true);
    }
    ImGui::Spacing();

    std::vector<uint32_t> ids;

    ids.reserve(g_hero_ids.size());

    for (uint32_t id : g_hero_ids) {

        if (only && cur_known && id != cur) continue;

        ids.push_back(id);

    }

    if (only && cur_known && ids.empty()) ids.push_back(cur);

    if (ids.empty()) ids = g_hero_ids;



    if (g_hero_sel < 0) g_hero_sel = 0;

    if (!ids.empty() && (size_t)g_hero_sel >= ids.size()) g_hero_sel = 0;

    if (only && cur_known) {

        for (size_t i = 0; i < ids.size(); ++i) {

            if (ids[i] == cur) { g_hero_sel = (int)i; break; }

        }

    }



    static int skin_page = 0;
    const char* skin_tabs[] = {"Characters", "Knives", "Guns", "Inventory"};
    const float tab_width = (ImGui::GetContentRegionAvail().x - 24.f) / 4.f;
    for (int i=0; i<4; ++i) {
        if(i) ImGui::SameLine(0,8);
        if(TabButton(skin_tabs[i],skin_page==i,tab_width)) skin_page=i;
    }
    ImGui::Dummy(ImVec2(0,12));
    {

        if (skin_page == 0) {

            if (only && !cur_known) {

                ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.25f, 1.0f),

                                   "Hero unavailable");

            }

            if (ImGui::BeginTable("herotbl", 2, ImGuiTableFlags_SizingStretchProp)) {

                ImGui::TableSetupColumn("heroes", ImGuiTableColumnFlags_WidthFixed, 175.0f);

                ImGui::TableSetupColumn("skins", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();

                ImGui::TableNextColumn();

                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.075f, .075f, .085f, 1.f));

                ImGui::BeginChild("herolist", ImVec2(0, 0), ImGuiChildFlags_None);

                ImGui::PopStyleColor();

                std::vector<int> filtered_heroes;

                filtered_heroes.reserve(ids.size());

                for (size_t i = 0; i < ids.size(); ++i) {

                    uint32_t id = ids[i];

                    auto it = g_hero_skins.find(id);

                    const CatEntry* first = nullptr;

                    if (it != g_hero_skins.end() && !it->second.empty())

                        first = &g_heroes[(size_t)it->second[0]];

                    std::string label = HeroLabel(id, first);

                    if (ContainsNoCase(label, g_search)) {

                        filtered_heroes.push_back((int)i);

                    }

                }

                ImGuiListClipper h_clipper;

                h_clipper.Begin((int)filtered_heroes.size());

                while (h_clipper.Step()) {

                    for (int k = h_clipper.DisplayStart; k < h_clipper.DisplayEnd; ++k) {

                        int i = filtered_heroes[k];

                        uint32_t id = ids[(size_t)i];

                        auto it = g_hero_skins.find(id);

                        const CatEntry* first = nullptr;

                        if (it != g_hero_skins.end() && !it->second.empty())

                            first = &g_heroes[(size_t)it->second[0]];

                        std::string label = HeroLabel(id, first);

                        char full[160];

                        snprintf(full, sizeof(full), "%s##h%u", label.c_str(), id);

                        ImGui::PushID(i);

                        if (ImGui::Selectable(full, i == g_hero_sel, 0, ImVec2(0, 22))) g_hero_sel = i;

                        ImGui::PopID();

                    }

                }

                h_clipper.End();

                ImGui::EndChild();



                ImGui::TableNextColumn();

                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.075f, .075f, .085f, 1.f));

                ImGui::BeginChild("heroskins", ImVec2(0, 0), ImGuiChildFlags_None);

                ImGui::PopStyleColor();

                uint32_t sel_id = 0;

                if (g_hero_sel >= 0 && (size_t)g_hero_sel < ids.size()) sel_id = ids[(size_t)g_hero_sel];

                std::vector<int> filtered_hskins;

                if (sel_id) {

                    auto itmap = g_hero_skins.find(sel_id);

                    if (itmap != g_hero_skins.end()) {

                        for (int idx : itmap->second) {

                            const auto& e = g_heroes[(size_t)idx];

                            std::string name = NameOf(e);

                            if (name.empty()) name = "Default skin";

                            if (ContainsNoCase(name, g_search)) {

                                filtered_hskins.push_back(idx);

                            }

                        }

                    }

                }

                if (!filtered_hskins.empty()) {

                    ImGuiListClipper hs_clipper;

                    hs_clipper.Begin((int)filtered_hskins.size());

                    while (hs_clipper.Step()) {

                        for (int k = hs_clipper.DisplayStart; k < hs_clipper.DisplayEnd; ++k) {

                            int idx = filtered_hskins[k];

                            const auto& e = g_heroes[(size_t)idx];

                            std::string name = NameOf(e);

                            if (name.empty()) name = "Default skin";

                            ImGui::PushID(idx);

                            const bool equipped = std::any_of(g_skin_inventory.begin(), g_skin_inventory.end(), [&](const CatEntry& saved) {
                                return saved.a == 1 && saved.c == e.c;
                            });
                            const float skin_width = std::max(80.f, ImGui::GetContentRegionAvail().x - 70.f);
                            if (ImGui::Selectable(name.c_str(), equipped, 0, ImVec2(skin_width, 0))) ApplyItem(1, e.c);

                            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 62.0f);

                            if (ImGui::Button("Apply", ImVec2(58, 0))) ApplyItem(1, e.c);

                            ImGui::PopID();

                        }

                    }

                    hs_clipper.End();

                } else if (sel_id) {

                    ImGui::TextDisabled("No results");

                }

                uint32_t restore_model = 0;

                auto itdef = g_hero_skins.find(sel_id);

                if (itdef != g_hero_skins.end()) {

                    for (int idx : itdef->second) {

                        const auto& e = g_heroes[(size_t)idx];

                        if (e.b == 0) { restore_model = e.c; break; }

                    }

                    if (!restore_model && !itdef->second.empty())

                        restore_model = g_heroes[(size_t)itdef->second[0]].c;

                }

                if (restore_model) {

                    if (ImGui::Button("Restore default model", ImVec2(180, 0))) ApplyItem(1, restore_model);

                }

                ImGui::EndChild();

                ImGui::EndTable();

            }



        }

        if (skin_page == 1) {

            DrawGunPane("knifetbl", g_knife_ids, g_knife_sel, 3);



        }

        if (skin_page == 2) {

            DrawGunPane("weapontbl", g_weapon_ids, g_weapon_sel, 2);



        }

        if (skin_page == 3) {
            DrawSkinInventory();

        }



    }

}



static DWORD WINAPI UnloadWorker(LPVOID) {

    Sleep(100);

    comet_capture::Restore(g_hwnd, g_obs);

    if (g_hwnd && g_wndproc) {

        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_wndproc);

        g_wndproc = nullptr;

    }

    MH_DisableHook(MH_ALL_HOOKS);

    MH_Uninitialize();

    if (g_imgui_ready) {

        ImGui_ImplDX11_Shutdown();

        ImGui_ImplWin32_Shutdown();

        ImGui::DestroyContext();

        g_imgui_ready = false;

    }

    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }

    if (g_context) { g_context->Release(); g_context = nullptr; }

    if (g_device) { g_device->Release(); g_device = nullptr; }

    if (g_shm_state_p) { UnmapViewOfFile(g_shm_state_p); g_shm_state_p = nullptr; }

    if (g_shm_state_h) { CloseHandle(g_shm_state_h); g_shm_state_h = nullptr; }

    if (g_shm_cfg_p) { UnmapViewOfFile(g_shm_cfg_p); g_shm_cfg_p = nullptr; }

    if (g_shm_cfg_h) { CloseHandle(g_shm_cfg_h); g_shm_cfg_h = nullptr; }



    SetF(F_EXIT_CHEAT, true);

    SaveCfg();



    Log("overlay unloaded cleanly");

    Sleep(200);

    FreeLibraryAndExitThread(g_hmodule, 0);

    return 0;

}



static void SafeExitCheatAndGame() {
    g_unloading=true;
    comet_capture::Restore(g_hwnd, g_obs);
    if (g_hwnd && g_wndproc) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_wndproc);
        g_wndproc = nullptr;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_imgui_ready) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imgui_ready = false;
    }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }

    if (g_shm_state_p) { UnmapViewOfFile(g_shm_state_p); g_shm_state_p = nullptr; }
    if (g_shm_state_h) { CloseHandle(g_shm_state_h); g_shm_state_h = nullptr; }
    if (g_shm_cfg_p) { UnmapViewOfFile(g_shm_cfg_p); g_shm_cfg_p = nullptr; }
    if (g_shm_cfg_h) { CloseHandle(g_shm_cfg_h); g_shm_cfg_h = nullptr; }

    std::vector<std::wstring> files = {
        StatePath(), CfgPath(), CatalogPath(),
        IpcDir() + L"\\runtime_err.txt",
        LogPath(),
        PathBase() + L"\\pyexec\\out.txt",
        PathBase() + L"\\pyexec\\booted.txt"
    };
    for (const auto& fpath : files) {
        DeleteFileW(fpath.c_str());
    }

    TerminateProcess(GetCurrentProcess(), 0);
}

static void UnloadOverlay() {

    if (g_unloading) return;

    g_unloading = true;

    HANDLE t = CreateThread(nullptr, 0, UnloadWorker, nullptr, 0, nullptr);

    if (t) CloseHandle(t);

}



static void DrawSettingsTab() {
    if(!ImGui::BeginTable("settings_columns",2,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_NoPadOuterX)) return;
    ImGui::TableNextColumn();
    BeginCard("settings_general",ImVec2(0,0));
    Section("Configuration");
    BindRow("Menu key",g_cfg.menu_key,3);
    if(g_bind_slot==3) ImGui::TextDisabled("Press a key to assign...");
    ImGui::Dummy(ImVec2(0,12));
    if(ImGui::Button("Save configuration",ImVec2(-1,34))) SaveConfiguration();
    if(!g_cfg_error.empty()) ImGui::TextWrapped("%s",g_cfg_error.c_str());
    ImGui::Dummy(ImVec2(0,12));
    if(ImGui::Button("Reset to defaults",ImVec2(-1,32))) ResetCfg();
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("settings_appearance",ImVec2(0,0));
    Section("Appearance");
    const char* themes[]={"Purple","Crimson","Cyan","Emerald","Mono"};
    int theme=g_cfg.pad[0]==CFG_UI_TAG&&g_cfg.pad[1]<=4?static_cast<int>(g_cfg.pad[1]):2;
    ImGui::TextUnformatted("Accent color"); ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo("##accent",&theme,themes,5)) {
        g_cfg.pad[0]=CFG_UI_TAG;g_cfg.pad[1]=theme;g_cfg_dirty=true;ApplyMenuPreferences();
    }
    bool reduced=g_cfg.pad[0]==CFG_UI_TAG&&g_cfg.pad[2]!=0;
    if(CometUi::Toggle("Reduced motion",&reduced,ImGui::GetContentRegionAvail().x)) {
        g_cfg.pad[0]=CFG_UI_TAG;g_cfg.pad[1]=theme;g_cfg.pad[2]=reduced?1u:0u;
        g_cfg_dirty=true;ApplyMenuPreferences();
    }
    EndCard();
    ImGui::TableNextColumn();
    BeginCard("settings_protection",ImVec2(0,0));
    Section("Protection");
    bool obs = HasOpt2(OPT2_OBS_PROTECT);
    if (CometUi::Toggle("OBS / screenshot blackout",&obs,ImGui::GetContentRegionAvail().x)) {
        SetOpt2(OPT2_OBS_PROTECT,obs);
        // Re-apply immediately so the toggle is honest in the same frame.
        g_obs = comet_capture::Blackout{};
        ApplyObsProtection();
    }
    ImGui::TextDisabled("%s",comet_capture::Describe(g_obs));
    if (g_obs.status==comet_capture::Status::Denied)
        ImGui::TextDisabled("SetWindowDisplayAffinity failed (%lu); the last affinity is unchanged.",g_obs.error);
    ImGui::TextWrapped("Game window excludes itself from Windows capture paths. Best effort: hardware capture or recorders that ignore display affinity still see the screen.");
    FlagCheck("Block reports",F_BLOCK_REPORT);
    FlagCheck("Unlock effects",F_UNLOCK_EFFECTS);
    FlagCheck("Force killfx",F_FORCE_KILLFX);
    EndCard();
    ImGui::Dummy(ImVec2(0,12));
    BeginCard("settings_session",ImVec2(0,0));
    Section("Session controls");
    if(ImGui::Button("Unload overlay",ImVec2(-1,34))) UnloadOverlay();
    ImGui::Dummy(ImVec2(0,8));
    ImGui::PushStyleColor(ImGuiCol_Text,ImGui::ColorConvertU32ToFloat4(CometUi::Danger));
    if(ImGui::Button("Safe exit",ImVec2(-1,34))) SafeExitCheatAndGame();
    ImGui::PopStyleColor();
    EndCard();
    ImGui::EndTable();
}

static void DrawMenu() {
    if (!Has(F_MENU_OPEN)) return;
    // Catalog hot-reload. This is a synchronous file read plus a rebuild of the
    // skin index, so it runs on a 2 s timer and only when the file really
    // changed - not once per frame while the menu is open.
    static ULONGLONG next_catalog_check = 0;
    static FILETIME last_catalog_write{};
    const ULONGLONG now_tick = GetTickCount64();
    if (now_tick >= next_catalog_check) {
        next_catalog_check = now_tick + 2000;
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (GetFileAttributesExW(CatalogPath().c_str(), GetFileExInfoStandard, &attr) &&
            (!g_catalog_ready || CompareFileTime(&last_catalog_write, &attr.ftLastWriteTime) != 0)) {
            LoadCatalog();
            last_catalog_write = attr.ftLastWriteTime;
        }
    }
    ProcessBind();
    auto& io = ImGui::GetIO();
    // Focus gate: dim the game behind the menu.
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 140));
    const float max_w = std::max(640.f, io.DisplaySize.x - 24.f);
    const float max_h = std::max(420.f, io.DisplaySize.y - 24.f);
    ImGui::SetNextWindowSize(ImVec2(std::min(960.f, max_w), std::min(680.f, max_h)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(820.f, max_w), std::min(560.f, max_h)), ImVec2(max_w, max_h));
    if (g_reset_menu_pos) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .5f), ImGuiCond_Always, ImVec2(.5f, .5f));
        g_reset_menu_pos = false;
    } else ImGui::SetNextWindowPos(ImVec2(60, 30), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##comet_main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    CometUi::Shadow(ImGui::GetBackgroundDrawList(),wp,ws);
    constexpr float rail = 188;
    dl->AddRectFilled(wp, ImVec2(wp.x + rail, wp.y + ws.y), CometUi::Ground, 12, ImDrawFlags_RoundCornersLeft);
    dl->AddLine(ImVec2(wp.x + rail, wp.y), ImVec2(wp.x + rail, wp.y + ws.y), CometUi::Border);
    CometUi::Logo(dl,ImVec2(wp.x+22,wp.y+26),30);
    if(g_font_big) dl->AddText(g_font_big,20,ImVec2(wp.x+64,wp.y+24),CometUi::Text,"Comet");
    else dl->AddText(ImVec2(wp.x+64,wp.y+24),CometUi::Text,"Comet");
    ImGui::SetCursorPos(ImVec2(12,100));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 6));
    ImGui::BeginChild("navigation", ImVec2(rail - 24, ws.y - 120), ImGuiChildFlags_None);
    const char* labels[] = {"Visuals", "Aim", "Skins", "Settings"};
    const CometUi::Icon icons[] = {CometUi::Icon::Eye, CometUi::Icon::Target, CometUi::Icon::Skins, CometUi::Icon::Settings};
    CometUi::NavIndicator(g_tab, rail - 24);
    for (int i = 0; i < 4; ++i) {
        if (CometUi::NavItem(labels[i], icons[i], g_tab == i, rail - 24)) g_tab = i;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::SetCursorPos(ImVec2(rail+26,22));
    if(g_font_big) ImGui::PushFont(g_font_big,22);
    ImGui::TextUnformatted(labels[std::clamp(g_tab,0,3)]);
    if(g_font_big) ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(ws.x-178,26));
    if(ImGui::Button("Save config",ImVec2(112,32))) SaveConfiguration();
    ImGui::SetCursorPos(ImVec2(ws.x-46,28));
    if(CometUi::CloseButton("##close",ImVec2(28,28))) SetF(F_MENU_OPEN,false);
    dl->AddLine(ImVec2(wp.x+rail+26,wp.y+70),ImVec2(wp.x+ws.x-26,wp.y+70),CometUi::Border);
    ImGui::SetCursorPos(ImVec2(rail+26,86));
    ImGui::PushID(g_tab);
    ImGui::BeginChild("content", ImVec2(ws.x - rail - 52, ws.y - 104), ImGuiChildFlags_None);
    switch (g_tab) { case 0: DrawESPTab(); break; case 1: DrawAimTab(); break;
                    case 2: DrawSkinTab(); break; default: DrawSettingsTab(); break; }
    ImGui::EndChild(); ImGui::PopID(); ImGui::End(); ImGui::PopStyleVar();
}



static void HandleToggle() {

    if (!g_cfg_ready) return;

    static bool prev = false;

    int key = g_cfg.menu_key;

    if (key < 0 || key > 255) key = VK_INSERT;

    bool down = (RawAsyncKeyState(key) & 0x8000) != 0;

    if (down && !prev) {

        SetF(F_MENU_OPEN, !Has(F_MENU_OPEN));

    }

    prev = down;

}



static void CreateRTV() {

    if (!g_swapchain || !g_device) return;

    if (g_rtv) {

        g_rtv->Release();

        g_rtv = nullptr;

    }

    ID3D11Texture2D* back = nullptr;

    if (FAILED(g_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;

    D3D11_TEXTURE2D_DESC td{};

    back->GetDesc(&td);

    if (td.Width > 0 && td.Height > 0) {

        g_width = td.Width;

        g_height = td.Height;

    }

    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);

    back->Release();

}



static bool BindSwapchain(IDXGISwapChain* sc) {

    if (!sc) return false;

    if (sc == g_swapchain) return true;

    // Rejection cache. Present runs at the refresh rate; a swapchain we already
    // refused (another window, another device) must not be re-interrogated, and
    // above all must not log: Log() opens, writes and closes the log file, which
    // is a synchronous disk write inside Present. Re-logging every present of a
    // non-selected swapchain was a direct source of frame drops.
    static IDXGISwapChain* rejected[8]{};
    static int rejected_count = 0;
    for (int i = 0; i < rejected_count; ++i) {

        if (rejected[i] == sc) return false;

    }

    auto reject = [&]() {

        if (rejected_count < (int)(sizeof(rejected) / sizeof(rejected[0]))) rejected[rejected_count++] = sc;

        return false;

    };

    DXGI_SWAP_CHAIN_DESC desc{};

    if (FAILED(sc->GetDesc(&desc))) return reject();

    if (g_hwnd && desc.OutputWindow && desc.OutputWindow != g_hwnd) {

        Log("swapchain skip other hwnd=%p", desc.OutputWindow);

        return reject();

    }

    ID3D11Device* dev = nullptr;

    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) {

        Log("swapchain skip no device %p", sc);

        return reject();

    }

    bool same_device = (dev == g_device);

    dev->Release();

    if (!same_device) {

        Log("swapchain skip other device %p", sc);

        return reject();

    }

    if (g_rtv) {

        g_rtv->Release();

        g_rtv = nullptr;

    }

    g_swapchain = sc;

    Log("swapchain rebind %p", sc);

    return true;

}



static void UpdateClientSize() {

    if (!g_hwnd) return;

    RECT rc{};

    if (GetClientRect(g_hwnd, &rc)) {

        UINT w = (UINT)std::max<LONG>(0, rc.right - rc.left);

        UINT h = (UINT)std::max<LONG>(0, rc.bottom - rc.top);

        if (w > 0 && h > 0) {

            g_client_w = w;

            g_client_h = h;

            return;

        }

    }

    if (g_width > 0 && g_height > 0) {

        g_client_w = g_width;

        g_client_h = g_height;

    }

}



static void InitFonts() {

    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->AddFontDefault();
    char windows_directory[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryA(windows_directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    const std::string font_root = std::string(windows_directory) + "\\Fonts\\";

    const char* fonts[] = {

        "segoeui.ttf",
        "msyh.ttc",

        "msyhbd.ttc",

        "simhei.ttf",

    };

    for (const char* file : fonts) {
        const std::string filename = font_root + file;
        const char* path = filename.c_str();

        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;

        ImFontConfig cfg;

        cfg.OversampleH = 2;

        cfg.OversampleV = 2;

        cfg.PixelSnapH = true;

        ImFont* f = io.Fonts->AddFontFromFileTTF(path, 16.0f, &cfg, io.Fonts->GetGlyphRangesChineseFull());

        if (f) {

            io.FontDefault = f;

            break;

        }

    }

    const char* bold[] = { "seguisb.ttf", "msyhbd.ttc", "msyh.ttc", "simhei.ttf" };

    for (const char* file : bold) {
        const std::string filename = font_root + file;
        const char* path = filename.c_str();

        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;

        ImFontConfig cfg;

        cfg.OversampleH = 2;

        cfg.OversampleV = 2;

        cfg.PixelSnapH = true;

        ImFont* f = io.Fonts->AddFontFromFileTTF(path, 21.0f, &cfg, io.Fonts->GetGlyphRangesChineseFull());

        if (f) {

            g_font_big = f;

            break;

        }

    }

    const char* esp_fonts[] = { "seguisb.ttf", "segoeuib.ttf", "msyhbd.ttc", "msyh.ttc", "simhei.ttf" };

    for (const char* file : esp_fonts) {
        const std::string filename = font_root + file;
        const char* path = filename.c_str();

        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;

        ImFontConfig cfg;

        cfg.OversampleH = 2;

        cfg.OversampleV = 2;

        cfg.PixelSnapH = true;

        ImFont* f = io.Fonts->AddFontFromFileTTF(path, 18.0f, &cfg, io.Fonts->GetGlyphRangesChineseFull());

        if (f) {

            g_font_esp = f;

            break;

        }

    }

}



static SHORT RawAsyncKeyState(int vk) {return GetAsyncKeyState(vk);}

static LRESULT CALLBACK HookWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {

    if(m==MenuInputCloseMessage()) {
        if(g_menu_owns_capture&&GetCapture()==h) ReleaseCapture();
        g_menu_owns_capture=false;
        return 0;
    }
    if (g_unloading) return CallWindowProcW(g_wndproc, h, m, w, l);

    if (m == WM_KEYDOWN && w == VK_ESCAPE && Has(F_MENU_OPEN)) {

        SetF(F_MENU_OPEN, false);

        return 0;

    }

    if (m == WM_SIZE || m == WM_DISPLAYCHANGE || m == WM_DPICHANGED) {

        g_display_dirty = true;

        g_reset_menu_pos = true;

    }


    // A close-button press can close the menu before its release arrives.
    if(!Has(F_MENU_OPEN)&&g_imgui_ready&&(m==WM_LBUTTONUP||m==WM_RBUTTONUP||m==WM_MBUTTONUP||
       m==WM_XBUTTONUP||m==WM_CAPTURECHANGED||m==WM_KILLFOCUS||m==WM_KEYUP||m==WM_SYSKEYUP))
        ImGui_ImplWin32_WndProcHandler(h,m,w,l);

    if (Has(F_MENU_OPEN)) {

        if (g_bind_slot && g_bind_armed) {

            if (m == WM_LBUTTONDOWN) { CaptureBind(VK_LBUTTON); return 0; }

            if (m == WM_RBUTTONDOWN) { CaptureBind(VK_RBUTTON); return 0; }

            if (m == WM_MBUTTONDOWN) { CaptureBind(VK_MBUTTON); return 0; }

            if (m == WM_XBUTTONDOWN) { CaptureBind(HIWORD(w) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2); return 0; }

        }

        const HWND previous_capture=GetCapture();
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        if(!previous_capture&&GetCapture()==h) g_menu_owns_capture=true;
        const auto& io=ImGui::GetIO();
        const bool editing=io.WantTextInput||(g_bind_slot&&g_bind_armed);

        // Raw mouse and keyboard share WM_INPUT; menu visibility alone must not consume keyboard.
        if(m==WM_INPUT) {
            RAWINPUTHEADER header{};UINT bytes=sizeof(header);
            if(GetRawInputData(reinterpret_cast<HRAWINPUT>(l),RID_HEADER,&header,&bytes,sizeof(header))==sizeof(header)) {
                if(header.dwType==RIM_TYPEMOUSE)
                    return DefWindowProcW(h,m,w,l);
                if(header.dwType==RIM_TYPEKEYBOARD&&editing) {
                    RAWINPUT raw{};bytes=sizeof(raw);
                    if(GetRawInputData(reinterpret_cast<HRAWINPUT>(l),RID_INPUT,&raw,&bytes,sizeof(header))!=UINT(-1)&&
                       !(raw.data.keyboard.Flags&RI_KEY_BREAK)) return DefWindowProcW(h,m,w,l);
                }
            }
            return CallWindowProcW(g_wndproc,h,m,w,l);
        }
        if(m==WM_KEYUP||m==WM_SYSKEYUP) return CallWindowProcW(g_wndproc,h,m,w,l);
        if(m==WM_KEYDOWN||m==WM_SYSKEYDOWN||m==WM_CHAR||m==WM_SYSCHAR) {
            if(editing) return 0;
            return CallWindowProcW(g_wndproc,h,m,w,l);
        }

        switch (m) {

            case WM_MOUSEMOVE:

            case WM_LBUTTONDOWN:

            case WM_LBUTTONUP:

            case WM_RBUTTONDOWN:

            case WM_RBUTTONUP:

            case WM_MBUTTONDOWN:

            case WM_MBUTTONUP:

            case WM_MOUSEWHEEL:

            case WM_XBUTTONDOWN:

            case WM_XBUTTONUP:

                return 0;

            case WM_SETCURSOR:

                SetCursor(nullptr);

                return TRUE;

            default:

                break;

        }

    }

    return CallWindowProcW(g_wndproc, h, m, w, l);

}



static void EnsureWndProc() {
    if(!g_hwnd) return;
    WNDPROC current=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_hwnd,GWLP_WNDPROC));
    if(!Has(F_MENU_OPEN)||g_unloading) {
        if(g_input_attached&&current==HookWndProc) {
            DWORD_PTR result=0;
            SendMessageTimeoutW(g_hwnd,MenuInputCloseMessage(),0,0,SMTO_ABORTIFHUNG,1000,&result);
            if(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_hwnd,GWLP_WNDPROC))==HookWndProc) {
                SetWindowLongPtrW(g_hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(g_wndproc));
                g_input_attached=false;
                Log("menu closed: native game input restored flags=%08x",g_cfg.flags);
            }
        }
        return;
    }
    // If another overlay chained this handler, do not install above it again.
    if(g_input_attached) return;
    if(current!=HookWndProc) {
        g_wndproc=current;
        SetWindowLongPtrW(g_hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(HookWndProc));
    }
    g_input_attached=true;
    Log("menu opened: input handler attached");
}

static bool InitOverlay(IDXGISwapChain* sc) {

    if (g_imgui_ready) return true;

    if (g_init_failed) return false;

    DXGI_SWAP_CHAIN_DESC desc{};

    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) {

        g_init_failed = true;

        Log("init failed: no output window");

        return false;

    }

    g_swapchain = sc;

    g_hwnd = desc.OutputWindow;
    // Display affinity belongs to capture_guard now: it applies the saved
    // OBS-protection setting instead of unconditionally clearing it, and
    // re-asserts it if the game drops it (see ApplyObsProtection).

    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device)) || !g_device) {

        g_init_failed = true;

        Log("init failed: GetDevice");

        return false;

    }

    g_device->GetImmediateContext(&g_context);

    if (!g_context) {

        g_init_failed = true;

        Log("init failed: GetImmediateContext");

        return false;

    }

    CreateRTV();

    if (!g_rtv) {

        g_init_failed = true;

        Log("init failed: rtv");

        return false;

    }

    IMGUI_CHECKVERSION();

    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();

    io.IniFilename = nullptr;

    io.LogFilename = nullptr;

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    CometUi::ApplyStyle();

    InitFonts();

    ImGui_ImplWin32_Init(g_hwnd);

    if (!ImGui_ImplDX11_Init(g_device, g_context)) {

        g_init_failed = true;

        Log("init failed: ImGui_ImplDX11_Init");

        return false;

    }

    EnsureWndProc();

    InitSharedMemory();

    LoadCfg();
    EnsureWndProc();
    ApplyMenuPreferences();
    SaveCfg();

    LoadCatalog();

    LoadState();

    g_imgui_ready = true;

    Log("overlay ready hwnd=%p device=%p rtv=%p size=%ux%u", g_hwnd, g_device, g_rtv, g_width, g_height);

    return true;

}



static void RenderFrame(IDXGISwapChain* sc) {

    if(!BindSwapchain(sc)) return;

    HandleToggle();

    if (!g_imgui_ready) return;

    ULONGLONG tick = GetTickCount64();

    bool menu = Has(F_MENU_OPEN);

    bool visual = menu || Has(F_ESP) || Has(F_AIM_FOV_CIRCLE);

    ULONGLONG state_interval = visual ? 0 : 250;

    if (tick - g_last_state_poll >= state_interval) {

        g_last_state_poll = tick;

        LoadState();

    }

    // One save path, one cadence. A slider drag used to rewrite both config files
    // (IPC + LOCALAPPDATA) on the render thread while the menu was open, which is
    // a synchronous disk write inside Present - visible as a periodic hitch.
    if (g_cfg_dirty && tick - g_last_cfg_save >= 250) {

        g_last_cfg_save = tick;

        SaveCfg();

    }

    EnsureWndProc();
    ApplyObsProtection();

    // Leaving the match is the reliable boundary for a new "Team 1": the label
    // cache is per-match, so it is cleared here as well as after a long empty
    // stretch (Frame() below), which covers wiping back to spectate.
    if (!g_state.in_match) g_team_labels.Reset();

    // Loot ESP has its own switch, so it must be able to ask for a frame on its
    // own; otherwise turning the player ESP off silently hides loot too.
    bool need = menu || (g_state.in_match &&
                         (Has(F_ESP) || Has(F_AIM_FOV_CIRCLE) || HasOpt2(OPT2_ESP_LOOT)));

    if (!need) return;

    if (!BindSwapchain(sc)) return;

    if (!g_rtv) CreateRTV();

    if (!g_rtv) return;

    // Back-buffer size change -> rebuild the RTV and the ImGui device objects.
    // This must run *before* NewFrame: NewFrame uploads the font atlas using the
    // texture handle stored in io.Fonts->TexID, so starting a frame after
    // invalidating and before re-creating leaves that pointer dangling - a crash
    // or a blank atlas on a window resize or DPI change.
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        const bool sized = SUCCEEDED(sc->GetDesc(&desc)) &&
                           desc.BufferDesc.Width > 0 && desc.BufferDesc.Height > 0;
        const bool resized = sized && (desc.BufferDesc.Width != g_width ||
                                       desc.BufferDesc.Height != g_height);
        if (resized || g_display_dirty) {
            g_display_dirty = false;
            ImGui_ImplDX11_InvalidateDeviceObjects();
            CreateRTV();
            ImGui_ImplDX11_CreateDeviceObjects();
            if (sized) {
                g_width = desc.BufferDesc.Width;
                g_height = desc.BufferDesc.Height;
            }
        }
    }

    ImGuiIO& io = ImGui::GetIO();

    io.MouseDrawCursor = Has(F_MENU_OPEN);
    if(Has(F_MENU_OPEN)) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        ClipCursor(nullptr);
    } else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    static bool previous_menu=false;
    if(previous_menu&&!Has(F_MENU_OPEN)) {io.ClearInputKeys();io.ClearInputMouse();}
    previous_menu=Has(F_MENU_OPEN);

    ImGui_ImplDX11_NewFrame();

    ImGui_ImplWin32_NewFrame();

    UpdateClientSize();

    if (g_client_w > 0 && g_client_h > 0) {

        ImVec2 ds((float)g_client_w, (float)g_client_h);

        if (io.DisplaySize.x != ds.x || io.DisplaySize.y != ds.y) {

            if (g_imgui_ready) g_reset_menu_pos = true;

        }

        io.DisplaySize = ds;

        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    }

    ImGui::NewFrame();

    // Resolve menu changes before choosing the visual destination in this same frame.
    auto* background=ImGui::GetBackgroundDrawList();
    background->ChannelsSplit(2);background->ChannelsSetCurrent(1);
    DrawMenu();
    EnsureWndProc();
    background->ChannelsSetCurrent(0);
    // Team numbering advances on every frame (not just when the ESP draws) so a
    // finished match clears the cache even if the ESP was off the whole time.
    g_team_labels.Frame(g_state.in_match && !g_ents.empty());
    DrawESP();DrawFOV();DrawRadar();
    // Loot is its own feature: it keeps working with the player ESP switched off.
    DrawLootESP();
    background->ChannelsMerge();
    if(g_unloading) {ImGui::EndFrame();return;}

    ImGui::Render();

    // Save the game's pipeline state and put it back afterwards. Binding our RTV
    // without restoring the previous render target leaves the game drawing into
    // the wrong target with the wrong viewport on the next frame, which presents
    // as a frozen picture while the window still processes input.
    // Restore *exactly* what the game had bound. Asking for the full MRT set
    // matters: reading one target and rebinding one target would quietly drop
    // the rest of a multi-target set, and the count matters too - if the game
    // had nothing bound, rebinding "one target" is already wrong.
    constexpr UINT kMaxTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
    ID3D11RenderTargetView* prev_rtvs[kMaxTargets]{};
    ID3D11DepthStencilView* prev_dsv = nullptr;
    g_context->OMGetRenderTargets(kMaxTargets, prev_rtvs, &prev_dsv);
    UINT prev_rtv_count = 0;
    while (prev_rtv_count < kMaxTargets && prev_rtvs[prev_rtv_count]) ++prev_rtv_count;

    D3D11_VIEWPORT prev_vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT vp_count = (UINT)(sizeof(prev_vp) / sizeof(prev_vp[0]));
    g_context->RSGetViewports(&vp_count, prev_vp);

    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    if (vp_count) g_context->RSSetViewports(vp_count, prev_vp);
    g_context->OMSetRenderTargets(prev_rtv_count, prev_rtv_count ? prev_rtvs : nullptr, prev_dsv);
    for (UINT i = 0; i < prev_rtv_count; ++i) prev_rtvs[i]->Release();
    if (prev_dsv) prev_dsv->Release();
}



static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if(g_inside_present) return g_present(sc,sync,flags);
    PresentScope present_scope;
    if (g_unloading) return g_present ? g_present(sc, sync, flags) : sc->Present(sync, flags);

    if (!g_render_disabled) {
        const unsigned fault = RenderFrameGuarded(sc);
        if (fault) NoteFault("present", fault);
    }
    // Always forward, whatever happened above: skipping the game's own Present
    // is what turns an overlay fault into a frozen picture.
    return g_present(sc, sync, flags);
}





static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    if(g_inside_present) return g_present1(sc,sync,flags,params);
    PresentScope present_scope;
    if (g_unloading) return g_present1 ? g_present1(sc, sync, flags, params) : sc->Present1(sync, flags, params);

    if (!g_render_disabled) {
        const unsigned fault = RenderFrameGuarded(sc);
        if (fault) NoteFault("present1", fault);
    }
    return g_present1(sc, sync, flags, params);
}





static HRESULT ResizeBuffersBody(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {

    if (g_unloading) return g_resize ? g_resize(sc, count, w, h, fmt, flags) : sc->ResizeBuffers(count, w, h, fmt, flags);

    if (g_swapchain && sc != g_swapchain) {

        Log("resize skip other swapchain %p", sc);

        return g_resize(sc, count, w, h, fmt, flags);

    }

    // We hold a reference to buffer 0 through g_rtv, and ResizeBuffers fails with
    // DXGI_ERROR_INVALID_CALL while that reference exists - which leaves the game
    // on a stale back buffer, the classic "picture is frozen but the UI still
    // responds" symptom. Release exactly once here, re-create it below.
    if (g_rtv) {

        g_rtv->Release();

        g_rtv = nullptr;

    }

    if (g_imgui_ready) ImGui_ImplDX11_InvalidateDeviceObjects();

    HRESULT hr = g_resize(sc, count, w, h, fmt, flags);

    if (FAILED(hr)) Log("resize buffers failed %ux%u hr=%08x", w, h, (unsigned)hr);

    if (g_imgui_ready) {

        CreateRTV();

        ImGui_ImplDX11_CreateDeviceObjects();

    }

    g_display_dirty = true;

    g_reset_menu_pos = true;

    Log("resize buffers %ux%u hr=%08x", w, h, (unsigned)hr);

    return hr;

}

// SEH wrapper: no destructible locals here (see the note above), so MSVC accepts
// the __try. A fault inside the resize path must not propagate into the game's
// ResizeBuffers caller, which usually aborts the process on failure.
static unsigned ResizeBuffersGuarded(IDXGISwapChain* sc, UINT count, UINT w, UINT h,
                                     DXGI_FORMAT fmt, UINT flags, HRESULT& hr) {
    DWORD code = 0;
    COMET_TRY {
        hr = ResizeBuffersBody(sc, count, w, h, fmt, flags);
    }
    COMET_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        code = GetExceptionCode();
    }
    return (unsigned)code;
}

static HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    HRESULT hr = S_OK;
    const unsigned fault = ResizeBuffersGuarded(sc, count, w, h, fmt, flags, hr);
    if (fault) {
        NoteFault("resize", fault);
        // Fall through to the original so the game still gets a well-formed
        // result instead of a made-up one.
        return g_resize ? g_resize(sc, count, w, h, fmt, flags) : hr;
    }
    return hr;
}




static bool InstallHook() {

    WNDCLASSEXW wc{};

    wc.cbSize = sizeof(wc);

    wc.lpfnWndProc = DefWindowProcW;

    wc.hInstance = GetModuleHandleW(nullptr);

    wc.lpszClassName = L"BS_OVERLAY_DUMMY";

    RegisterClassExW(&wc);

    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,

                             nullptr, nullptr, wc.hInstance, nullptr);

    if (!w) {

        Log("dummy window failed %lu", GetLastError());

        UnregisterClassW(wc.lpszClassName, wc.hInstance);

        return false;

    }

    DXGI_SWAP_CHAIN_DESC sd{};

    sd.BufferCount = 1;

    sd.BufferDesc.Width = 8;

    sd.BufferDesc.Height = 8;

    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    sd.OutputWindow = w;

    sd.SampleDesc.Count = 1;

    sd.Windowed = TRUE;

    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* dev = nullptr;

    ID3D11DeviceContext* ctx = nullptr;

    IDXGISwapChain* sc = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,

                                               nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);

    if (FAILED(hr)) {

        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,

                                           nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);

    }

    if (FAILED(hr) || !sc) {

        Log("dummy swapchain failed hr=%08x", (unsigned)hr);

        if (dev) dev->Release();

        if (ctx) ctx->Release();

        DestroyWindow(w);

        UnregisterClassW(wc.lpszClassName, wc.hInstance);

        return false;

    }

    void** vt = *reinterpret_cast<void***>(sc);

    void* present_addr = vt[8];

    void* resize_addr = vt[13];

    IDXGISwapChain1* sc1 = nullptr;

    void* present1_addr = nullptr;

    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {

        void** vt1 = *reinterpret_cast<void***>(sc1);

        present1_addr = vt1[22];

    }

    MH_STATUS st = MH_Initialize();

    Log("MinHook init=%d present=%p present1=%p resize=%p", (int)st, present_addr, present1_addr, resize_addr);

    bool ok = true;

    if (MH_CreateHook(present_addr, &HookPresent, (void**)&g_present) != MH_OK) {

        ok = false;

        Log("MH_CreateHook present failed");

    }

    if (present1_addr && MH_CreateHook(present1_addr, &HookPresent1, (void**)&g_present1) != MH_OK) {

        Log("MH_CreateHook present1 failed");

    }

    if (resize_addr && MH_CreateHook(resize_addr, &HookResizeBuffers, (void**)&g_resize) != MH_OK) {

        Log("MH_CreateHook resize failed");

    }

    if (ok) {

        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {

            ok = false;

            Log("MH_EnableHook failed");

        }

    }

    if (sc1) sc1->Release();

    sc->Release();

    ctx->Release();

    dev->Release();

    DestroyWindow(w);

    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    Log("hook install %s", ok ? "ok" : "failed");

    return ok;

}



static DWORD WINAPI InitThread(LPVOID) {

    Sleep(800);

    InstallHook();

    return 0;

}



// ---------------------------------------------------------------------------
// Recoil Control System (RCS): mouse-assist layer. NOT no-recoil.
// No-recoil is handled by pyexec (bs_runtime.py): WeaponCase.GetRealRecoilData
// hooks + FpsPlacer impulse damping zero the pattern at source. RCS is a
// secondary user-driven mouse drag that ONLY runs when Strength > 0, meant
// as a backup if the memory hooks miss a weapon path.
// ---------------------------------------------------------------------------
static DWORD WINAPI RecoilControlSystemWorker(LPVOID) {
    // Pacing: a short 6 ms tick keeps drag smooth without thrashing SendInput.
    constexpr DWORD kActiveMs = 6;
    constexpr DWORD kIdleMs   = 200;   // flag/menu/match gate not active → cheap poll

    // Fractional carry so sub-pixel drag doesn't get truncated each tick.
    double carry = 0.0;
    ULONGLONG hold_start = 0;

    while (!g_unloading) {
        // Fast gate check first — skip the physical input polling 32× cheaper
        // when the subsystem is not engaged (menu open, out of match, strength=0).
        const bool engaged = Has(F_RECOIL) && !Has(F_MENU_OPEN) && g_state.in_match
                           && g_cfg.humanize > 0.0f;
        Sleep(engaged ? kActiveMs : kIdleMs);
        if (!engaged) { carry = 0.0; hold_start = 0; continue; }
        if (!g_hwnd) continue;

        HWND fg = GetForegroundWindow();
        if (fg != g_hwnd && GetAncestor(g_hwnd, GA_ROOT) != fg) {
            carry = 0.0; hold_start = 0; continue;
        }

        // RawAsyncKeyState reads the hardware state; IME overlays don't mask it.
        bool firing = (RawAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (!firing) { carry = 0.0; hold_start = 0; continue; }

        // Warm-up: first ~70 ms of a burst has no visible kick, so skip it.
        ULONGLONG now = GetTickCount64();
        if (!hold_start) { hold_start = now; continue; }
        if (now - hold_start < 70) continue;

        // Drag per tick. Strength 0..2 maps to 0..~3 px / tick of downward offset.
        // Humanize flag adds ±0.5 px jitter so the compensation doesn't look robotic.
        float strength = std::clamp(g_cfg.humanize, 0.0f, 2.0f);
        double dy = strength * 1.5 * (kActiveMs / 6.0);
        if (Has(F_AIM_HUMANIZE)) {
            double j = ((double)(now & 0xFF) / 255.0 - 0.5);  // cheap deterministic jitter
            dy += j * 0.9;
        }
        carry += dy;
        int step = (int)carry;
        if (step == 0) continue;
        carry -= step;

        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = 0;
        in.mi.dy = step;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof(in));
    }
    return 0;
}





#ifndef CIYE_MENU_PREVIEW
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {

    if (reason == DLL_PROCESS_ATTACH) {

        g_hmodule = module;

        DisableThreadLibraryCalls(module);

        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);

        if (t) CloseHandle(t);

        HANDLE r = CreateThread(nullptr, 0, RecoilControlSystemWorker, nullptr, 0, nullptr);

        if (r) CloseHandle(r);

    }

    return TRUE;

}
#endif
