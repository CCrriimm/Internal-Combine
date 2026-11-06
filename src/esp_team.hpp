// Team label numbering and team color selection shared by the overlay renderer
// and the menu. Header-only and free of ImGui/Windows so the exact rules can be
// unit-tested without the game, a D3D device or a swap chain.
//
// The reference build (Comet external) numbers teams 1..N in the order they are
// first seen during a match and tints boxes/snaplines with a fixed palette
// indexed by the raw team id. Both pieces are reproduced here:
//
//   * Label    - "Team 1", "Team 2", ... assigned per match, cleared after a
//                sustained empty snapshot so a new match starts at 1 again.
//   * Colours  - palette colour for the raw team id; independent of the label
//                number so the same team keeps its colour across matches.
//
// Neither function participates in team or aim filtering. They only choose how
// an already-accepted entity is presented.
#pragma once
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace teamtint {

// Palette slot for a raw team id: `team % palette size`. The palette itself is
// the user's four configurable slot colours (cfg.col_team_box /
// cfg.col_team_line), so this only has to be stable, not opinionated. A team
// therefore keeps its colour across matches, and the same team gets the same
// slot for its box and its snapline. Team ids <= 0 (unknown) use slot 0.
template <typename Palette>
inline std::size_t SlotFor(int team, const Palette& palette) {
    constexpr std::size_t count = sizeof(Palette) / sizeof(palette[0]);
    static_assert(count > 0, "team palette must not be empty");
    if (team <= 0) return 0; // unknown team -> base colour, never out of range
    return static_cast<std::size_t>(team) % count;
}

template <typename Palette>
inline uint32_t ColourFor(int team, const Palette& palette) {
    return palette[SlotFor(team, palette)];
}

// Compact, stable, per-match label number. 0 is not a valid team id and is
// never assigned.
class Labels {
public:
    // Raw team ids (for example 2001) are too long for an ESP chip, so the
    // first distinct team seen becomes 1, the next 2, and so on. A team keeps
    // its number for the rest of the match.
    int NumberFor(int team) {
        if (team <= 0) return 0;
        const auto it = m_numbers.find(team);
        if (it != m_numbers.end()) return it->second;
        const int number = static_cast<int>(m_numbers.size()) + 1;
        m_numbers.emplace(team, number);
        m_empty_frames = 0;
        return number;
    }

    // Called once per rendered frame with the number of eligible players.
    // Clears after 120 consecutive empty frames (about two seconds at 60 FPS)
    // so a following match restarts at Team 1 instead of continuing to climb.
    void Frame(bool any_players) {
        if (any_players) {
            m_empty_frames = 0;
            return;
        }
        if (++m_empty_frames < 120) return;
        m_numbers.clear();
        m_empty_frames = 0;
    }

    void Reset() {
        m_numbers.clear();
        m_empty_frames = 0;
    }

    size_t size() const { return m_numbers.size(); }

private:
    std::unordered_map<int, int> m_numbers;
    int m_empty_frames = 0;
};

} // namespace teamtint
