// 34cheat/src/ui_renderer.hpp
// context: C++20 / ui_renderer.hpp / MSVC / x64 / Windows 10/11 / version-5a21b87bbc2641cf
// Immediate-mode Direct2D/DirectWrite widget layer shared by the loader and the overlay menu.
#pragma once

#include <windows.h>
// winuser.h maps DrawText to DrawTextA/W. Drop it before d2d1.h so the D2D and widget names stay literal.
#ifdef DrawText
#undef DrawText
#endif
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include "../include/math.hpp"
#include "../include/config.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace UI {

    struct StyleColors {
        D2D1_COLOR_F BgBase;         // Window ground
        D2D1_COLOR_F BgSurface;      // Content panel
        D2D1_COLOR_F BgElevated;     // Controls, hover rows, popups
        D2D1_COLOR_F BorderSubtle;   // Hairlines and dividers
        D2D1_COLOR_F BorderHover;    // Hovered control outline
        D2D1_COLOR_F BorderActive;   // Accent outline (focus, listening)
        D2D1_COLOR_F TextPrimary;
        D2D1_COLOR_F TextSecondary;
        D2D1_COLOR_F TextTertiary;
        D2D1_COLOR_F Accent;
        D2D1_COLOR_F AccentHover;    // Accent lifted for hovered primary buttons
        D2D1_COLOR_F AccentGlow;     // Accent at low alpha, for tinted fills
        D2D1_COLOR_F OnAccent;       // Readable text on an accent fill
        D2D1_COLOR_F Success;
        D2D1_COLOR_F Danger;
        D2D1_COLOR_F Warning;
    };

    enum Font : int {
        FontBody = 0,     // 13px regular
        FontHeader = 1,   // 14px semibold
        FontBold = 2,     // 13px semibold
        FontMono = 3,     // 12px monospace, numbers only
        FontSmall = 4,    // 11.5px regular
        FontTitle = 5,    // 20px semibold
        FontCount
    };

    enum class Icon {
        Aim, Visuals, Players, Utilities, Settings, Movement, Rage, Lighting,
        Close, Minimize, Check, Cross, Chevron
    };

    // Stable widget identity across frames, derived from the label.
    constexpr uint64_t HashId(std::wstring_view s, uint64_t seed = 1469598103934665603ull) {
        uint64_t h = seed;
        for (wchar_t c : s) { h ^= static_cast<uint64_t>(c); h *= 1099511628211ull; }
        return h;
    }

    class UIRenderer {
    private:
        uint64_t m_idScope = 0;
        ID2D1Factory1* m_pD2DFactory = nullptr;
        IDWriteFactory* m_pDWriteFactory = nullptr;
        ID2D1RenderTarget* m_pRenderTarget = nullptr;
        ID2D1SolidColorBrush* m_pSolidBrush = nullptr;
        ID2D1StrokeStyle* m_pRoundStroke = nullptr;
        IDWriteTextFormat* m_fonts[FontCount]{};

        StyleColors m_colors{};
        Math::Vector2 m_mousePos{ 0.0f, 0.0f };
        bool m_mouseClicked = false;
        bool m_mouseDown = false;
        bool m_mouseReleased = false;
        float m_wheel = 0.0f;
        float m_pendingWheel = 0.0f;

        // Frame timing for eased transitions
        LARGE_INTEGER m_qpcFreq{};
        LARGE_INTEGER m_qpcLast{};
        float m_dt = 1.0f / 60.0f;
        float m_time = 0.0f;
        std::unordered_map<uint64_t, float> m_anim;

        // Widget layout and interaction state
        float m_itemWidth = 280.0f;
        std::vector<D2D1_RECT_F> m_clipStack;
        uint64_t m_activeSlider = 0;
        uint64_t m_listeningKey = 0;
        bool m_listeningSeen = false;
        uint64_t m_textFocus=0;
        bool m_textSeen=false,m_textSelectAll=false;
        std::vector<wchar_t> m_textChars;
        bool m_keyWasDown[256]{};

        // Combo popup, drawn after everything else so it sits on top
        uint64_t m_openCombo = 0;
        bool m_openComboSeen = false;
        int m_comboScroll=0;
        D2D1_RECT_F m_comboAnchor{};
        std::vector<std::wstring> m_comboOptions;
        int m_comboSelected = -1;
        uint64_t m_comboChangedId = 0;
        int m_comboChangedIndex = -1;
        bool m_clickEatenByPopup = false;
        bool m_inPopupPass = false;
        uint64_t m_openColor=0;
        Config::Color4* m_colorValue=nullptr;
        bool m_colorSeen=false;
        Math::Vector2 m_colorAnchor;
        std::wstring m_colorLabel;
        D2D1_RECT_F ColorPopupRect() const;

        IDWriteTextFormat* Format(int fontStyle) const;
        D2D1_RECT_F PopupRect() const;
        bool Clicked() const { return m_mouseClicked && (!m_clickEatenByPopup||m_inPopupPass); }
        void DrawPolyline(const std::vector<D2D1_POINT_2F>& pts, const D2D1_COLOR_F& color, float strokeWidth);
        void DrawPathEllipseArc(D2D1_POINT_2F from, D2D1_POINT_2F to, D2D1_SIZE_F radius, bool clockwise, bool large, const D2D1_COLOR_F& color, float strokeWidth, bool closeWithMirror = false);

    public:
        UIRenderer() = default;
        ~UIRenderer() { Shutdown(); }

        bool Initialize(IDXGISwapChain* pSwapChain);
        void Shutdown();
        void UpdateTheme(Config::AccentTheme theme);

        void BeginDraw();
        void EndDraw();

        void UpdateInput(const Math::Vector2& mousePos, bool mouseDown);
        void AddWheel(float notches) { m_pendingWheel += notches; }

        // Primitives
        void DrawLine(const Math::Vector2& from, const Math::Vector2& to, const D2D1_COLOR_F& color, float strokeWidth = 1.0f);
        void DrawRect(const Math::Vector2& pos, const Math::Vector2& size, const D2D1_COLOR_F& color, float strokeWidth = 1.0f);
        void FillRect(const Math::Vector2& pos, const Math::Vector2& size, const D2D1_COLOR_F& color);
        void DrawRoundedRect(const Math::Vector2& pos, const Math::Vector2& size, float radius, const D2D1_COLOR_F& color, float strokeWidth = 1.0f);
        void FillRoundedRect(const Math::Vector2& pos, const Math::Vector2& size, float radius, const D2D1_COLOR_F& color);
        void DrawCircle(const Math::Vector2& center, float radius, const D2D1_COLOR_F& color, float strokeWidth = 1.0f);
        void FillCircle(const Math::Vector2& center, float radius, const D2D1_COLOR_F& color);
        void DrawArc(const Math::Vector2& center, float radius, float startRad, float sweepRad, const D2D1_COLOR_F& color, float strokeWidth = 1.5f);
        void DrawIcon(Icon icon, const Math::Vector2& center, float size, const D2D1_COLOR_F& color);
        void DrawShadow(const Math::Vector2& pos, const Math::Vector2& size, float radius, float offsetY, float spread, float alpha);
        void FillPolygon(const std::vector<Math::Vector2>& pts, const D2D1_COLOR_F& color);
        void DrawPolygon(const std::vector<Math::Vector2>& pts, const D2D1_COLOR_F& color, float strokeWidth = 1.5f);

        // Brand: accent tile holding an isometric cube, and the two-tone "34Cheat" wordmark beside it.
        // reveal 0..1 drops the cube faces in one after another (loader intro); 1 is the resting mark.
        void DrawLogoMark(const Math::Vector2& pos, float size, float reveal = 1.0f, float alpha = 1.0f);
        float DrawLogo(const Math::Vector2& pos, float markSize); // returns total width

        // Text
        void DrawText(const std::wstring& text, const Math::Vector2& pos, const Math::Vector2& size, const D2D1_COLOR_F& color, int fontStyle = FontBody, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
        float MeasureText(const std::wstring& text, int fontStyle = FontBody);

        // Clipping (also restricts hit-testing)
        void PushClip(const Math::Vector2& pos, const Math::Vector2& size);
        void PopClip();

        // Widgets. Row widgets span SetItemWidth() pixels: label left, control right.
        void SetItemWidth(float width) { m_itemWidth = width; }
        float GetItemWidth() const { return m_itemWidth; }
        bool RenderToggle(const Math::Vector2& pos, const std::wstring& label, bool& state);
        bool RenderSlider(const Math::Vector2& pos, const std::wstring& label, float& value, float min, float max, const std::wstring& suffix = L"", int decimals = 1);
        bool RenderIntSlider(const Math::Vector2& pos, const std::wstring& label, int& value, int min, int max, const std::wstring& suffix = L"");
        bool RenderButton(const Math::Vector2& pos, const Math::Vector2& size, const std::wstring& label, bool primary = false, bool enabled = true);
        bool RenderIconButton(const Math::Vector2& pos, const Math::Vector2& size, Icon icon, bool danger = false);
        void AddCharacter(wchar_t character) {if(m_textFocus)m_textChars.push_back(character);}
        bool RenderTextInput(const Math::Vector2& pos,const std::wstring& label,std::wstring& value,size_t limit=64);
        bool RenderKeybind(const Math::Vector2& pos, const std::wstring& label, int& key);
        void RenderColorButton(const Math::Vector2& pos,const std::wstring& label,Config::Color4& color);
        void ClosePopups() {m_openCombo=0;m_openColor=0;m_colorValue=nullptr;m_activeSlider=0;}
        void CancelInputCapture() {ClosePopups();m_listeningKey=0;m_textFocus=0;}
        bool RenderCombo(const Math::Vector2& pos, const std::wstring& label, int& selectedIndex, const std::vector<std::wstring>& options);
        void RenderBadge(const Math::Vector2& pos, const std::wstring& text, const D2D1_COLOR_F& color);
        void RenderProgressBar(const Math::Vector2& pos, const Math::Vector2& size, float progress, const D2D1_COLOR_F& fillColor);
        void RenderCard(const Math::Vector2& pos, const Math::Vector2& size, const std::wstring& title, const std::wstring& subtitle = L"");

        // Must run after all other widgets and before EndDraw so popups paint on top.
        void RenderPopups();

        // Eased value keyed by id + channel, moving toward target each frame.
        float Animate(uint64_t id, int channel, float target, float speed = 16.0f);
        void SetAnim(uint64_t id, int channel, float value);
        void SetIdScope(uint64_t scope) { m_idScope = scope; }
        bool IsHovered(const Math::Vector2& pos, const Math::Vector2& size) const;
        static std::wstring KeyName(int vk);

        const StyleColors& GetColors() const { return m_colors; }
        const Math::Vector2& GetMousePos() const { return m_mousePos; }
        bool IsMouseClicked() const { return Clicked(); }
        void ConsumeMouseClick() {m_mouseClicked=false;}
        bool IsMouseDown() const { return m_mouseDown; }
        bool IsPopupOpen() const { return m_openCombo != 0||m_openColor!=0; }
        bool IsCapturingInput() const { return m_activeSlider != 0 || m_listeningKey != 0 || m_textFocus!=0; }
        float ConsumeWheel() { float w = m_wheel; m_wheel = 0.0f; return w; }
        float DeltaTime() const { return m_dt; }
        float Time() const { return m_time; }
    };

    inline D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { c.a = a; return c; }
    inline D2D1_COLOR_F FromColor(const Config::Color4& c) { return D2D1::ColorF(c.r,c.g,c.b,c.a); }
    inline D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
        return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
    }

    inline UIRenderer g_UI;

} // namespace UI
