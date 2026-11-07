// Shared config layout between overlay.cpp and the loader UI.
// Keep in sync with overlay.cpp's Cfg (do not reorder — it's serialized).
// CHANGE LOG vs original:
//   - pad[3] -> pad[0] consumed for col_armor2 (was already there), padding adjusted
//   - Ent: added faction (uint8), hero_name[32]
//   - New flags: F_ESP_HERO_NAME, F_ESP_TEAM_LABEL, F_AIM_INPUT_NOISE
//   - New OPT bits: OPT_CAPTURE_PROTECT, OPT_ARMOR_TIER_COLOR
//   - col_armor removed from user-editable path (now computed at render time)
//   - FactionColor() helper maps faction int -> deterministic non-red ImU32
//   - ArmorTierColor() maps max_armor -> tier color
#pragma once
#include <cstdint>
#include <cmath>

#pragma pack(push, 1)
struct Cfg {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t flags;
    uint32_t apply_seq;
    int32_t  apply_type;
    uint32_t apply_item;
    int32_t  aim_key;
    uint32_t col_fov;
    int32_t  menu_key;
    float    aim_fov;
    float    aim_smooth;
    float    aim_max_dist;
    float    human_smooth;
    float    humanize;
    int32_t  aim_bone;
    int32_t  esp_max_dist;
    uint32_t reserved;      // OPT_* bits
    uint32_t col_box;
    uint32_t col_box_hidden;
    uint32_t col_team;
    uint32_t col_monster;
    uint32_t col_skel;
    uint32_t col_line;
    uint32_t col_name;
    uint32_t col_gun;
    uint32_t col_dist;
    uint32_t col_health;
    uint32_t col_headdot;
    uint32_t col_count;
    uint32_t esp_style;
    float    box_thickness;
    uint32_t skin_hero_item;
    uint32_t skin_gun_item;
    uint32_t skin_knife_item;
    uint32_t auto_skin;
    uint32_t col_armor;     // kept for binary compat; not user-editable when armor-tier is on
    uint32_t pad[3];        // pad[0..2] reserved for future; do not reorder above
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Feature flags (flags field)
// ---------------------------------------------------------------------------
enum : uint32_t {
    F_MENU_OPEN      = 1u << 0,
    F_ESP            = 1u << 1,
    F_ESP_BOX        = 1u << 2,
    F_ESP_SKELETON   = 1u << 3,
    F_ESP_LINE       = 1u << 4,
    F_ESP_NAME       = 1u << 5,
    F_ESP_GUN        = 1u << 6,
    F_ESP_DISTANCE   = 1u << 7,
    F_ESP_HEALTH     = 1u << 8,
    F_ESP_HEADDOT    = 1u << 9,
    F_ESP_TEAM       = 1u << 10,
    F_ESP_VISIBLE    = 1u << 11,
    F_ESP_COUNT      = 1u << 12,
    F_AIM            = 1u << 13,
    F_AIM_VISIBLE    = 1u << 14,
    F_AIM_TEAM       = 1u << 15,
    F_AIM_FOV_CIRCLE = 1u << 16,
    F_AIM_HUMANIZE   = 1u << 17,
    F_AIM_PREDICT    = 1u << 18,
    F_ESP_ARMOR      = 1u << 19,
    F_UNLOCK_EFFECTS = 1u << 20,
    F_BLOCK_REPORT   = 1u << 21,
    F_SKIN_CURRENT   = 1u << 22,
    F_RECOIL         = 1u << 23,
    F_AIM_MULTIPOINT = 1u << 24,
    F_FORCE_KILLFX   = 1u << 25,
    F_EXIT_CHEAT     = 1u << 26,
    F_ESP_CORNER     = 1u << 27,
    F_ESP_OUTLINE    = 1u << 28,
    F_ESP_GRADIENT   = 1u << 29,
    F_ESP_SHADOW     = 1u << 30,
    F_ESP_FILLED     = 1u << 31,
};

// ---------------------------------------------------------------------------
// OPT bits (reserved field)
// ---------------------------------------------------------------------------
inline constexpr uint32_t OPT_AIM_IGNORE_KNOCKED = 1u << 1;
inline constexpr uint32_t OPT_AIM_BONES_SET      = 1u << 2;
inline constexpr uint32_t OPT_ESP_RADAR          = 1u << 3;
inline constexpr uint32_t OPT_FOV_COLOR_SET      = 1u << 4;
inline constexpr uint32_t OPT_AIM_SILENT         = 1u << 5;
inline constexpr uint32_t OPT_AIM_HEAD           = 1u << 8;
inline constexpr uint32_t OPT_AIM_NECK           = 1u << 9;
inline constexpr uint32_t OPT_AIM_BODY           = 1u << 10;
inline constexpr uint32_t OPT_AIM_BONE_MASK      = OPT_AIM_HEAD | OPT_AIM_NECK | OPT_AIM_BODY;
// NEW:
inline constexpr uint32_t OPT_CAPTURE_PROTECT    = 1u << 6;  // WDA_EXCLUDEFROMCAPTURE on loader hwnd
inline constexpr uint32_t OPT_ARMOR_TIER_COLOR   = 1u << 7;  // auto armor tier coloring
inline constexpr uint32_t OPT_ESP_HERO_NAME      = 1u << 11; // show hero (character) name below player name
inline constexpr uint32_t OPT_ESP_TEAM_LABEL     = 1u << 12; // show T1/T2 label + faction-based box/line color
inline constexpr uint32_t OPT_AIM_INPUT_NOISE    = 1u << 13; // Gaussian mouse noise on aimbot SendInput

static_assert((OPT_AIM_SILENT & (OPT_AIM_IGNORE_KNOCKED | OPT_AIM_BONES_SET | OPT_ESP_RADAR |
                                  OPT_FOV_COLOR_SET | OPT_AIM_BONE_MASK)) == 0);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
inline constexpr uint32_t CFG_UI_TAG             = 0x55490001;
inline constexpr uint32_t ESP_LINE_ORIGIN_MASK   = 3u;
inline constexpr uint32_t ESP_OFFSCREEN_ARROW    = 1u << 2;

bool SaveConfiguration();
const char* ConfigurationStatus();

extern Cfg  g_cfg;
extern bool g_cfg_dirty;

inline bool Has(uint32_t f)             { return (g_cfg.flags & f) != 0; }
inline bool HasOpt(uint32_t o)          { return (g_cfg.reserved & o) != 0; }
inline void SetF(uint32_t f, bool on)   {
    uint32_t prev = g_cfg.flags;
    g_cfg.flags = on ? (g_cfg.flags | f) : (g_cfg.flags & ~f);
    if (g_cfg.flags != prev) g_cfg_dirty = true;
}
inline void SetOpt(uint32_t o, bool on) {
    uint32_t prev = g_cfg.reserved;
    g_cfg.reserved = on ? (g_cfg.reserved | o) : (g_cfg.reserved & ~o);
    if (g_cfg.reserved != prev) g_cfg_dirty = true;
}

// ---------------------------------------------------------------------------
// ArmorTierColor — maps max_armor to a tier ImU32
// Called from overlay draw; user cannot override when OPT_ARMOR_TIER_COLOR on.
//   <=50  → Gray
//   <=75  → Blue
//   <=100 → Purple
//   >100  → Gold
// ---------------------------------------------------------------------------
inline uint32_t ArmorTierColor(float max_armor) {
    if (max_armor <= 50.0f)  return 0xFFA8A8A8; // Gray  (ABGR)
    if (max_armor <= 75.0f)  return 0xFFE09040; // Blue  (ABGR: 0xFF + B=0xE0 + G=0x90 + R=0x40)
    if (max_armor <= 100.0f) return 0xFFD060D0; // Purple
    return                          0xFF20C8FF; // Gold  (ABGR: bright yellow-gold)
}
// Note: ImU32 is 0xAABBGGRR in ImGui's default.
// Gray  : R=168 G=168 B=168 -> IM_COL32(168,168,168,255)
// Blue  : R=64  G=144 B=224 -> IM_COL32(64,144,224,255)
// Purple: R=208 G=96  B=208 -> IM_COL32(208,96,208,255)
// Gold  : R=255 G=200 B=32  -> IM_COL32(255,200,32,255)
inline uint32_t ArmorTierColorIM(float max_armor) {
    if (max_armor <= 50.0f)  return IM_COL32(168, 168, 168, 240); // Gray
    if (max_armor <= 75.0f)  return IM_COL32(64,  144, 224, 240); // Blue
    if (max_armor <= 100.0f) return IM_COL32(208,  96, 208, 240); // Purple
    return                          IM_COL32(255, 200,  32, 240); // Gold
}

// ---------------------------------------------------------------------------
// FactionColor — deterministic, non-red color from faction int
// Factions 0-7 get a fixed palette; avoids red (team-visible association).
// Same faction int always → same color within a session.
// ---------------------------------------------------------------------------
inline uint32_t FactionColorIM(int faction) {
    // 8 slots, no red. Cycling: cyan, lime, orange, magenta, teal, yellow, purple, white.
    static const uint32_t kPalette[8] = {
        IM_COL32(  0, 210, 230, 220), // Cyan
        IM_COL32( 90, 230,  60, 220), // Lime
        IM_COL32(255, 160,  30, 220), // Orange
        IM_COL32(210,  60, 210, 220), // Magenta
        IM_COL32( 20, 185, 185, 220), // Teal
        IM_COL32(245, 220,  40, 220), // Yellow
        IM_COL32(140,  60, 230, 220), // Purple
        IM_COL32(210, 210, 210, 220), // White
    };
    // Map faction to palette index. Faction -1 or unknown → white.
    if (faction < 0) return kPalette[7];
    return kPalette[faction % 8];
}
