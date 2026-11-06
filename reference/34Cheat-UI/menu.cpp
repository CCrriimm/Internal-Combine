// 34cheat/src/menu.cpp
// context: C++20 / menu.cpp / MSVC / x64 / Windows 10/11 / version-5a21b87bbc2641cf
#include "menu.hpp"
#include "../include/protected_text.hpp"
#include "overlay.hpp"
#include "../include/updater.hpp"
#include "../include/profiles.hpp"
#include "../include/local_effects.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cwctype>

namespace Menu {

    namespace {
        constexpr float kRailWidth = 196.0f;
        constexpr float kHeaderHeight = 64.0f;
        constexpr float kRadius = 12.0f;

        struct TabInfo {
            const wchar_t* Title;
            UI::Icon Glyph;
        };

        const TabInfo kTabs[kTabCount] = {
            {L"Legit",UI::Icon::Aim},
            {L"Rage",UI::Icon::Rage},
            {L"ESP",UI::Icon::Visuals},
            {L"Misc",UI::Icon::Utilities},
            {L"Players",UI::Icon::Players},
            {L"Teams",UI::Icon::Players},
            {L"Settings",UI::Icon::Settings},
        };
        const std::vector<std::wstring> kPages[kTabCount]={
            {L"Aim",L"Trigger"},
            {L"Silent",L"Magnet",L"Hitboxes",L"Spin"},
            {},
            {L"Movement",L"Character",L"Utilities"},
            {},{},{},
        };

        // Vertical flow for one settings column. Space above a heading is larger than below it.
        struct Column {
            float x, y, w;
            bool first = true;

            void Section(const std::wstring& title, const std::wstring& aside = L"") {
                const auto& colors = UI::g_UI.GetColors();
                if (!first) y += 22.0f;
                first = false;
                UI::g_UI.DrawText(title, { x, y }, { w, 22.0f }, colors.TextPrimary, UI::FontHeader);
                if (!aside.empty()) {
                    UI::g_UI.DrawText(aside, { x, y }, { w, 22.0f }, colors.TextTertiary, UI::FontMono, DWRITE_TEXT_ALIGNMENT_TRAILING);
                }
                y += 30.0f;
            }

            bool Toggle(const std::wstring& label, bool& value) {
                bool changed = UI::g_UI.RenderToggle({ x, y }, label, value);
                y += 30.0f;
                return changed;
            }

            bool ToggleColor(const std::wstring& label, bool& value, Config::Color4& color) {
                UI::g_UI.SetItemWidth(w-40);
                bool changed=UI::g_UI.RenderToggle({x,y},label,value);
                UI::g_UI.SetItemWidth(w);
                UI::g_UI.RenderColorButton({x+w-28,y+5},label+L" colour",color);
                y+=30;
                return changed;
            }
            bool ComboColor(const std::wstring& label,int& index,const std::vector<std::wstring>& options,Config::Color4& color) {
                UI::g_UI.SetItemWidth(w-40);
                bool changed=UI::g_UI.RenderCombo({x,y},label,index,options);
                UI::g_UI.SetItemWidth(w);
                UI::g_UI.RenderColorButton({x+w-28,y+5},label+L" colour",color);
                y+=30;
                return changed;
            }
            void Color(const std::wstring& label,Config::Color4& color) {
                UI::g_UI.DrawText(label,{x,y},{w-40,30},UI::g_UI.GetColors().TextSecondary,UI::FontBody);
                UI::g_UI.RenderColorButton({x+w-28,y+5},label,color);
                y+=30;
            }

            bool Slider(const std::wstring& label, float& value, float min, float max, const std::wstring& suffix = L"", int decimals = 1) {
                bool changed = UI::g_UI.RenderSlider({ x, y + 2.0f }, label, value, min, max, suffix, decimals);
                y += 48.0f;
                return changed;
            }

            bool IntSlider(const std::wstring& label, int& value, int min, int max, const std::wstring& suffix = L"") {
                bool changed = UI::g_UI.RenderIntSlider({ x, y + 2.0f }, label, value, min, max, suffix);
                y += 48.0f;
                return changed;
            }

            bool Combo(const std::wstring& label, int& index, const std::vector<std::wstring>& options) {
                bool changed = UI::g_UI.RenderCombo({ x, y }, label, index, options);
                y += 30.0f;
                return changed;
            }

            bool Keybind(const std::wstring& label, int& key) {
                bool changed = UI::g_UI.RenderKeybind({ x, y }, label, key);
                y += 30.0f;
                return changed;
            }

            bool Button(const std::wstring& label, bool primary = false, bool enabled = true) {
                y += 6.0f;
                bool clicked = UI::g_UI.RenderButton({ x, y }, { w, 32.0f }, label, primary, enabled);
                y += 38.0f;
                return clicked;
            }

            void State(const std::wstring& text) {
                UI::g_UI.DrawText(text, { x, y }, { w, 20.0f }, UI::g_UI.GetColors().TextTertiary, UI::FontSmall);
                y += 22.0f;
            }
        };

        std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
    }

    void DrawCrosshair(const Math::Vector2& center, const D2D1_COLOR_F& baseColor) {
        const auto& cfg = Config::g_Config.Utilities;
        float arm = cfg.CrosshairSize;
        float gap = cfg.CrosshairGap;
        float thick = cfg.CrosshairThickness;
        auto& ui = UI::g_UI;
        const double time=GetTickCount64()/1000.0;
        const auto rgb=OverlayLogic::Rainbow(time);
        const auto color=cfg.CrosshairRainbow?D2D1::ColorF(rgb.x,rgb.y,rgb.z,baseColor.a):baseColor;
        if(cfg.CrosshairPulse)gap*=1+.4f*std::sin(float(time*cfg.CrosshairPulseSpeed*2*Math::PI));
        const float angle=cfg.CrosshairSpin?float(std::fmod(time*cfg.CrosshairSpinSpeed,360))*Math::DEG2RAD:0;
        auto rotate=[&](Math::Vector2 p){auto d=p-center;return center+Math::Vector2{d.x*std::cos(angle)-d.y*std::sin(angle),d.x*std::sin(angle)+d.y*std::cos(angle)};};
        auto line=[&](Math::Vector2 a,Math::Vector2 b,const D2D1_COLOR_F& c,float width){ui.DrawLine(rotate(a),rotate(b),c,width);};

        switch (cfg.CrosshairStyle) {
        case 1: // Dot
            ui.FillCircle(center, (std::max)(thick * 1.5f, 2.0f), color);
            break;
        case 2: // Circle
            ui.DrawCircle(center, gap + arm * 0.5f, color, thick);
            break;
        case 3: // T-shape: no top arm
            line({ center.x, center.y + gap }, { center.x, center.y + gap + arm }, color, thick);
            line({ center.x - gap, center.y }, { center.x - gap - arm, center.y }, color, thick);
            line({ center.x + gap, center.y }, { center.x + gap + arm, center.y }, color, thick);
            break;
        default: // Cross
            line({ center.x, center.y - gap }, { center.x, center.y - gap - arm }, color, thick);
            line({ center.x, center.y + gap }, { center.x, center.y + gap + arm }, color, thick);
            line({ center.x - gap, center.y }, { center.x - gap - arm, center.y }, color, thick);
            line({ center.x + gap, center.y }, { center.x + gap + arm, center.y }, color, thick);
            break;
        }

        if (cfg.CrosshairDot && cfg.CrosshairStyle != 1) {
            ui.FillCircle(center, (std::max)(thick * 0.8f, 1.2f), color);
        }
    }

    void MainMenu::Render() {
        auto& ui = UI::g_UI;
        const auto& colors = ui.GetColors();
        const auto& mouse = ui.GetMousePos();

        // Drag from the top strip, except over the close button.
        if (!ui.IsCapturingInput() && ui.IsHovered(m_pos, { m_size.x - 56.0f, 64.0f }) && ui.IsMouseClicked()) {
            m_dragging = true;
            m_dragOffset = { mouse.x - m_pos.x, mouse.y - m_pos.y };
        }
        if (!ui.IsMouseDown()) m_dragging = false;
        if (m_dragging) m_pos = { mouse.x - m_dragOffset.x, mouse.y - m_dragOffset.y };

        // The window floats over the game, so it gets the one real shadow.
        ui.DrawShadow(m_pos, m_size, kRadius, 12.0f, 28.0f, 0.7f);
        ui.FillRoundedRect(m_pos, m_size, kRadius, colors.BgSurface);
        ui.FillRoundedRect(m_pos, { kRailWidth + kRadius, m_size.y }, kRadius, colors.BgBase);
        ui.FillRect({ m_pos.x + kRailWidth, m_pos.y }, { kRadius, m_size.y }, colors.BgSurface);
        ui.DrawLine({ m_pos.x + kRailWidth, m_pos.y }, { m_pos.x + kRailWidth, m_pos.y + m_size.y }, colors.BorderSubtle, 1.0f);
        ui.DrawRoundedRect(m_pos, m_size, kRadius, colors.BorderSubtle, 1.0f);

        RenderSidebar();
        RenderHeader();
        RenderBody();
    }

