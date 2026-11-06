// 34cheat/src/menu.hpp
// context: C++20 / menu.hpp / MSVC / x64 / Windows 10/11 / version-5a21b87bbc2641cf
// In-game settings window drawn on the overlay.
#pragma once

#include "ui_renderer.hpp"
#include "features.hpp"
#include "../include/profiles.hpp"
#include "../include/team_check.hpp"

namespace Menu {

    enum class Tab { Legit, Rage, ESP, Misc, Players, Teams, Settings, Count };
    constexpr int kTabCount=static_cast<int>(Tab::Count);

    class MainMenu {
    private:
        Math::Vector2 m_pos{ 100.0f, 80.0f };
        Math::Vector2 m_size{ 900.0f, 620.0f };
        int m_activeTab=static_cast<int>(Tab::Legit);
        int m_pages[kTabCount]{};
        std::vector<Profiles::Entry> m_profiles;
        std::filesystem::path m_defaultProfile;
        int m_profileSelection=-1;
        bool m_profilesLoaded=false;
        std::wstring m_profileName;
        std::wstring m_teamSearch;
        std::string m_selectedTeam;
#ifdef CHEAT_TEST_STORAGE
        std::vector<TeamCheck::TeamInfo> m_previewTeams;
#endif
        void RefreshProfiles(const std::filesystem::path& select={});
        bool m_dragging = false;
        Math::Vector2 m_dragOffset{ 0.0f, 0.0f };

        float m_scrollTarget[kTabCount]{};
        float m_contentHeight[kTabCount]{};

        void RenderSidebar();
        void RenderHeader();
        void RenderBody();
        void RenderCombat(int page, float x, float y, float colW, float gap, float& height);
        void RenderTabVisuals(float x, float y, float colW, float gap, float& height);
        void RenderTabPlayers(float x, float y, float colW, float gap, float& height);
        void RenderTabTeams(float x, float y, float colW, float gap, float& height);
        void RenderTabUtilities(float x, float y, float colW, float gap, float& height);
        void RenderTabSettings(float x, float y, float colW, float gap, float& height);
        void RenderTabMovement(float x, float y, float colW, float gap, float& height);
        void RenderCharacter(float x, float y, float colW, float gap, float& height);
        void RenderTabSpin(float x, float y, float colW, float gap, float& height);

        void RenderLiveESPPreview(const Math::Vector2& pos, const Math::Vector2& size);
        void RenderCrosshairPreview(const Math::Vector2& pos, const Math::Vector2& size);

    public:
        MainMenu() = default;
#ifdef CHEAT_TEST_STORAGE
        void SetTeamPreview(std::vector<TeamCheck::TeamInfo> teams) {m_previewTeams=std::move(teams);}
#endif

        void Render();
        void SetPosition(const Math::Vector2& pos) { m_pos = pos; }
        const Math::Vector2& GetPosition() const { return m_pos; }
        const Math::Vector2& GetSize() const { return m_size; }
    };

    // Shared by the in-game crosshair and the menu preview so both always match.
    void DrawCrosshair(const Math::Vector2& center, const D2D1_COLOR_F& color);

    inline MainMenu g_Menu;

} // namespace Menu
