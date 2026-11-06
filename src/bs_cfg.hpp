// Shared config layout between overlay.cpp and the Menu34 UI.
// Keep in sync with overlay.cpp's Cfg (do not reorder — it's serialized).
#pragma once
#include <cstdint>

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
    uint32_t col_fov; // Reuses the unused key slot; serialized size/offsets stay unchanged.
    int32_t  menu_key;
    float    aim_fov;
    float    aim_smooth;
    float    aim_max_dist;
    float    human_smooth; // Reuses the former unused FOV slot; serialized layout is unchanged.
    float    humanize;
    int32_t  aim_bone;
    int32_t  esp_max_dist;
    uint32_t reserved;
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
    uint32_t col_armor;
    uint32_t pad[3];
};
#pragma pack(pop)

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

inline constexpr uint32_t CFG_UI_TAG = 0x55490001;
inline constexpr uint32_t ESP_LINE_ORIGIN_MASK = 3u; // Bottom=0, Top=1, Mid=2.
inline constexpr uint32_t ESP_OFFSCREEN_ARROW = 1u << 2;
bool SaveConfiguration();
const char* ConfigurationStatus();

inline constexpr uint32_t OPT_AIM_IGNORE_KNOCKED = 1u << 1;
inline constexpr uint32_t OPT_AIM_BONES_SET = 1u << 2;
inline constexpr uint32_t OPT_ESP_RADAR = 1u << 3;
inline constexpr uint32_t OPT_AIM_SILENT = 1u << 5;
inline constexpr uint32_t OPT_FOV_COLOR_SET = 1u << 4;
inline constexpr uint32_t OPT_AIM_HEAD = 1u << 8;
inline constexpr uint32_t OPT_AIM_NECK = 1u << 9;
inline constexpr uint32_t OPT_AIM_BODY = 1u << 10;
inline constexpr uint32_t OPT_AIM_BONE_MASK = OPT_AIM_HEAD | OPT_AIM_NECK | OPT_AIM_BODY;
static_assert((OPT_AIM_SILENT & (OPT_AIM_IGNORE_KNOCKED | OPT_AIM_BONES_SET | OPT_ESP_RADAR |
                               OPT_FOV_COLOR_SET | OPT_AIM_BONE_MASK)) == 0);

extern Cfg  g_cfg;
extern bool g_cfg_dirty;

inline bool Has(uint32_t f) { return (g_cfg.flags & f) != 0; }
inline void SetF(uint32_t f, bool on) {
    uint32_t prev = g_cfg.flags;
    g_cfg.flags = on ? (g_cfg.flags | f) : (g_cfg.flags & ~f);
    if (g_cfg.flags != prev) g_cfg_dirty = true;
}