    void MainMenu::RenderSidebar() {
        auto& ui = UI::g_UI;
        const auto& colors = ui.GetColors();

        ui.DrawLogo({ m_pos.x + 20.0f, m_pos.y + 20.0f }, 22.0f);

        // Navigation, with one sliding indicator behind the active item.
        const float startY = m_pos.y + 72.0f;
        const float itemH = 36.0f;
        const float itemStep = 38.0f;
        const float itemX = m_pos.x + 12.0f;
        const float itemW = kRailWidth - 24.0f;

        float indicatorY = ui.Animate(UI::HashId(L"menu.nav"), 0, m_activeTab * itemStep, 20.0f);
        ui.FillRoundedRect({ itemX, startY + indicatorY }, { itemW, itemH }, 8.0f, colors.BgElevated);

        for (int i = 0; i < kTabCount; ++i) {
            Math::Vector2 itemPos{ itemX, startY + i * itemStep };
            bool active = (m_activeTab == i);
            bool hovered = ui.IsHovered(itemPos, { itemW, itemH });

            if (hovered && ui.IsMouseClicked() && m_activeTab != i) {
                m_activeTab = i;
                ui.CancelInputCapture();
            }

            float hover = ui.Animate(UI::HashId(kTabs[i].Title, 0x7AB), 0, (hovered && !active) ? 1.0f : 0.0f, 22.0f);
            if (hover > 0.01f) {
                ui.FillRoundedRect(itemPos, { itemW, itemH }, 8.0f, UI::WithAlpha(D2D1::ColorF(D2D1::ColorF::White), 0.035f * hover));
            }

            D2D1_COLOR_F text = active ? colors.TextPrimary : UI::Mix(colors.TextSecondary, colors.TextPrimary, hover);
            ui.DrawIcon(kTabs[i].Glyph, { itemPos.x + 20.0f, itemPos.y + itemH * 0.5f }, 15.0f, active ? colors.Accent : text);
            ui.DrawText(kTabs[i].Title, { itemPos.x + 40.0f, itemPos.y }, { itemW - 48.0f, itemH }, text, UI::FontBody);
        }

        // Session status: the dot reflects whether the process handle is live.
        const float footY = m_pos.y + m_size.y - 72.0f;
        ui.DrawLine({ m_pos.x + 20.0f, footY }, { m_pos.x + kRailWidth - 20.0f, footY }, colors.BorderSubtle, 1.0f);

        bool attached = Memory::g_Memory.IsValid();
        ui.FillCircle({ m_pos.x + 24.0f, footY + 23.0f }, 3.5f, attached ? colors.Success : colors.Danger);
        std::wstring status = attached ? (L"Attached, PID " + std::to_wstring(Memory::g_Memory.GetPid())) : L"Not attached";
        ui.DrawText(status, { m_pos.x + 36.0f, footY + 13.0f }, { kRailWidth - 52.0f, 20.0f }, colors.TextSecondary, UI::FontSmall);

    }

    void MainMenu::RenderHeader() {
        auto& ui = UI::g_UI;
        const auto& colors = ui.GetColors();
        const float x0 = m_pos.x + kRailWidth + 24.0f;
        const float w = m_size.x - kRailWidth - 96.0f;

        ui.DrawText(kTabs[m_activeTab].Title, { x0, m_pos.y + 20.0f }, { w, 30.0f }, colors.TextPrimary, UI::FontTitle);

        if (ui.RenderIconButton({ m_pos.x + m_size.x - 46.0f, m_pos.y + 16.0f }, { 30.0f, 28.0f }, UI::Icon::Close)) {
            Overlay::g_Overlay.ToggleMenu();
        }
    }

    void MainMenu::RenderBody() {
        auto& ui = UI::g_UI;
        const auto& colors = ui.GetColors();

        const float x0 = m_pos.x + kRailWidth;
        const float contentW = m_size.x - kRailWidth;
        float bodyTop = m_pos.y + kHeaderHeight;
        float bodyH = m_size.y - kHeaderHeight - 12.0f;
        const auto& pages=kPages[m_activeTab];
        if(!pages.empty()) {
            const float width=(contentW-48-8*(pages.size()-1))/pages.size();
            for(int i=0;i<static_cast<int>(pages.size());++i) {
                const Math::Vector2 pos{x0+24+i*(width+8),bodyTop+2};
                const bool hovered=ui.IsHovered(pos,{width,30});
                if(hovered&&ui.IsMouseClicked()) {
                    m_pages[m_activeTab]=i;m_scrollTarget[m_activeTab]=0;m_contentHeight[m_activeTab]=0;
                    ui.CancelInputCapture();
                    ui.SetAnim(UI::HashId(L"menu.scroll",m_activeTab),0,0);
                }
                const bool active=m_pages[m_activeTab]==i;
                if(hovered)ui.FillRoundedRect(pos,{width,30},4,colors.BgElevated);
                ui.DrawText(pages[i],pos,{width,30},active?colors.TextPrimary:colors.TextSecondary,active?UI::FontBold:UI::FontBody,DWRITE_TEXT_ALIGNMENT_CENTER);
                if(active)ui.DrawLine({pos.x,bodyTop+34},{pos.x+width,bodyTop+34},colors.Accent,2);
            }
            bodyTop+=42;bodyH-=42;
        }
        const float padX = 24.0f;
        const float gap = 32.0f;
        const float colW = std::floor((contentW - padX * 2.0f - gap) * 0.5f);

        // Scroll only this tab's body; the popup owns the wheel while open.
        float maxScroll = (std::max)(0.0f, m_contentHeight[m_activeTab] - bodyH + 24.0f);
        float wheel = ui.ConsumeWheel();
        if (wheel != 0.0f && !ui.IsPopupOpen() && ui.IsHovered({ x0, bodyTop }, { contentW, bodyH })) {
            m_scrollTarget[m_activeTab] -= wheel * 56.0f;
        }
        m_scrollTarget[m_activeTab] = std::clamp(m_scrollTarget[m_activeTab], 0.0f, maxScroll);
        float scroll = ui.Animate(UI::HashId(L"menu.scroll", m_activeTab), 0, m_scrollTarget[m_activeTab], 18.0f);

        if (scroll > 0.5f) {
            ui.DrawLine({ x0, bodyTop }, { x0 + contentW, bodyTop }, colors.BorderSubtle, 1.0f);
        }

        ui.SetItemWidth(colW);
        ui.PushClip({ x0 + 1.0f, bodyTop + 1.0f }, { contentW - 2.0f, bodyH });

        float height = 0.0f;
        const float x = x0 + padX;
        const float y = bodyTop + 6.0f - scroll;
        ui.SetIdScope(UI::HashId(kTabs[m_activeTab].Title,static_cast<uint64_t>(m_pages[m_activeTab]+1)));
        switch (static_cast<Tab>(m_activeTab)) {
        case Tab::Legit: RenderCombat(m_pages[m_activeTab]==0?0:3,x,y,colW,gap,height);break;
        case Tab::Rage: {
            constexpr int combatPages[]={2,1,4};
            if(m_pages[m_activeTab]==3)RenderTabSpin(x,y,colW,gap,height);
            else RenderCombat(combatPages[m_pages[m_activeTab]],x,y,colW,gap,height);
            break;
        }
        case Tab::ESP: RenderTabVisuals(x,y,colW,gap,height);break;
        case Tab::Misc:
            if(m_pages[m_activeTab]==0)RenderTabMovement(x,y,colW,gap,height);
            else if(m_pages[m_activeTab]==1)RenderCharacter(x,y,colW,gap,height);
            else RenderTabUtilities(x,y,colW,gap,height);
            break;
        case Tab::Players: RenderTabPlayers(x,y,colW,gap,height);break;
        case Tab::Teams: RenderTabTeams(x,y,colW,gap,height);break;
        case Tab::Settings: RenderTabSettings(x,y,colW,gap,height);break;
        default: break;
        }
        ui.SetIdScope(0);
        m_contentHeight[m_activeTab] = height;

        ui.PopClip();

        if (maxScroll > 0.0f) {
            float trackH = bodyH - 8.0f;
            float thumbH = (std::max)(32.0f, trackH * bodyH / (height + 24.0f));
            float thumbY = bodyTop + 4.0f + (trackH - thumbH) * std::clamp(scroll / maxScroll, 0.0f, 1.0f);
            ui.FillRoundedRect({ m_pos.x + m_size.x - 7.0f, thumbY }, { 3.0f, thumbH }, 1.5f, colors.BorderHover);
        }
    }

