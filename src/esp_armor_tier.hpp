// Max-armor tier colours, ported from the reference Comet build
// (src/esp_colors.hpp :: esp_colors::ArmorTier).
//
// The armor BAR keeps whatever colour the user picked. This header only decides
// the colour used for the numeric max-armor label, so the tier is readable at a
// glance: 50 grey, 75 blue, 100 purple, 125 gold. Intermediate replicated max
// values blend between the two adjacent tiers instead of being rounded into the
// wrong one, which is what the reference does.
//
// Renderer-independent on purpose: the tier maths is unit-tested without ImGui.
#pragma once
#include <cmath>
#include <cstdint>

namespace armortier {

struct Rgba {
    float r, g, b, a;
};

inline constexpr float Clamp01(float value) {
    return !(value >= 0.0f) ? 0.0f : (value > 1.0f ? 1.0f : value);
}

inline constexpr Rgba Mix(const Rgba& from, const Rgba& to, float amount) {
    amount = Clamp01(amount);
    return {from.r + (to.r - from.r) * amount,
            from.g + (to.g - from.g) * amount,
            from.b + (to.b - from.b) * amount,
            from.a + (to.a - from.a) * amount};
}

// 50 grey -> 75 blue -> 100 purple -> 125 gold.
inline constexpr Rgba Grey50{0.550f, 0.580f, 0.620f, 1.0f};
inline constexpr Rgba Blue75{0.200f, 0.550f, 1.000f, 1.0f};
inline constexpr Rgba Purple100{0.680f, 0.320f, 1.000f, 1.0f};
inline constexpr Rgba Gold125{1.000f, 0.760f, 0.120f, 1.0f};

// NaN and unknown (<= 50) inputs land on the grey tier, matching the reference.
inline constexpr Rgba Tier(float max_armor) {
    if (!(max_armor > 50.0f)) return Grey50;
    if (max_armor <= 75.0f) return Mix(Grey50, Blue75, (max_armor - 50.0f) / 25.0f);
    if (max_armor <= 100.0f) return Mix(Blue75, Purple100, (max_armor - 75.0f) / 25.0f);
    if (max_armor < 125.0f) return Mix(Purple100, Gold125, (max_armor - 100.0f) / 25.0f);
    return Gold125;
}

// Same boundaries, caller-supplied palette, so the menu's tier colours are
// what the label actually uses. The single-colour overload above stays the
// documented default.
inline constexpr Rgba Tier(float max_armor, const Rgba& grey50, const Rgba& blue75,
                          const Rgba& purple100, const Rgba& gold125) {
    if (!(max_armor > 50.0f)) return grey50;
    if (max_armor <= 75.0f) return Mix(grey50, blue75, (max_armor - 50.0f) / 25.0f);
    if (max_armor <= 100.0f) return Mix(blue75, purple100, (max_armor - 75.0f) / 25.0f);
    if (max_armor < 125.0f) return Mix(purple100, gold125, (max_armor - 100.0f) / 25.0f);
    return gold125;
}

inline constexpr uint32_t Pack(const Rgba& c) {
    const auto channel = [](float v) {
        const float scaled = Clamp01(v) * 255.0f;
        return static_cast<uint32_t>(scaled + 0.5f);
    };
    return (channel(c.a) << 24) | (channel(c.b) << 16) | (channel(c.g) << 8) | channel(c.r);
}

// ImU32 is 0xAABBGGRR. Also the source of the config's default tier colours,
// so the palette in this header and the shipped defaults cannot drift apart.
inline constexpr uint32_t Packed(float max_armor) {
    return Pack(Tier(max_armor));
}

// ImU32 from a packed Windows colour (0xAABBGGRR), i.e. what the config stores.
inline Rgba Unpack(uint32_t bgra) {
    const auto channel = [](uint32_t v) { return static_cast<float>(v) / 255.0f; };
    return {channel(bgra & 0xFFu), channel((bgra >> 8) & 0xFFu),
            channel((bgra >> 16) & 0xFFu), channel((bgra >> 24) & 0xFFu)};
}

} // namespace armortier
