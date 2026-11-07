// Team label numbering and team tint colours shared by the overlay renderer and
// the menu. Header-only and free of ImGui/Windows so the exact rules can be
// unit-tested without the game, a D3D device or a swap chain.
//
// Reference behaviour (Comet external):
//
//   * Label    - "Team 1", "Team 2", ... assigned per match in first-seen order,
//                cleared when the match ends.
//   * Colours  - the opponent team gets a colour that is generated once and then
//                held for the rest of the match, so a team never flickers between
//                colours while you look around. The colours are picked from a
//                spread of distinguishable hues and never repeat the colour the
//                user already chose for enemies/teammates, so a team tint is
//                always tellable apart from an untinted entity.
//
// Neither function participates in team or aim filtering. They only choose how
// an already-accepted entity is presented.
#pragma once
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace teamtint {

// Distinguishable, saturated colours in ImU32 / 0xAABBGGRR order (what
// ImGui::AddRect takes). Deliberately no grey and no near-white entry: those
// read as "untinted" on top of the game.
inline constexpr uint32_t kCandidates[] = {
    0xFF5050FFu, // red
    0xFF5ADC50u, // green
    0xFFFF9646u, // blue
    0xFFE15AFFu, // magenta
    0xFFE6E646u, // cyan
    0xFF3CAAFFu, // orange
    0xFFF0E68Cu, // violet
};

constexpr int kCandidateCount = (int)(sizeof(kCandidates) / sizeof(kCandidates[0]));

// Perceptual "too close": compare hue-ish dominance rather than every channel.
// The candidate list is already hand-picked to be distinguishable, so this only
// has to reject genuine near-duplicates - a channel-wise AND test wrongly called
// saturated blue and green similar because their red channels both sat low.
inline bool TooClose(uint32_t a, uint32_t b) {
    const auto channel = [](uint32_t v, int shift) { return (int)((v >> shift) & 0xFF); };
    const int ar = channel(a, 0), ag = channel(a, 8), ab = channel(a, 16);
    const int br = channel(b, 0), bg = channel(b, 8), bb = channel(b, 16);
    // Dominant channel differs -> clearly different colour families.
    auto dominant = [](int r, int g, int bl) {
        if (r >= g && r >= bl) return 0;
        return g >= bl ? 1 : 2;
    };
    if (dominant(ar, ag, ab) != dominant(br, bg, bb)) return false;
    // Same dominant channel: only reject if the whole triple is close.
    const int dr = ar - br, dg = ag - bg, db = ab - bb;
    const int dist = (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
    return dist < 96;
}

// Compact, stable, per-match label number and per-match colour. 0 is not a
// valid team id and is never assigned a number.
class Labels {
public:
    // Raw team ids (for example 2001) are too long for an ESP chip, so the
    // first distinct team seen becomes 1, the next 2, and so on. A team keeps
    // its number for the rest of the match.
    int NumberFor(int team) {
        if (team <= 0) return 0;
        const auto it = m_teams.find(team);
        if (it != m_teams.end()) return it->second.number;
        const int number = static_cast<int>(m_teams.size()) + 1;
        Team& entry = m_teams[team];
        entry.number = number;
        entry.colour = 0; // assigned lazily, needs the reserved-colour set
        m_empty_frames = 0;
        return number;
    }

    // Colour for a team, generated on first use and then held for the match.
    // `reserved` holds colours that must not be picked (the player's own enemy
    // and teammate colours) so a team tint stays distinguishable from them.
    // Returns 0 when the team is invalid; callers fall back to their base colour.
    template <typename Reserved>
    uint32_t ColourFor(int team, const Reserved& reserved) {
        if (team <= 0) return 0;
        const auto it = m_teams.find(team);
        if (it == m_teams.end()) {
            NumberFor(team);
            return ColourFor(team, reserved);
        }
        if (it->second.colour != 0) return it->second.colour;
        const uint32_t picked = Pick(team, reserved);
        m_teams[team].colour = picked;
        return picked;
    }

    // Called once per rendered frame with whether any players are present.
    // Clears after 120 consecutive empty frames (about two seconds at 60 FPS)
    // so a following match restarts at Team 1 with fresh colours.
    void Frame(bool any_players) {
        if (any_players) {
            m_empty_frames = 0;
            return;
        }
        if (++m_empty_frames < 120) return;
        m_teams.clear();
        m_empty_frames = 0;
    }

    void Reset() {
        m_teams.clear();
        m_empty_frames = 0;
    }

    size_t size() const { return m_teams.size(); }

private:
    struct Team {
        int number = 0;
        uint32_t colour = 0;
    };

    // Deterministic pick: walk the candidate list starting at an offset derived
    // from the team id, skip anything reserved or already used by another team,
    // and skip near-duplicates of anything already in play. Deterministic means
    // the same team id gets the same colour for a given set of teams already
    // known, so it never flickers; the id offset is what makes different teams
    // land on different colours.
    template <typename Reserved>
    uint32_t Pick(int team, const Reserved& reserved) const {
        constexpr int count = kCandidateCount;
        const int start = (team < 0 ? -team : team) % count;
        for (int attempt = 0; attempt < count; ++attempt) {
            const uint32_t candidate = kCandidates[(start + attempt) % count];
            if (reserved.count(candidate)) continue;
            bool taken = false;
            for (const auto& other : m_teams) {
                if (other.second.colour == 0) continue;
                if (other.second.colour == candidate ||
                    TooClose(other.second.colour, candidate)) {
                    taken = true;
                    break;
                }
            }
            if (!taken) return candidate;
        }
        // Every candidate is in use (more teams than colours): allow reuse but
        // still avoid the user's reserved colours.
        for (int attempt = 0; attempt < count; ++attempt) {
            const uint32_t candidate = kCandidates[(start + attempt) % count];
            if (!reserved.count(candidate)) return candidate;
        }
        return kCandidates[start];
    }

    std::unordered_map<int, Team> m_teams;
    int m_empty_frames = 0;
};

} // namespace teamtint