    void MainMenu::RenderCombat(int page,float x, float y, float colW, float gap, float& height) {
        auto& aim=Config::g_Config.Aimbot;
        Column a{x,y,colW},b{x+colW+gap,y,colW};
        static const std::vector<std::wstring> bones={PROTECTED_TEXT(L"Head"),PROTECTED_TEXT(L"Neck"),PROTECTED_TEXT(L"Chest"),PROTECTED_TEXT(L"Root"),PROTECTED_TEXT(L"Left arm"),PROTECTED_TEXT(L"Right arm"),PROTECTED_TEXT(L"Left leg"),PROTECTED_TEXT(L"Right leg")};
        static const std::vector<std::wstring> activation={PROTECTED_TEXT(L"Hold"),PROTECTED_TEXT(L"Toggle")};
        static const std::vector<std::wstring> priorities={PROTECTED_TEXT(L"Crosshair"),PROTECTED_TEXT(L"Distance"),PROTECTED_TEXT(L"Lowest health"),PROTECTED_TEXT(L"Position delta")};
        auto mode=[&](Column& c,const std::wstring& label,bool& toggle) {
            int index=toggle?1:0;if(c.Combo(label,index,activation))toggle=index==1;
        };
        auto part=[&](Column& c,const std::wstring& label,Config::TargetBone& bone) {
            int index=static_cast<int>(bone);if(c.Combo(label,index,bones))bone=static_cast<Config::TargetBone>(index);
        };
        auto filters=[&](Column& c) {
            c.Toggle(PROTECTED_TEXT(L"Team check"),aim.TeamCheck);
            c.Toggle(PROTECTED_TEXT(L"Knock check"),aim.KnockCheck);
        };
        auto priority=[&](Column& c) {
            int index=static_cast<int>(aim.Priority);
            if(c.Combo(PROTECTED_TEXT(L"Priority"),index,priorities))aim.Priority=static_cast<Config::TargetPriority>(index);
        };
        auto lowHealth=[&](Column& c) {
            c.Toggle(PROTECTED_TEXT(L"Low-health target"),aim.LowHealthSwitch);
            if(aim.LowHealthSwitch) {
                c.Slider(PROTECTED_TEXT(L"Threshold"),aim.LowHealthThreshold,1,200,L" HP",0);
                part(c,PROTECTED_TEXT(L"Low-health part"),aim.LowHealthBone);
            }
        };
        auto smoothing=[&](Column& c,bool allAround) {
            c.Section(PROTECTED_TEXT(L"Smoothing"));
            c.Toggle(PROTECTED_TEXT(L"Smooth aim"),aim.SmoothAim);
            if(aim.AimMethod==0)c.Slider(PROTECTED_TEXT(L"Sensitivity"),aim.Sensitivity,.05f,2,L"",2);
            if(aim.SmoothAim) {
                if(aim.AimMethod==0)c.Toggle(PROTECTED_TEXT(L"Separate axes"),aim.SeparateSmoothing);
                if(aim.AimMethod==0&&aim.SeparateSmoothing) {
                    c.Slider(PROTECTED_TEXT(L"Horizontal"),aim.SmoothnessX,1,25);
                    c.Slider(PROTECTED_TEXT(L"Vertical"),aim.SmoothnessY,1,25);
                } else c.Slider(PROTECTED_TEXT(L"Amount"),aim.Smoothness,1,25);
                if(aim.AimMethod==0) {
                    static const std::vector<std::wstring> easing={PROTECTED_TEXT(L"Linear"),PROTECTED_TEXT(L"Ease in"),PROTECTED_TEXT(L"Ease out"),PROTECTED_TEXT(L"Sine")};
                    c.Combo(PROTECTED_TEXT(L"Curve"),aim.SmoothingStyle,easing);
                }
                if(!allAround)c.Toggle(PROTECTED_TEXT(L"Dynamic smoothing"),aim.DynamicSmoothing);
            }
        };
        static const std::vector<std::wstring> methods={PROTECTED_TEXT(L"Mouse"),PROTECTED_TEXT(L"Camera")};
        switch(page) {
        case 0:
            a.Section(PROTECTED_TEXT(L"General"));
            a.Toggle(PROTECTED_TEXT(L"Enabled"),aim.Enabled);
            a.Keybind(PROTECTED_TEXT(L"Keybind"),aim.Hotkey);
            mode(a,PROTECTED_TEXT(L"Activation"),aim.IsToggleMode);
            a.Combo(PROTECTED_TEXT(L"Method"),aim.AimMethod,methods);
            a.Section(PROTECTED_TEXT(L"Targeting"));
            part(a,PROTECTED_TEXT(L"Target part"),aim.Bone);priority(a);
            a.Slider(PROTECTED_TEXT(L"Range"),aim.MaxDistance,10,3000,L" studs",0);
            filters(a);a.Toggle(PROTECTED_TEXT(L"Visibility check"),aim.VisibleCheck);
            a.Toggle(PROTECTED_TEXT(L"Sticky aim"),aim.StickyAim);
            a.Toggle(PROTECTED_TEXT(L"Prediction"),aim.GlobalPrediction);
            a.Toggle(PROTECTED_TEXT(L"Resolver"),aim.Resolver);
            b.Section(PROTECTED_TEXT(L"FOV"));
            b.Toggle(PROTECTED_TEXT(L"360 aim"),aim.Aim360);
            if(!aim.Aim360) {
                b.Slider(PROTECTED_TEXT(L"Radius"),aim.FovRadius,20,400,L" px",0);
                b.ToggleColor(PROTECTED_TEXT(L"Draw FOV"),aim.DrawFov,aim.FovColor);
            }
            b.Toggle(PROTECTED_TEXT(L"Lock indicator"),aim.LockIndicator);
            b.ToggleColor(PROTECTED_TEXT(L"Target line"),aim.TargetLine,aim.TargetLineColor);
            smoothing(b,aim.Aim360);lowHealth(a);
            break;
        case 1: {
            a.Section(PROTECTED_TEXT(L"General"));
            a.Toggle(PROTECTED_TEXT(L"Enabled"),aim.Magnet);
            a.Keybind(PROTECTED_TEXT(L"Keybind"),aim.MagnetKey);
            static const std::vector<std::wstring> modes={PROTECTED_TEXT(L"Tap"),PROTECTED_TEXT(L"Spray"),PROTECTED_TEXT(L"Silent")};
            static const std::vector<std::wstring> inputs={PROTECTED_TEXT(L"Click"),PROTECTED_TEXT(L"Hold"),PROTECTED_TEXT(L"Toggle")};
            a.Combo(PROTECTED_TEXT(L"Activation"),aim.MagnetActivation,inputs);
            a.Combo(PROTECTED_TEXT(L"Mode"),aim.MagnetMode,modes);
            if(aim.MagnetMode==1)a.IntSlider(PROTECTED_TEXT(L"Max burst"),aim.MagnetMaxBurstMs,30,2000,L" ms");
            a.Section(PROTECTED_TEXT(L"FOV"));
            a.Toggle(PROTECTED_TEXT(L"360 targeting"),aim.Magnet360);
            a.Toggle(PROTECTED_TEXT(L"Visibility check"),aim.MagnetVisibleCheck);
            if(!aim.Magnet360) {
                a.Slider(PROTECTED_TEXT(L"Radius"),aim.MagnetFovRadius,20,400,L" px",0);
                a.ToggleColor(PROTECTED_TEXT(L"Draw FOV"),aim.DrawMagnetFov,aim.MagnetFovColor);
            }
            b.Section(PROTECTED_TEXT(L"Targeting"));
            part(b,PROTECTED_TEXT(L"Target part"),aim.Bone);priority(b);
            b.Slider(PROTECTED_TEXT(L"Range"),aim.MaxDistance,10,3000,L" studs",0);
            filters(b);b.Toggle(PROTECTED_TEXT(L"Sticky aim"),aim.StickyAim);
            if(aim.MagnetMode!=2) {
                a.Combo(PROTECTED_TEXT(L"Method"),aim.AimMethod,methods);
                smoothing(b,aim.Magnet360);
            }
            a.Section(PROTECTED_TEXT(L"Assists"));lowHealth(a);
            a.Toggle(PROTECTED_TEXT(L"Prediction"),aim.GlobalPrediction);
            a.Toggle(PROTECTED_TEXT(L"Resolver"),aim.Resolver);
            break;
        }
        case 2: {
            a.Section(PROTECTED_TEXT(L"General"));
            a.Toggle(PROTECTED_TEXT(L"Enabled"),aim.SilentAim);
            a.Keybind(PROTECTED_TEXT(L"Keybind"),aim.SilentHotkey);
            mode(a,PROTECTED_TEXT(L"Activation"),aim.SilentToggleMode);
            a.Toggle(PROTECTED_TEXT(L"360 flick"),aim.Silent360);
            if(!aim.Silent360) {
                static const std::vector<std::wstring> silentMethods={PROTECTED_TEXT(L"Mouse"),PROTECTED_TEXT(L"Viewport")};
                a.Combo(PROTECTED_TEXT(L"Method"),aim.SilentMethod,silentMethods);
            }
            a.Section(PROTECTED_TEXT(L"Targeting"));
            part(a,PROTECTED_TEXT(L"Target part"),aim.SilentBone);
            a.Toggle(PROTECTED_TEXT(L"Visibility check"),aim.SilentVisibleCheck);
            if(!aim.Silent360) {
                a.Slider(PROTECTED_TEXT(L"Radius"),aim.SilentFovRadius,20,400,L" px",0);
                a.ToggleColor(PROTECTED_TEXT(L"Draw FOV"),aim.DrawSilentFov,aim.SilentFovColor);
            }
            b.Section(PROTECTED_TEXT(L"Filters"));
            filters(b);b.Slider(PROTECTED_TEXT(L"Range"),aim.MaxDistance,10,3000,L" studs",0);
            b.Toggle(PROTECTED_TEXT(L"Prediction"),aim.GlobalPrediction);
            b.Toggle(PROTECTED_TEXT(L"Resolver"),aim.Resolver);lowHealth(b);
            break;
        }
        case 3:
            a.Section(PROTECTED_TEXT(L"General"));
            a.Toggle(PROTECTED_TEXT(L"Enabled"),aim.TriggerBot);
            a.Keybind(PROTECTED_TEXT(L"Keybind"),aim.TriggerHotkey);
            mode(a,PROTECTED_TEXT(L"Activation"),aim.TriggerToggleMode);
            a.Section(PROTECTED_TEXT(L"Targeting"));
            {
                static const std::vector<std::wstring> triggerBones={PROTECTED_TEXT(L"Head"),PROTECTED_TEXT(L"Neck"),PROTECTED_TEXT(L"Chest"),PROTECTED_TEXT(L"Root"),PROTECTED_TEXT(L"Any body part")};
                a.Combo(PROTECTED_TEXT(L"Target part"),aim.TriggerBone,triggerBones);
            }
            a.Slider(PROTECTED_TEXT(L"Range"),aim.TriggerBotMaxDistance,10,1000,L" studs",0);
            a.Toggle(PROTECTED_TEXT(L"Visibility check"),aim.TriggerVisibleCheck);filters(a);
            b.Section(PROTECTED_TEXT(L"Timing"));
            b.Toggle(PROTECTED_TEXT(L"Random delay"),aim.TriggerRandomDelay);
            if(aim.TriggerRandomDelay) {
                b.IntSlider(PROTECTED_TEXT(L"Minimum delay"),aim.TriggerDelayMin,0,300,L" ms");
                b.IntSlider(PROTECTED_TEXT(L"Maximum delay"),aim.TriggerDelayMax,0,300,L" ms");
                aim.TriggerDelayMax=(std::max)(aim.TriggerDelayMin,aim.TriggerDelayMax);
            } else b.IntSlider(PROTECTED_TEXT(L"Delay"),aim.TriggerBotDelayMs,0,150,L" ms");
            break;
        case 4: {
            a.Section(PROTECTED_TEXT(L"Hitbox"));
            a.Toggle(PROTECTED_TEXT(L"Enabled"),aim.HitboxExtender);
            static const std::vector<std::wstring> hitboxParts={PROTECTED_TEXT(L"Head"),PROTECTED_TEXT(L"Torso"),PROTECTED_TEXT(L"Root")};
            int partIndex=aim.HitboxPart==Config::TargetBone::Head?0:aim.HitboxPart==Config::TargetBone::Chest?1:2;
            if(a.Combo(PROTECTED_TEXT(L"Target part"),partIndex,hitboxParts))aim.HitboxPart=partIndex==0?Config::TargetBone::Head:partIndex==1?Config::TargetBone::Chest:Config::TargetBone::HumanoidRootPart;
            a.Toggle(PROTECTED_TEXT(L"No collision"),aim.HitboxNoCollision);
            a.Slider(PROTECTED_TEXT(L"Width"),aim.HitboxSize.x,2,25);
            a.Slider(PROTECTED_TEXT(L"Height"),aim.HitboxSize.y,2,25);
            a.Slider(PROTECTED_TEXT(L"Depth"),aim.HitboxSize.z,2,25);
            b.Section(PROTECTED_TEXT(L"Display"));
            b.ToggleColor(PROTECTED_TEXT(L"Visualizer"),aim.HitboxVisualizer,aim.HitboxVisualizerColor);
            b.Section(PROTECTED_TEXT(L"Filters"));filters(b);
            b.Section(PROTECTED_TEXT(L"Restore"));
            b.Toggle(PROTECTED_TEXT(L"Restore on exit"),aim.RestoreHitboxesOnExit);
            if(b.Button(PROTECTED_TEXT(L"Restore now"))) {
                aim.HitboxExtender=false;
                Features::g_Aimbot.RestoreHitboxes();
            }
            break;
        }
        }
        height=(std::max)(a.y,b.y)-y;
    }

    void MainMenu::RenderTabVisuals(float x, float y, float colW, float gap, float& height) {
        auto& vis = Config::g_Config.Visuals;

        Column a{ x, y, colW };
        a.Section(PROTECTED_TEXT(L"Players"));
        a.Toggle(PROTECTED_TEXT(L"Enabled"), vis.Enabled);
        a.Toggle(PROTECTED_TEXT(L"Health check"), vis.HealthCheck);
        bool visibleCheck=vis.VisibleCheck||vis.ColorByVisibility;
        if(a.Toggle(PROTECTED_TEXT(L"Visibility check"),visibleCheck)) {
            vis.VisibleCheck=visibleCheck;vis.ColorByVisibility=visibleCheck;
        }
        a.y+=8;
        if(visibleCheck) {
            a.Color(PROTECTED_TEXT(L"Visible box"),vis.VisibleColor);
            a.Color(PROTECTED_TEXT(L"Occluded box"),vis.HiddenColor);
        }

        static const std::vector<std::wstring> boxOptions = { PROTECTED_TEXT(L"None"), PROTECTED_TEXT(L"Box"), PROTECTED_TEXT(L"Corners"), PROTECTED_TEXT(L"3D box") };
        int boxIdx = static_cast<int>(vis.Boxes);
        if (a.ComboColor(PROTECTED_TEXT(L"Outline"), boxIdx, boxOptions, vis.BoxColor)) vis.Boxes = static_cast<Config::BoxStyle>(boxIdx);

        a.ToggleColor(PROTECTED_TEXT(L"Names"), vis.Names, vis.NameColor);
        a.ToggleColor(PROTECTED_TEXT(L"Distance"), vis.Distance, vis.DistanceColor);
        a.ToggleColor(PROTECTED_TEXT(L"Health bar"), vis.HealthDisplay, vis.HealthColor);
        a.ToggleColor(PROTECTED_TEXT(L"Health gradient"), vis.HealthBarGradient, vis.HealthLowColor);
        a.ToggleColor(PROTECTED_TEXT(L"Weapon"), vis.ToolDisplay, vis.ToolColor);
        a.ToggleColor(PROTECTED_TEXT(L"Skeleton"), vis.SkeletonESP, vis.SkeletonColor);
        a.ToggleColor(PROTECTED_TEXT(L"Head dot"), vis.HeadDot, vis.HeadDotColor);
        a.ToggleColor(PROTECTED_TEXT(L"View direction"), vis.LookDirection, vis.LookDirectionColor);
        a.ToggleColor(PROTECTED_TEXT(L"Off-screen"), vis.OffScreenArrows, vis.OffScreenColor);

        static const std::vector<std::wstring> snaplineOptions = { PROTECTED_TEXT(L"Off"), PROTECTED_TEXT(L"From bottom"), PROTECTED_TEXT(L"From center"), PROTECTED_TEXT(L"From top") };
        int snapIdx = static_cast<int>(vis.Snaplines);
        if (a.ComboColor(PROTECTED_TEXT(L"Tracers"), snapIdx, snaplineOptions, vis.SnaplineColor)) vis.Snaplines = static_cast<Config::SnaplineOrigin>(snapIdx);

        Column b{ x + colW + gap, y, colW };
        b.Section(PROTECTED_TEXT(L"Preview"));
        RenderLiveESPPreview({ b.x, b.y }, { colW, 220.0f });
        b.y += 232.0f;

        static const std::vector<std::wstring> presetOptions = { PROTECTED_TEXT(L"Custom"), PROTECTED_TEXT(L"Legit"), PROTECTED_TEXT(L"Rage"), PROTECTED_TEXT(L"Minimal"), PROTECTED_TEXT(L"Tournament") };
        int presetIdx = static_cast<int>(vis.Preset);
        if (b.Combo(PROTECTED_TEXT(L"Preset"), presetIdx, presetOptions)) Config::g_Config.ApplyPreset(static_cast<Config::ESPPreset>(presetIdx));

        b.Section(PROTECTED_TEXT(L"Radar"));
        b.ToggleColor(PROTECTED_TEXT(L"Mini radar"), vis.Radar, vis.RadarColor);
        b.Slider(PROTECTED_TEXT(L"Range"), vis.RadarRange, 100.0f, 600.0f, L" m", 0);

        b.Section(PROTECTED_TEXT(L"Trails"));
        b.ToggleColor(PROTECTED_TEXT(L"Motion trails"), vis.PlayerTrails, vis.TrailColor);
        b.IntSlider(PROTECTED_TEXT(L"Trail length"), vis.MaxTrailPoints, 5, 50, L" pts");
        b.Section(PROTECTED_TEXT(L"Details"));
        b.ToggleColor(PROTECTED_TEXT(L"Filled boxes"),vis.FillBox,vis.FillColor);
        b.Toggle(PROTECTED_TEXT(L"Health numbers"),vis.HealthText);
        b.Slider(PROTECTED_TEXT(L"Render distance"),vis.RenderDistance,10,10000,L" studs",0);
        b.ToggleColor(PROTECTED_TEXT(L"Damage numbers"),vis.DamageNumbers,vis.DamageColor);
        b.Slider(PROTECTED_TEXT(L"Damage lifetime"),vis.DamageDuration,.2f,4,L" s",1);
        b.Toggle(PROTECTED_TEXT(L"Enemy counter"),vis.EnemyCounter);
        b.Toggle(PROTECTED_TEXT(L"Knock check"),vis.KnockCheck);
        b.Section(PROTECTED_TEXT(L"Radar options"));
        b.Toggle(PROTECTED_TEXT(L"Rotate radar with camera"),vis.RadarRotate);
        b.Toggle(PROTECTED_TEXT(L"Radar facing directions"),vis.RadarLooking);
        b.Slider(PROTECTED_TEXT(L"Radar dot size"),vis.RadarDotSize,1,8,L" px",1);
        b.Slider(PROTECTED_TEXT(L"Radar panel size"),vis.RadarSize,100,350,L" px",0);

        height = (std::max)(a.y, b.y) - y;
    }

    void MainMenu::RenderLiveESPPreview(const Math::Vector2& pos, const Math::Vector2& size) {
        auto& ui = UI::g_UI;
        const auto& colors = ui.GetColors();
        const auto& vis = Config::g_Config.Visuals;

        ui.FillRoundedRect(pos, size, 8.0f, colors.BgBase);
        ui.DrawRoundedRect(pos, size, 8.0f, colors.BorderSubtle, 1.0f);

        Math::Vector2 center{ pos.x + size.x * 0.5f, pos.y + size.y * 0.52f };
        const float bodyH = 120.0f;
        const float bodyW = 52.0f;
        Math::Vector2 tl{ center.x - bodyW * 0.5f, center.y - bodyH * 0.5f };
        Math::Vector2 br{ tl.x + bodyW, tl.y + bodyH };
        Math::Vector2 head{ center.x, tl.y + 14.0f };

        // Neutral stand-in figure so the toggles have something to sit on.
        D2D1_COLOR_F figure = UI::WithAlpha(colors.TextPrimary, 0.07f);
        ui.FillCircle(head, 10.0f, figure);
        ui.FillRoundedRect({ center.x - 16.0f, tl.y + 28.0f }, { 32.0f, 46.0f }, 6.0f, figure);
        ui.FillRoundedRect({ center.x - 15.0f, tl.y + 76.0f }, { 13.0f, 44.0f }, 4.0f, figure);
        ui.FillRoundedRect({ center.x + 2.0f, tl.y + 76.0f }, { 13.0f, 44.0f }, 4.0f, figure);

        if (!vis.Enabled) {
            ui.DrawText(PROTECTED_TEXT(L"Player visuals are off"), { pos.x, pos.y + size.y - 30.0f }, { size.x, 20.0f }, colors.TextTertiary, UI::FontSmall, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }

        const D2D1_COLOR_F box = UI::FromColor((vis.VisibleCheck||vis.ColorByVisibility)?vis.VisibleColor:vis.BoxColor);
        if (vis.Snaplines != Config::SnaplineOrigin::None) {
            float sy = vis.Snaplines == Config::SnaplineOrigin::ScreenTop ? pos.y
                     : vis.Snaplines == Config::SnaplineOrigin::ScreenCenter ? pos.y + size.y * 0.5f
                     : pos.y + size.y;
            Math::Vector2 target = vis.Snaplines == Config::SnaplineOrigin::ScreenTop ? Math::Vector2{ center.x, tl.y } : Math::Vector2{ center.x, br.y };
            if (vis.Snaplines == Config::SnaplineOrigin::ScreenCenter) target = { br.x, center.y };
            ui.DrawLine({ vis.Snaplines == Config::SnaplineOrigin::ScreenCenter ? pos.x + size.x : center.x, sy }, target, UI::FromColor(vis.SnaplineColor), 1.0f);
        }

        switch (vis.Boxes) {
        case Config::BoxStyle::Corner2D: {
            const float c = 12.0f;
            ui.DrawLine(tl, { tl.x + c, tl.y }, box, 1.5f);
            ui.DrawLine(tl, { tl.x, tl.y + c }, box, 1.5f);
            ui.DrawLine({ br.x, tl.y }, { br.x - c, tl.y }, box, 1.5f);
            ui.DrawLine({ br.x, tl.y }, { br.x, tl.y + c }, box, 1.5f);
            ui.DrawLine({ tl.x, br.y }, { tl.x + c, br.y }, box, 1.5f);
            ui.DrawLine({ tl.x, br.y }, { tl.x, br.y - c }, box, 1.5f);
            ui.DrawLine(br, { br.x - c, br.y }, box, 1.5f);
            ui.DrawLine(br, { br.x, br.y - c }, box, 1.5f);
            break;
        }
        case Config::BoxStyle::Box2D:
            ui.DrawRect(tl, { bodyW, bodyH }, box, 1.5f);
            break;
        case Config::BoxStyle::Box3D: {
            const Math::Vector2 d{ 10.0f, -8.0f };
            ui.DrawRect(tl, { bodyW, bodyH }, box, 1.2f);
            ui.DrawRect({ tl.x + d.x, tl.y + d.y }, { bodyW, bodyH }, UI::WithAlpha(box, box.a*.6f), 1.2f);
            ui.DrawLine(tl, { tl.x + d.x, tl.y + d.y }, UI::WithAlpha(box, box.a*.6f), 1.2f);
            ui.DrawLine({ br.x, tl.y }, { br.x + d.x, tl.y + d.y }, UI::WithAlpha(box, box.a*.6f), 1.2f);
            ui.DrawLine({ tl.x, br.y }, { tl.x + d.x, br.y + d.y }, UI::WithAlpha(box, box.a*.6f), 1.2f);
            ui.DrawLine(br, { br.x + d.x, br.y + d.y }, UI::WithAlpha(box, box.a*.6f), 1.2f);
            break;
        }
        default: break;
        }

        if (vis.SkeletonESP) {
            D2D1_COLOR_F skel = UI::FromColor(vis.SkeletonColor);
            Math::Vector2 neck{ center.x, tl.y + 28.0f };
            Math::Vector2 hip{ center.x, tl.y + 74.0f };
            ui.DrawLine(neck, hip, skel, 1.2f);
            ui.DrawLine({ center.x - 22.0f, tl.y + 58.0f }, { center.x - 12.0f, tl.y + 32.0f }, skel, 1.2f);
            ui.DrawLine({ center.x - 12.0f, tl.y + 32.0f }, { center.x + 12.0f, tl.y + 32.0f }, skel, 1.2f);
            ui.DrawLine({ center.x + 12.0f, tl.y + 32.0f }, { center.x + 22.0f, tl.y + 58.0f }, skel, 1.2f);
            ui.DrawLine(hip, { center.x - 9.0f, br.y - 2.0f }, skel, 1.2f);
            ui.DrawLine(hip, { center.x + 9.0f, br.y - 2.0f }, skel, 1.2f);
        }
        if (vis.HeadDot) ui.FillCircle(head, 3.5f, UI::FromColor(vis.HeadDotColor));
        if (vis.LookDirection) ui.DrawLine(head, { head.x + 30.0f, head.y - 8.0f }, UI::FromColor(vis.LookDirectionColor), 1.5f);

        if (vis.HealthDisplay) {
            Math::Vector2 bar{ tl.x - 7.0f, tl.y };
            ui.FillRect(bar, { 3.0f, bodyH }, colors.BgElevated);
            ui.FillRect({ bar.x, bar.y + bodyH * 0.25f }, { 3.0f, bodyH * 0.75f }, vis.HealthBarGradient?UI::Mix(UI::FromColor(vis.HealthLowColor),UI::FromColor(vis.HealthColor),.75f):UI::FromColor(vis.HealthColor));
        }

        if (vis.Names || vis.Distance) {
            std::wstring name=vis.Names?L"Player":L"";
            std::wstring distance=vis.Distance?L" [42 studs]":L"";
            float nameWidth=ui.MeasureText(name,UI::FontSmall), distanceWidth=ui.MeasureText(distance,UI::FontSmall);
            float textX=center.x-(nameWidth+distanceWidth)*.5f;
            ui.DrawText(name,{textX,tl.y-20},{nameWidth+2,16},UI::FromColor(vis.NameColor),UI::FontSmall);
            ui.DrawText(distance,{textX+nameWidth,tl.y-20},{distanceWidth+2,16},UI::FromColor(vis.DistanceColor),UI::FontSmall);
        }
        if (vis.ToolDisplay) {
            ui.DrawText(PROTECTED_TEXT(L"Sword"), { tl.x - 40.0f, br.y + 4.0f }, { bodyW + 80.0f, 16.0f }, UI::FromColor(vis.ToolColor), UI::FontSmall, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    void MainMenu::RenderTabTeams(float x,float y,float colW,float gap,float& height) {
        auto& ui=UI::g_UI;
        const auto& colors=ui.GetColors();
        auto& config=Config::g_Config.Teams;
        auto teams=TeamCheck::g_Resolver.Teams();
#ifdef CHEAT_TEST_STORAGE
        if(!m_previewTeams.empty())teams=m_previewTeams;
#endif
        auto widen=[](const std::string& value) {
            const int size=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);
            std::wstring result(size,L' ');
            if(size)MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),size);
            return result;
        };
        auto lower=[](std::wstring text) {for(auto& c:text)c=static_cast<wchar_t>(std::towlower(c));return text;};
        auto priority=[&](const TeamCheck::TeamInfo& team) {
            const auto relation=TeamCheck::ApplyPolicy(team.Automatic,team.Key,config);
            return relation==TeamCheck::Relation::Enemy?2:relation==TeamCheck::Relation::Teammate?1:0;
        };
        auto set=[&](const std::string& key,int value) {
            if(key.size()<=512&&(config.Overrides.count(key)||config.Overrides.size()<128)) {
                config.Overrides[key]=value;config.Enabled=true;
            } else Features::g_Utilities.PushNotification("Teams","Saved team limit reached. Reset unused rules in a config.");
        };
        const std::vector<std::wstring> priorities={L"Neutral",L"Ally",L"Enemy"};
        Column a{x,y,colW},b{x+colW+gap,y,colW};
        a.Section(PROTECTED_TEXT(L"Teams"),std::to_wstring(teams.size()));
        ui.RenderTextInput({a.x,a.y},PROTECTED_TEXT(L"Search teams"),m_teamSearch);a.y+=60;
        a.Toggle(PROTECTED_TEXT(L"Custom teams"),config.Enabled);
        a.Toggle(PROTECTED_TEXT(L"Auto-detect allies"),config.AutoDetectAllies);
        a.y+=10;
        const auto search=lower(m_teamSearch);
        const TeamCheck::TeamInfo* selected=nullptr;
        size_t matches=0;
        for(const auto& team:teams) {
            if(team.Key==m_selectedTeam)selected=&team;
            if(lower(widen(team.Name)).find(search)==std::wstring::npos)continue;
            ++matches;
            if(m_selectedTeam.empty()){m_selectedTeam=team.Key;selected=&team;}
            const bool active=m_selectedTeam==team.Key;
            const bool hovered=ui.IsHovered({a.x,a.y},{colW,48});
            if(active||hovered)ui.FillRoundedRect({a.x,a.y},{colW,48},6,active?colors.BgElevated:colors.BgSurface);
            if(hovered&&ui.IsMouseClicked()){m_selectedTeam=team.Key;selected=&team;}
            const int state=priority(team);
            const auto color=state==2?colors.Danger:state==1?colors.Success:colors.TextTertiary;
            ui.DrawText(widen(team.Name),{a.x+10,a.y+3},{colW-88,22},colors.TextPrimary,UI::FontBold);
            ui.DrawText(priorities[state],{a.x+colW-72,a.y+3},{62,22},color,UI::FontBody,DWRITE_TEXT_ALIGNMENT_TRAILING);
            const auto meta=(team.IsLocal?L"Your team  /  ":L"")+std::to_wstring(team.Members)+L" members";
            ui.DrawText(meta,{a.x+10,a.y+25},{colW-20,18},colors.TextSecondary,UI::FontSmall);
            a.y+=52;
        }
        if(!matches) {
            a.State(teams.empty()?L"No teams detected.":L"No matching teams.");
            a.State(teams.empty()?L"Join an experience.":L"Try another name.");
        }
        if(!selected&&!m_selectedTeam.empty())m_selectedTeam.clear();
        b.Section(PROTECTED_TEXT(L"Selection"));
        if(selected) {
            ui.DrawText(widen(selected->Name),{b.x,b.y},{colW,24},colors.TextPrimary,UI::FontBold);b.y+=30;
            int state=priority(*selected);
            if(b.Combo(PROTECTED_TEXT(L"Priority"),state,priorities))set(selected->Key,state);
            b.y+=8;
            b.State(state==2?L"Enemy: ESP and aim.":state==1?L"Ally: excluded.":L"Neutral: excluded.");
            if(b.Button(PROTECTED_TEXT(L"Reset team")))config.Overrides.erase(selected->Key);
        } else b.State(PROTECTED_TEXT(L"Select a team."));
        b.Section(PROTECTED_TEXT(L"All teams"));
        if(b.Button(PROTECTED_TEXT(L"All enemy"),false,!teams.empty()))for(const auto& team:teams)set(team.Key,2);
        if(b.Button(PROTECTED_TEXT(L"All ally"),false,!teams.empty()))for(const auto& team:teams)set(team.Key,1);
        if(b.Button(PROTECTED_TEXT(L"Reset rules"))) {config=Config::TeamsConfig{};}
        b.y+=8;
        height=(std::max)(a.y,b.y)-y;
    }

    void MainMenu::RenderTabPlayers(float x, float y, float colW, float gap, float& height) {
        auto& ui = UI::g_UI;
        const auto& colors = ui.GetColors();
        const auto& players = Features::g_Visuals.GetCachedPlayers();
        auto& whitelist = Config::g_Config.Players.WhitelistedUsers;

        Column a{ x, y, colW };
        a.Section(PROTECTED_TEXT(L"Players"), std::to_wstring(players.size()));

        if (players.empty()) {
            ui.DrawText(PROTECTED_TEXT(L"No players"), { a.x, a.y }, { colW, 20.0f }, colors.TextSecondary, UI::FontBody);
            a.y += 22.0f;
        }

        for (size_t i = 0; i < players.size(); ++i) {
            const auto& p = players[i];
            const float rowY = a.y;

            if (i > 0) ui.DrawLine({ a.x, rowY - 6.0f }, { a.x + colW, rowY - 6.0f }, colors.BorderSubtle, 1.0f);

            std::wstringstream meta;
            meta << static_cast<int>(p.Distance) << L" m  " << static_cast<int>(p.Health) << L" hp";
            ui.DrawText(Widen(p.Name), { a.x, rowY }, { colW - 100.0f, 20.0f }, colors.TextPrimary, UI::FontBold);
            ui.DrawText(meta.str(), { a.x + colW - 100.0f, rowY }, { 100.0f, 20.0f }, colors.TextSecondary, UI::FontMono, DWRITE_TEXT_ALIGNMENT_TRAILING);

            // Row actions, sized to their labels.
            float bx = a.x;
            const float by = rowY + 26.0f;
            auto action = [&](const std::wstring& label, bool enabled = true) {
                float w = ui.MeasureText(label, UI::FontBold) + 22.0f;
                bool clicked = ui.RenderButton({ bx, by }, { w, 26.0f }, label, false, enabled);
                bx += w + 6.0f;
                return clicked;
            };

            if (action(PROTECTED_TEXT(L"Teleport"))) {
                SDK::Player target = p.Target;
                Features::g_Players.TeleportToPlayer(target);
            }
            if (action(PROTECTED_TEXT(L"Fling"))) {
                SDK::Player target = p.Target;
                Features::g_Players.FlingPlayer(target);
            }
            if (action(PROTECTED_TEXT(L"Spectate"))) {
                SDK::Player target = p.Target;
                Features::g_Players.SpectatePlayer(target);
            }
            bool listed = whitelist.count(p.UserId) > 0;
            if (action(listed ? L"Whitelisted" : L"Whitelist", !listed)) {
                whitelist.insert(p.UserId);
                Features::g_Utilities.PushNotification("Player whitelisted", "Aim will skip this player.", 3.0f);
            }

            a.y = rowY + 66.0f;
        }

        Column b{ x + colW + gap, y, colW };
        b.Section(PROTECTED_TEXT(L"Server"));
        if (b.Button(PROTECTED_TEXT(L"Rejoin"))) Features::g_Players.RejoinServer();
        if (b.Button(PROTECTED_TEXT(L"Server hop"))) Features::g_Players.ServerHop();
        if (b.Button(PROTECTED_TEXT(L"Copy server info"))) Features::g_Players.CopyServerInfo();
        if (b.Button(PROTECTED_TEXT(L"Reset spectator"))) Features::g_Players.ResetSpectate();

        const auto& log = Features::g_Players.GetActivityLog();
        b.Section(PROTECTED_TEXT(L"Activity"));
        if (log.empty()) {
            b.State(PROTECTED_TEXT(L"No activity"));
        }
        for (const auto& entry : log) {
            D2D1_COLOR_F dot{ entry.Color.r, entry.Color.g, entry.Color.b, 1.0f };
            ui.FillCircle({ b.x + 3.0f, b.y + 11.0f }, 3.0f, dot);
            ui.DrawText(Widen(entry.Timestamp), { b.x + 14.0f, b.y }, { 64.0f, 22.0f }, colors.TextTertiary, UI::FontMono);
            ui.DrawText(Widen(entry.Message), { b.x + 80.0f, b.y }, { colW - 80.0f, 22.0f }, colors.TextSecondary, UI::FontSmall);
            b.y += 24.0f;
        }

        height = (std::max)(a.y, b.y) - y;
    }

    void MainMenu::RenderTabUtilities(float x, float y, float colW, float gap, float& height) {
        auto& util = Config::g_Config.Utilities;

        Column a{ x, y, colW };
        a.Section(PROTECTED_TEXT(L"Session"));
        a.Toggle(PROTECTED_TEXT(L"Anti-AFK"), util.AntiAFK);
        a.IntSlider(PROTECTED_TEXT(L"AFK interval"), util.AntiAFKIntervalSec, 30, 300, L" s");
        a.Toggle(PROTECTED_TEXT(L"FPS unlock"), util.FPSUnlocker);
        a.IntSlider(PROTECTED_TEXT(L"Frame rate cap"), util.TargetFPS, 60, 360, L" fps");
        a.Toggle(PROTECTED_TEXT(L"Notifications"), util.Notifications);
        a.Toggle(PROTECTED_TEXT(L"Keybinds"),util.KeybindList);

        Column b{ x + colW + gap, y, colW };
        b.Section(PROTECTED_TEXT(L"Crosshair"));
        b.Toggle(PROTECTED_TEXT(L"Enabled"), util.CrosshairBuilder);
        b.Toggle(PROTECTED_TEXT(L"Rainbow"), util.CrosshairRainbow);
        static const std::vector<std::wstring> crosshairStyles = { PROTECTED_TEXT(L"Cross"), PROTECTED_TEXT(L"Dot"), PROTECTED_TEXT(L"Circle"), PROTECTED_TEXT(L"T-shape") };
        b.Combo(PROTECTED_TEXT(L"Shape"), util.CrosshairStyle, crosshairStyles);
        b.Slider(PROTECTED_TEXT(L"Length"), util.CrosshairSize, 2.0f, 25.0f, L" px");
        b.Slider(PROTECTED_TEXT(L"Gap"), util.CrosshairGap, 0.0f, 15.0f, L" px");
        b.Slider(PROTECTED_TEXT(L"Thickness"), util.CrosshairThickness, 1.0f, 4.0f, L" px");
        b.Toggle(PROTECTED_TEXT(L"Center dot"), util.CrosshairDot);
        b.Toggle(PROTECTED_TEXT(L"Dynamic spread"), util.CrosshairDynamicSpread);
        b.Toggle(PROTECTED_TEXT(L"Rotation"),util.CrosshairSpin);
        if(util.CrosshairSpin)b.Slider(PROTECTED_TEXT(L"Rotation speed"),util.CrosshairSpinSpeed,1,720,L" deg/s",0);
        b.Toggle(PROTECTED_TEXT(L"Pulse"),util.CrosshairPulse);
        if(util.CrosshairPulse)b.Slider(PROTECTED_TEXT(L"Pulse speed"),util.CrosshairPulseSpeed,.1f,10,L" Hz",1);
        b.y += 10.0f;
        RenderCrosshairPreview({ b.x, b.y }, { colW, 96.0f });
        b.y += 96.0f;

        height = (std::max)(a.y, b.y) - y;
    }

    void MainMenu::RenderCrosshairPreview(const Math::Vector2& pos, const Math::Vector2& size) {
        const auto& colors = UI::g_UI.GetColors();
        UI::g_UI.FillRoundedRect(pos, size, 8.0f, colors.BgBase);
        UI::g_UI.DrawRoundedRect(pos, size, 8.0f, colors.BorderSubtle, 1.0f);
        Math::Vector2 center{ pos.x + size.x * 0.5f, pos.y + size.y * 0.5f };
        if (Config::g_Config.Utilities.CrosshairBuilder) {
            DrawCrosshair(center, colors.Accent);
        } else {
            UI::g_UI.DrawText(PROTECTED_TEXT(L"Disabled"), pos, size, colors.TextTertiary, UI::FontSmall, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    void MainMenu::RefreshProfiles(const std::filesystem::path& select) {
        const auto previous=m_profileSelection>=0&&m_profileSelection<static_cast<int>(m_profiles.size())?m_profiles[m_profileSelection].Path:std::filesystem::path{};
        Profiles::Store store(Profiles::Directory());std::string error;
        m_profiles=store.List(error);m_defaultProfile=store.Default();m_profilesLoaded=true;m_profileSelection=-1;
        const auto preferred=!select.empty()?select:!previous.empty()?previous:m_defaultProfile;
        for(size_t i=0;i<m_profiles.size();++i)if(m_profiles[i].Path==preferred)m_profileSelection=static_cast<int>(i);
        if(m_profileSelection<0&&!m_profiles.empty())m_profileSelection=0;
        if(m_profileSelection>=0)m_profileName=m_profiles[m_profileSelection].Name;
        if(!error.empty())Features::g_Utilities.PushNotification("Configs",error,4);
    }

    void MainMenu::RenderTabSettings(float x, float y, float colW, float gap, float& height) {
        auto& custom = Config::g_Config.Customization;

        Column a{ x, y, colW };
        a.Section(PROTECTED_TEXT(L"Appearance"));
        static const std::vector<std::wstring> themeOptions = { PROTECTED_TEXT(L"Violet"), PROTECTED_TEXT(L"Crimson"), PROTECTED_TEXT(L"Cyan"), PROTECTED_TEXT(L"Emerald"), PROTECTED_TEXT(L"Mono") };
        int themeIdx = static_cast<int>(custom.Theme);
        if (a.Combo(PROTECTED_TEXT(L"Accent"), themeIdx, themeOptions)) {
            custom.Theme = static_cast<Config::AccentTheme>(themeIdx);
            UI::g_UI.UpdateTheme(custom.Theme);
        }
        a.Toggle(PROTECTED_TEXT(L"Streamproof"), custom.StreamProof);
        a.Toggle(PROTECTED_TEXT(L"Watermark"), custom.Watermark);
        a.Toggle(PROTECTED_TEXT(L"Reduced motion"), custom.ReducedMotion);

        Column b{ x + colW + gap, y, colW };
        b.Section(PROTECTED_TEXT(L"Hotkeys"));
        b.Keybind(PROTECTED_TEXT(L"Menu"), custom.MenuHotkey);
        b.Keybind(PROTECTED_TEXT(L"Unload"), custom.PanicHotkey);

        b.Section(PROTECTED_TEXT(L"Configs"));
        if(!m_profilesLoaded)RefreshProfiles();
        Profiles::Store store(Profiles::Directory());
        std::vector<std::wstring> names;for(const auto& entry:m_profiles)names.push_back(entry.Name);
        if(names.empty()){int none=0;b.Combo(PROTECTED_TEXT(L"Profile"),none,{L"None"});}
        else if(b.Combo(PROTECTED_TEXT(L"Profile"),m_profileSelection,names)&&m_profileSelection>=0)m_profileName=m_profiles[m_profileSelection].Name;
        UI::g_UI.RenderTextInput({b.x,b.y},L"Name",m_profileName);b.y+=60;
        const bool selected=m_profileSelection>=0&&m_profileSelection<static_cast<int>(m_profiles.size());
        const auto path=selected?m_profiles[m_profileSelection].Path:std::filesystem::path{};
        auto result=[&](bool ok,const std::string& success,const std::string& error) {
            Features::g_Utilities.PushNotification(ok?"Config updated":"Config failed",ok?success:error,4);
        };
        if(b.Button(PROTECTED_TEXT(L"Create"),true,!m_profileName.empty())) {
            std::string error;std::filesystem::path created;
            bool ok=store.Create(m_profileName,Config::g_Config,created,error);
            if(ok)RefreshProfiles(created);
            result(ok,"Profile created.",error);
        }
        if(b.Button(PROTECTED_TEXT(L"Rename"),false,selected&&!m_profileName.empty())) {
            std::string error;std::filesystem::path renamed;
            bool ok=store.Rename(path,m_profileName,renamed,error);
            if(ok)RefreshProfiles(renamed);
            result(ok,"Config renamed.",error);
        }
        if(b.Button(PROTECTED_TEXT(L"Save"),false,selected)) {
            std::string error;bool ok=Profiles::Save(path,Config::g_Config,error);
            result(ok,"Profile saved.",error);
        }
        if(b.Button(PROTECTED_TEXT(L"Load"),false,selected)) {
            std::string error;bool ok=Profiles::Load(path,Config::g_Config,error);
            if(ok){Features::g_Aimbot.ResetActivation();Features::g_LocalEffects.Stop();UI::g_UI.UpdateTheme(Config::g_Config.Customization.Theme);}
            result(ok,"Profile loaded.",error);
        }
        bool loadDefault=selected&&path==m_defaultProfile;
        if(selected&&b.Toggle(PROTECTED_TEXT(L"Load on startup"),loadDefault)) {
            std::string error;bool ok=store.SetDefault(loadDefault?path:std::filesystem::path{},error);
            if(ok)m_defaultProfile=store.Default();
            result(ok,loadDefault?"Startup profile set.":"Startup profile cleared.",error);
        }
        b.State(m_defaultProfile.empty()?L"Default: none":L"Default: "+m_defaultProfile.stem().wstring());
        if(b.Button(PROTECTED_TEXT(L"Refresh")))RefreshProfiles();

        height = (std::max)(a.y, b.y) - y;
    }


    void MainMenu::RenderTabMovement(float x,float y,float colW,float gap,float& height) {
        auto& c=Config::g_Config.Movement;
        Column a{x,y,colW}, b{x+colW+gap,y,colW};
        static const std::vector<std::wstring> modes={PROTECTED_TEXT(L"Hold"),PROTECTED_TEXT(L"Toggle")};
        a.Section(PROTECTED_TEXT(L"Speed"));
        a.Toggle(PROTECTED_TEXT(L"Speed"),c.Speed);
        static const std::vector<std::wstring> methods={PROTECTED_TEXT(L"Velocity"),PROTECTED_TEXT(L"WalkSpeed")};
        a.Combo(PROTECTED_TEXT(L"Speed method"),c.SpeedMethod,methods);
        a.Slider(PROTECTED_TEXT(L"Speed value"),c.SpeedValue,1,300,L" studs/s",0);
        a.Keybind(PROTECTED_TEXT(L"Speed key"),c.SpeedKey);
        int mode=c.SpeedToggle?1:0;
        if(a.Combo(PROTECTED_TEXT(L"Speed activation"),mode,modes))c.SpeedToggle=mode==1;
        a.Section(PROTECTED_TEXT(L"Jump"));
        a.Toggle(PROTECTED_TEXT(L"Jump override"),c.Jump);
        a.Slider(PROTECTED_TEXT(L"Jump power"),c.JumpPower,1,300,L"",0);
        b.Section(PROTECTED_TEXT(L"Flight"));
        b.Toggle(PROTECTED_TEXT(L"Flight"),c.Fly);
        static const std::vector<std::wstring> flyMethods={PROTECTED_TEXT(L"Velocity"),PROTECTED_TEXT(L"Position")};
        b.Combo(PROTECTED_TEXT(L"Flight method"),c.FlyMethod,flyMethods);
        b.Slider(PROTECTED_TEXT(L"Flight speed"),c.FlySpeed,1,300,L" studs/s",0);
        b.Keybind(PROTECTED_TEXT(L"Flight key"),c.FlyKey);
        mode=c.FlyToggle?1:0;
        if(b.Combo(PROTECTED_TEXT(L"Flight activation"),mode,modes))c.FlyToggle=mode==1;
        b.Section(PROTECTED_TEXT(L"Gravity"));
        b.Toggle(PROTECTED_TEXT(L"Gravity"),c.Gravity);
        b.Slider(PROTECTED_TEXT(L"Gravity value"),c.GravityValue,0,500,L"",1);
        b.Section(PROTECTED_TEXT(L"Physics"));
        b.Toggle(PROTECTED_TEXT(L"Tickrate override"),c.PhysicsTickrate);
        b.Slider(PROTECTED_TEXT(L"Tickrate"),c.PhysicsTickrateValue,30,500,L" Hz",0);
        b.Section(PROTECTED_TEXT(L"Teleport behind"));
        b.Toggle(PROTECTED_TEXT(L"Teleport behind"),c.TeleportBehind);
        b.Keybind(PROTECTED_TEXT(L"Teleport key"),c.TeleportKey);
        b.Slider(PROTECTED_TEXT(L"Behind distance"),c.TeleportDistance,1,20,L" studs",1);
        b.Slider(PROTECTED_TEXT(L"Target range"),c.TeleportRange,10,1000,L" studs",0);
        height=(std::max)(a.y,b.y)-y;
    }

    void MainMenu::RenderCharacter(float x,float y,float colW,float gap,float& height) {
        auto& c=Config::g_Config.Rage;
        Column a{x,y,colW}, b{x+colW+gap,y,colW};
        static const std::vector<std::wstring> modes={PROTECTED_TEXT(L"Hold"),PROTECTED_TEXT(L"Toggle")};
        a.Section(PROTECTED_TEXT(L"Collision"));
        a.Toggle(PROTECTED_TEXT(L"Noclip"),c.Noclip);
        a.Keybind(PROTECTED_TEXT(L"Noclip key"),c.NoclipKey);
        int mode=c.NoclipToggle?1:0;
        if(a.Combo(PROTECTED_TEXT(L"Noclip activation"),mode,modes))c.NoclipToggle=mode==1;
        a.Section(PROTECTED_TEXT(L"Character"));
        a.Toggle(PROTECTED_TEXT(L"Hip override"),c.HipHeight);
        a.Slider(PROTECTED_TEXT(L"Hip height"),c.HipHeightValue,0,30,L" studs",1);
        b.Section(PROTECTED_TEXT(L"Orbit"));
        b.Toggle(PROTECTED_TEXT(L"Orbit"),c.Orbit);
        b.Keybind(PROTECTED_TEXT(L"Orbit key"),c.OrbitKey);
        mode=c.OrbitToggle?1:0;
        if(b.Combo(PROTECTED_TEXT(L"Orbit activation"),mode,modes))c.OrbitToggle=mode==1;
        b.Slider(PROTECTED_TEXT(L"Orbit radius"),c.OrbitRadius,1,50,L" studs",1);
        b.Slider(PROTECTED_TEXT(L"Orbit height"),c.OrbitHeight,-10,30,L" studs",1);
        b.Slider(PROTECTED_TEXT(L"Orbit rate"),c.OrbitRate,.1f,5,L" turns/s",1);
        height=(std::max)(a.y,b.y)-y;
    }

    void MainMenu::RenderTabSpin(float x,float y,float colW,float gap,float& height) {
        auto& c=Config::g_Config.Rage;
        Column a{x,y,colW},b{x+colW+gap,y,colW};
        a.Section(PROTECTED_TEXT(L"General"));
        a.Toggle(PROTECTED_TEXT(L"Enabled"),c.Spin);
        a.Keybind(PROTECTED_TEXT(L"Keybind"),c.SpinKey);
        int activation=c.SpinToggle?1:0;
        if(a.Combo(PROTECTED_TEXT(L"Activation"),activation,{L"Hold",L"Toggle"}))c.SpinToggle=activation==1;
        a.Slider(PROTECTED_TEXT(L"Rate"),c.SpinRate,.1f,10,L" cycles/s",1);
        b.Section(PROTECTED_TEXT(L"Presets"));
        if(b.Button(PROTECTED_TEXT(L"Continuous"))) {c.StandingSpin={};c.MovingSpin={};}
        if(b.Button(PROTECTED_TEXT(L"Jitter / sway"))) {
            c.StandingSpin={};c.MovingSpin={};c.StandingSpin.YawMode=2;c.MovingSpin.YawMode=3;
        }
        if(b.Button(PROTECTED_TEXT(L"Reset spin"))) {
            c.Spin=false;c.SpinRate=1;c.SpinKey='G';c.SpinToggle=true;c.StandingSpin={};c.MovingSpin={};
        }
        const float start=(std::max)(a.y,b.y)+22;
        Column standing{x,start,colW},moving{x+colW+gap,start,colW};
        const std::vector<std::wstring> yaw={L"None",L"Spin",L"Jitter",L"Sway",L"Random",L"Backwards"};
        const std::vector<std::wstring> pitch={L"None",L"Up",L"Down",L"Jitter",L"Sway"};
        auto profile=[&](Column& column,const std::wstring& name,Config::SpinProfile& settings) {
            column.Section(name);
            UI::g_UI.SetIdScope(UI::HashId(name));
            column.Combo(PROTECTED_TEXT(L"Pitch"),settings.PitchMode,pitch);
            if(settings.PitchMode)column.Slider(PROTECTED_TEXT(L"Pitch angle"),settings.PitchAngle,0,80,L" deg",0);
            column.Combo(PROTECTED_TEXT(L"Yaw"),settings.YawMode,yaw);
            if(settings.YawMode>=2&&settings.YawMode<=4)column.Slider(PROTECTED_TEXT(L"Yaw range"),settings.YawRange,0,180,L" deg",0);
            column.Slider(PROTECTED_TEXT(L"Yaw offset"),settings.YawOffset,-180,180,L" deg",0);
            column.Toggle(PROTECTED_TEXT(L"At target"),settings.AtTarget);
            UI::g_UI.SetIdScope(0);
        };
        profile(standing,PROTECTED_TEXT(L"Standing"),c.StandingSpin);
        profile(moving,PROTECTED_TEXT(L"Moving"),c.MovingSpin);
        height=(std::max)(standing.y,moving.y)-y;
    }

} // namespace Menu
