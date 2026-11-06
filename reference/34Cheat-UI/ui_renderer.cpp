// 34cheat/src/ui_renderer.cpp
// context: C++20 / ui_renderer.cpp / MSVC / x64 / Windows 10/11 / version-5a21b87bbc2641cf
#include "ui_renderer.hpp"
#include "../include/diagnostics.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace UI {

    namespace {
        constexpr float kPi = 3.14159265f;
        constexpr float kRowHeight = 30.0f;
        constexpr float kSliderHeight = 44.0f;
        constexpr float kPopupItemHeight = 28.0f;

        float Linearize(float c) {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        float Luminance(const D2D1_COLOR_F& c) {
            return 0.2126f * Linearize(c.r) + 0.7152f * Linearize(c.g) + 0.0722f * Linearize(c.b);
        }

        float Contrast(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
            float la = Luminance(a), lb = Luminance(b);
            return ((std::max)(la, lb) + 0.05f) / ((std::min)(la, lb) + 0.05f);
        }

        bool PointIn(const Math::Vector2& p, const D2D1_RECT_F& r) {
            return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
        }

        // Picks the first installed family so Windows 10 falls back cleanly from the Variable faces.
        std::wstring PickFamily(IDWriteFactory* factory, std::initializer_list<const wchar_t*> candidates) {
            IDWriteFontCollection* collection = nullptr;
            if (FAILED(factory->GetSystemFontCollection(&collection)) || !collection) {
                return *candidates.begin();
            }
            std::wstring chosen = *(candidates.end() - 1);
            for (const wchar_t* name : candidates) {
                UINT32 index = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists) {
                    chosen = name;
                    break;
                }
            }
            collection->Release();
            return chosen;
        }
    }

    bool UIRenderer::Initialize(IDXGISwapChain* pSwapChain) {
        if (!pSwapChain) return false;

        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);
        if (FAILED(hr)) return false;

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&m_pDWriteFactory));
        if (FAILED(hr)) return false;

        IDXGISurface* pBackBuffer = nullptr;
        hr = pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
        if (FAILED(hr) || !pBackBuffer) return false;

        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f
        );

        hr = m_pD2DFactory->CreateDxgiSurfaceRenderTarget(pBackBuffer, &props, &m_pRenderTarget);
        pBackBuffer->Release();
        if (FAILED(hr) || !m_pRenderTarget) return false;

        m_pRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        // ClearType fringes on a transparent overlay surface, grayscale stays clean.
        m_pRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        hr = m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_pSolidBrush);
        if (FAILED(hr)) return false;

        D2D1_STROKE_STYLE_PROPERTIES strokeProps = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        m_pD2DFactory->CreateStrokeStyle(strokeProps, nullptr, 0, &m_pRoundStroke);

        const std::wstring textFamily = PickFamily(m_pDWriteFactory, { L"Segoe UI Variable Text", L"Segoe UI" });
        const std::wstring displayFamily = PickFamily(m_pDWriteFactory, { L"Segoe UI Variable Display", L"Segoe UI" });
        const std::wstring monoFamily = PickFamily(m_pDWriteFactory, { L"Cascadia Mono", L"Consolas" });

        struct FontSpec { const std::wstring* Family; DWRITE_FONT_WEIGHT Weight; float Size; };
        const FontSpec specs[FontCount] = {
            { &textFamily,    DWRITE_FONT_WEIGHT_REGULAR,   13.0f },
            { &textFamily,    DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f },
            { &textFamily,    DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f },
            { &monoFamily,    DWRITE_FONT_WEIGHT_REGULAR,   12.0f },
            { &textFamily,    DWRITE_FONT_WEIGHT_REGULAR,   11.5f },
            { &displayFamily, DWRITE_FONT_WEIGHT_SEMI_BOLD, 20.0f },
        };

        for (int i = 0; i < FontCount; ++i) {
            m_pDWriteFactory->CreateTextFormat(specs[i].Family->c_str(), nullptr, specs[i].Weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, specs[i].Size, L"en-us", &m_fonts[i]);
            if (!m_fonts[i]) continue;

            m_fonts[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            IDWriteInlineObject* ellipsis = nullptr;
            if (SUCCEEDED(m_pDWriteFactory->CreateEllipsisTrimmingSign(m_fonts[i], &ellipsis)) && ellipsis) {
                DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                m_fonts[i]->SetTrimming(&trimming, ellipsis);
                ellipsis->Release();
            }
        }

        QueryPerformanceFrequency(&m_qpcFreq);
        QueryPerformanceCounter(&m_qpcLast);

        UpdateTheme(Config::g_Config.Customization.Theme);
        return true;
    }

    void UIRenderer::Shutdown() {
        ClosePopups();
        if (m_pSolidBrush) { m_pSolidBrush->Release(); m_pSolidBrush = nullptr; }
        if (m_pRoundStroke) { m_pRoundStroke->Release(); m_pRoundStroke = nullptr; }
        for (auto& font : m_fonts) {
            if (font) { font->Release(); font = nullptr; }
        }
        if (m_pRenderTarget) { m_pRenderTarget->Release(); m_pRenderTarget = nullptr; }
        if (m_pDWriteFactory) { m_pDWriteFactory->Release(); m_pDWriteFactory = nullptr; }
        if (m_pD2DFactory) { m_pD2DFactory->Release(); m_pD2DFactory = nullptr; }
        m_anim.clear();
        m_clipStack.clear();
    }

    void UIRenderer::UpdateTheme(Config::AccentTheme theme) {
        // Graphite neutrals: the overlay sits on top of a bright, busy game, so the
        // panel stays near-opaque and colourless and the accent is the only hue.
        m_colors.BgBase = D2D1::ColorF(0x0F1012, 0.98f);
        m_colors.BgSurface = D2D1::ColorF(0x16171A, 1.0f);
        m_colors.BgElevated = D2D1::ColorF(0x212226, 1.0f);
        m_colors.BorderSubtle = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.07f);
        m_colors.BorderHover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
        m_colors.TextPrimary = D2D1::ColorF(0xEDEDEF, 1.0f);
        m_colors.TextSecondary = D2D1::ColorF(0xA4A6AE, 1.0f);
        m_colors.TextTertiary = D2D1::ColorF(0x7D808A, 1.0f);
        m_colors.Success = D2D1::ColorF(0x3DCB82, 1.0f);
        m_colors.Danger = D2D1::ColorF(0xF0555A, 1.0f);
        m_colors.Warning = D2D1::ColorF(0xF2B441, 1.0f);

        UINT32 accent = 0x9B8AF4;
        switch (theme) {
        case Config::AccentTheme::MidnightPurple: accent = 0x9B8AF4; break;
        case Config::AccentTheme::CrimsonRed:     accent = 0xF0555A; break;
        case Config::AccentTheme::CyberpunkCyan:  accent = 0x2EC3D6; break;
        case Config::AccentTheme::EmeraldGreen:   accent = 0x3DCB82; break;
        case Config::AccentTheme::Monochrome:     accent = 0xE4E4E7; break;
        }

        m_colors.Accent = D2D1::ColorF(accent, 1.0f);
        m_colors.AccentHover = Mix(m_colors.Accent, D2D1::ColorF(D2D1::ColorF::White), 0.14f);
        m_colors.AccentGlow = D2D1::ColorF(accent, 0.16f);
        m_colors.BorderActive = D2D1::ColorF(accent, 0.55f);

        const D2D1_COLOR_F dark = D2D1::ColorF(0x111214, 1.0f);
        const D2D1_COLOR_F light = D2D1::ColorF(0xFFFFFF, 1.0f);
        m_colors.OnAccent = Contrast(m_colors.Accent, dark) >= Contrast(m_colors.Accent, light) ? dark : light;
    }

    void UIRenderer::BeginDraw() {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (m_qpcFreq.QuadPart > 0) {
            m_dt = std::clamp(static_cast<float>(now.QuadPart - m_qpcLast.QuadPart) / static_cast<float>(m_qpcFreq.QuadPart), 0.0f, 0.1f);
        }
        m_qpcLast = now;
        m_time += m_dt;

        m_wheel = m_pendingWheel;
        m_pendingWheel = 0.0f;

        // An open popup owns the next click: inside picks an option, anywhere else dismisses.
        m_clickEatenByPopup = false;
        if(m_openColor&&m_mouseClicked) {
            m_clickEatenByPopup=true;
            if(!PointIn(m_mousePos,ColorPopupRect())) {m_openColor=0;m_colorValue=nullptr;m_activeSlider=0;}
        }
        m_colorSeen=false;
        if (m_openCombo && m_mouseClicked) {
            m_clickEatenByPopup = true;
            if (!PointIn(m_mousePos, PopupRect())) {
                m_openCombo = 0;
            }
        }
        m_openComboSeen = false;
        m_listeningSeen = false;
        m_textSeen=false;
        if(m_openCombo&&m_wheel!=0) {
            m_comboScroll=std::clamp(m_comboScroll-static_cast<int>(m_wheel*3),0,(std::max)(0,static_cast<int>(m_comboOptions.size())-10));
            m_wheel=0;
        }

        if (m_pRenderTarget) {
            m_pRenderTarget->BeginDraw();
            m_pRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
        }
    }

    void UIRenderer::EndDraw() {
        while (!m_clipStack.empty()) PopClip();

        if (m_pRenderTarget) {
            HRESULT result = m_pRenderTarget->EndDraw();
            static unsigned errors = 0;
            if (FAILED(result) && errors++ < 8) {
                Diagnostics::Log("Direct2D EndDraw failed HRESULT=0x", std::hex,
                    static_cast<unsigned long>(result));
            }
        }

        if (!m_mouseDown) m_activeSlider = 0;
        if (m_listeningKey && !m_listeningSeen) m_listeningKey = 0;

        if(m_textFocus&&!m_textSeen)m_textFocus=0;
        m_textChars.clear();
        m_mouseClicked = false;
        m_mouseReleased = false;
    }

    void UIRenderer::UpdateInput(const Math::Vector2& mousePos, bool mouseDown) {
        m_mousePos = mousePos;
        if (mouseDown && !m_mouseDown) {
            m_mouseClicked = true;
        } else if (!mouseDown && m_mouseDown) {
            m_mouseReleased = true;
        }
        m_mouseDown = mouseDown;
    }

    float UIRenderer::Animate(uint64_t id, int channel, float target, float speed) {
        uint64_t key = id ^ (static_cast<uint64_t>(channel + 1) * 0x9E3779B97F4A7C15ull);
        auto it = m_anim.find(key);
        if (it == m_anim.end() || Config::g_Config.Customization.ReducedMotion) {
            m_anim[key] = target;
            return target;
        }
        float k = 1.0f - std::exp(-speed * m_dt);
        it->second += (target - it->second) * k;
        if (std::fabs(target - it->second) < 0.001f) it->second = target;
        return it->second;
    }

    void UIRenderer::SetAnim(uint64_t id, int channel, float value) {
        uint64_t key = id ^ (static_cast<uint64_t>(channel + 1) * 0x9E3779B97F4A7C15ull);
        m_anim[key] = value;
    }

    bool UIRenderer::IsHovered(const Math::Vector2& pos, const Math::Vector2& size) const {
        D2D1_RECT_F r{ pos.x, pos.y, pos.x + size.x, pos.y + size.y };
        if (!PointIn(m_mousePos, r)) return false;
        if (!m_clipStack.empty() && !PointIn(m_mousePos, m_clipStack.back())) return false;
        if (!m_inPopupPass && m_openCombo && PointIn(m_mousePos, PopupRect())) return false;
        if (!m_inPopupPass && m_openColor && PointIn(m_mousePos,ColorPopupRect())) return false;
        return true;
    }

    // Primitives
    void UIRenderer::DrawLine(const Math::Vector2& from, const Math::Vector2& to, const D2D1_COLOR_F& color, float strokeWidth) {
        if (!m_pRenderTarget || !m_pSolidBrush) return;
        m_pSolidBrush->SetColor(color);
        m_pRenderTarget->DrawLine(D2D1::Point2F(from.x, from.y), D2D1::Point2F(to.x, to.y), m_pSolidBrush, strokeWidth, m_pRoundStroke);
    }

    void UIRenderer::DrawRect(const Math::Vector2& pos, const Math::Vector2& size, const D2D1_COLOR_F& color, float strokeWidth) {
        if (!m_pRenderTarget || !m_pSolidBrush) return;
        m_pSolidBrush->SetColor(color);
        m_pRenderTarget->DrawRectangle(D2D1::RectF(pos.x, pos.y, pos.x + size.x, pos.y + size.y), m_pSolidBrush, strokeWidth);
    }

    void UIRenderer::FillRect(const Math::Vector2& pos, const Math::Vector2& size, const D2D1_COLOR_F& color) {
        if (!m_pRenderTarget || !m_pSolidBrush) return;
        m_pSolidBrush->SetColor(color);
        m_pRenderTarget->FillRectangle(D2D1::RectF(pos.x, pos.y, pos.x + size.x, pos.y + size.y), m_pSolidBrush);
    }

    void UIRenderer::DrawRoundedRect(const Math::Vector2& pos, const Math::Vector2& size, float radius, const D2D1_COLOR_F& color, float strokeWidth) {
        if (!m_pRenderTarget || !m_pSolidBrush) return;
        m_pSolidBrush->SetColor(color);
        // Inset by half the stroke so hairlines land on whole pixels.
        float h = strokeWidth * 0.5f;
        D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(D2D1::RectF(pos.x + h, pos.y + h, pos.x + size.x - h, pos.y + size.y - h), radius, radius);
        m_pRenderTarget->DrawRoundedRectangle(rrect, m_pSolidBrush, strokeWidth);
    }

    void UIRenderer::FillRoundedRect(const Math::Vector2& pos, const Math::Vector2& size, float radius, const D2D1_COLOR_F& color) {
        if (!m_pRenderTarget || !m_pSolidBrush) return;
        m_pSolidBrush->SetColor(color);
        D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(D2D1::RectF(pos.x, pos.y, pos.x + size.x, pos.y + size.y), radius, radius);
        m_pRenderTarget->FillRoundedRectangle(rrect, m_pSolidBrush);
    }

    void UIRenderer::DrawCircle(const Math::Vector2& center, float radius, const D2D1_COLOR_F& color, float strokeWidth) {
        if (!m_pRenderTarget || !m_pSolidBrush) return;
        m_pSolidBrush->SetColor(color);
        m_pRenderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(center.x, center.y), radius, radius), m_pSolidBrush, strokeWidth);
    }

    void UIRenderer::FillCircle(const Math::Vector2& center, float radius, const D2D1_COLOR_F& color) {
        if (!m_pRenderTarget || !m_pSolidBrush) return;
        m_pSolidBrush->SetColor(color);
        m_pRenderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x, center.y), radius, radius), m_pSolidBrush);
    }

    void UIRenderer::DrawPolyline(const std::vector<D2D1_POINT_2F>& pts, const D2D1_COLOR_F& color, float strokeWidth) {
        if (!m_pRenderTarget || !m_pSolidBrush || pts.size() < 2) return;
        ID2D1PathGeometry* path = nullptr;
        if (FAILED(m_pD2DFactory->CreatePathGeometry(&path)) || !path) return;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(path->Open(&sink)) && sink) {
            sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLines(pts.data() + 1, static_cast<UINT32>(pts.size() - 1));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            m_pSolidBrush->SetColor(color);
            m_pRenderTarget->DrawGeometry(path, m_pSolidBrush, strokeWidth, m_pRoundStroke);
        }
        path->Release();
    }

    void UIRenderer::DrawPathEllipseArc(D2D1_POINT_2F from, D2D1_POINT_2F to, D2D1_SIZE_F radius, bool clockwise, bool large, const D2D1_COLOR_F& color, float strokeWidth, bool closeWithMirror) {
        if (!m_pRenderTarget || !m_pSolidBrush) return;
        ID2D1PathGeometry* path = nullptr;
        if (FAILED(m_pD2DFactory->CreatePathGeometry(&path)) || !path) return;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(path->Open(&sink)) && sink) {
            sink->BeginFigure(from, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(to, radius, 0.0f,
                clockwise ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            if (closeWithMirror) {
                sink->AddArc(D2D1::ArcSegment(from, radius, 0.0f,
                    clockwise ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                    large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            }
            sink->EndFigure(closeWithMirror ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            m_pSolidBrush->SetColor(color);
            m_pRenderTarget->DrawGeometry(path, m_pSolidBrush, strokeWidth, m_pRoundStroke);
        }
        path->Release();
    }

    void UIRenderer::DrawArc(const Math::Vector2& center, float radius, float startRad, float sweepRad, const D2D1_COLOR_F& color, float strokeWidth) {
        D2D1_POINT_2F from{ center.x + std::cos(startRad) * radius, center.y + std::sin(startRad) * radius };
        D2D1_POINT_2F to{ center.x + std::cos(startRad + sweepRad) * radius, center.y + std::sin(startRad + sweepRad) * radius };
        DrawPathEllipseArc(from, to, { radius, radius }, sweepRad >= 0.0f, std::fabs(sweepRad) > kPi, color, strokeWidth);
    }

    // One 1.5px round-capped stroke family for every icon.
    void UIRenderer::DrawIcon(Icon icon, const Math::Vector2& c, float s, const D2D1_COLOR_F& color) {
        const float w = 1.5f;
        auto P = [&](float x, float y) { return D2D1::Point2F(c.x + x * s, c.y + y * s); };

        switch (icon) {
        case Icon::Aim:
            DrawCircle(c, s * 0.34f, color, w);
            DrawLine({ c.x, c.y - s * 0.5f }, { c.x, c.y - s * 0.18f }, color, w);
            DrawLine({ c.x, c.y + s * 0.18f }, { c.x, c.y + s * 0.5f }, color, w);
            DrawLine({ c.x - s * 0.5f, c.y }, { c.x - s * 0.18f, c.y }, color, w);
            DrawLine({ c.x + s * 0.18f, c.y }, { c.x + s * 0.5f, c.y }, color, w);
            break;
        case Icon::Visuals:
            DrawPathEllipseArc(P(-0.5f, 0.0f), P(0.5f, 0.0f), { s * 0.62f, s * 0.62f }, true, false, color, w, true);
            DrawCircle(c, s * 0.15f, color, w);
            break;
        case Icon::Players:
            DrawCircle({ c.x, c.y - s * 0.17f }, s * 0.17f, color, w);
            DrawPathEllipseArc(P(-0.36f, 0.46f), P(0.36f, 0.46f), { s * 0.36f, s * 0.32f }, true, false, color, w);
            break;
        case Icon::Utilities:
            for (int i = -1; i <= 1; ++i) {
                float y = c.y + i * s * 0.3f;
                float knob = c.x + (i == -1 ? -0.16f : (i == 0 ? 0.18f : -0.02f)) * s;
                DrawLine({ c.x - s * 0.44f, y }, { c.x + s * 0.44f, y }, color, w);
                FillCircle({ knob, y }, s * 0.11f, color);
            }
            break;
        case Icon::Settings:
            DrawCircle(c, s * 0.2f, color, w);
            for (int i = 0; i < 8; ++i) {
                float a = i * kPi / 4.0f;
                DrawLine({ c.x + std::cos(a) * s * 0.33f, c.y + std::sin(a) * s * 0.33f },
                         { c.x + std::cos(a) * s * 0.47f, c.y + std::sin(a) * s * 0.47f }, color, w);
            }
            break;
        case Icon::Close:
            DrawLine({c.x-s*.32f,c.y-s*.32f},{c.x+s*.32f,c.y+s*.32f},color,w);
            DrawLine({c.x+s*.32f,c.y-s*.32f},{c.x-s*.32f,c.y+s*.32f},color,w);
            break;
        case Icon::Movement:
            DrawLine({c.x-s*.4f,c.y},{c.x+s*.4f,c.y},color,w);
            DrawLine({c.x+s*.1f,c.y-s*.25f},{c.x+s*.4f,c.y},color,w);
            DrawLine({c.x+s*.1f,c.y+s*.25f},{c.x+s*.4f,c.y},color,w);
            DrawLine({c.x-s*.1f,c.y-s*.25f},{c.x-s*.4f,c.y},color,w);
            DrawLine({c.x-s*.1f,c.y+s*.25f},{c.x-s*.4f,c.y},color,w);
            break;
        case Icon::Rage:
            DrawPolygon({{c.x+s*.1f,c.y-s*.5f},{c.x-s*.3f,c.y+s*.05f},{c.x,c.y+s*.05f},
                {c.x-s*.1f,c.y+s*.5f},{c.x+s*.3f,c.y-s*.05f},{c.x,c.y-s*.05f}},color,w);
            break;
        case Icon::Lighting:
            DrawCircle(c,s*.2f,color,w);
            for (int i=0;i<8;++i) {
                float angle=i*kPi/4;
                DrawLine({c.x+std::cos(angle)*s*.32f,c.y+std::sin(angle)*s*.32f},
                    {c.x+std::cos(angle)*s*.48f,c.y+std::sin(angle)*s*.48f},color,w);
            }
            break;
        case Icon::Cross:
            DrawLine({ c.x - s * 0.32f, c.y - s * 0.32f }, { c.x + s * 0.32f, c.y + s * 0.32f }, color, w);
            DrawLine({ c.x + s * 0.32f, c.y - s * 0.32f }, { c.x - s * 0.32f, c.y + s * 0.32f }, color, w);
            break;
        case Icon::Minimize:
            DrawLine({ c.x - s * 0.36f, c.y }, { c.x + s * 0.36f, c.y }, color, w);
            break;
        case Icon::Check:
            DrawPolyline({ P(-0.36f, 0.02f), P(-0.1f, 0.28f), P(0.38f, -0.26f) }, color, w + 0.25f);
            break;
        case Icon::Chevron:
            DrawPolyline({ P(-0.3f, -0.14f), P(0.0f, 0.16f), P(0.3f, -0.14f) }, color, w);
            break;
        }
    }

    // The "3" is two clockwise bowls meeting at the waist; the "4" is a stem, a diagonal and a crossbar
    // that runs past the stem. Strokes scale with the mark so it stays crisp from 16 to 64 px.
    void UIRenderer::FillPolygon(const std::vector<Math::Vector2>& pts, const D2D1_COLOR_F& color) {
        if (!m_pRenderTarget || !m_pSolidBrush || pts.size() < 3) return;
        ID2D1PathGeometry* path = nullptr;
        if (FAILED(m_pD2DFactory->CreatePathGeometry(&path)) || !path) return;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(path->Open(&sink)) && sink) {
            sink->BeginFigure({ pts[0].x, pts[0].y }, D2D1_FIGURE_BEGIN_FILLED);
            for (size_t i = 1; i < pts.size(); ++i) sink->AddLine({ pts[i].x, pts[i].y });
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            m_pSolidBrush->SetColor(color);
            m_pRenderTarget->FillGeometry(path, m_pSolidBrush);
        }
        path->Release();
    }

    void UIRenderer::DrawPolygon(const std::vector<Math::Vector2>& pts, const D2D1_COLOR_F& color, float strokeWidth) {
        if (pts.size() < 2) return;
        std::vector<D2D1_POINT_2F> closed;
        closed.reserve(pts.size() + 2);
        for (const auto& p : pts) closed.push_back({ p.x, p.y });
        // Run one segment past the start so the round join also covers the first corner.
        closed.push_back(closed[0]);
        closed.push_back(closed[1]);
        DrawPolyline(closed, color, strokeWidth);
    }

    void UIRenderer::DrawLogoMark(const Math::Vector2& pos, float s, float reveal, float alpha) {
        FillRoundedRect(pos, { s, s }, s * 0.28f, WithAlpha(m_colors.Accent, alpha));

        auto P = [&](float x, float y, float dy) { return Math::Vector2{ pos.x + x * s, pos.y + (y + dy) * s }; };

        // Pointy-top hexagon split into three faces: lit top, mid left, faint right.
        struct Face { float Shade; float Pts[8]; };
        const Face faces[3] = {
            { 1.00f, { 0.50f, 0.20f, 0.77f, 0.355f, 0.50f, 0.51f, 0.23f, 0.355f } },
            { 0.62f, { 0.23f, 0.355f, 0.50f, 0.51f, 0.50f, 0.82f, 0.23f, 0.665f } },
            { 0.30f, { 0.50f, 0.51f, 0.77f, 0.355f, 0.77f, 0.665f, 0.50f, 0.82f } },
        };

        for (int i = 0; i < 3; ++i) {
            float t = std::clamp((reveal - i * 0.22f) / 0.56f, 0.0f, 1.0f);
            if (t <= 0.0f) continue;
            float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
            float dy = -(1.0f - e) * 0.14f;
            const float* f = faces[i].Pts;
            FillPolygon({ P(f[0], f[1], dy), P(f[2], f[3], dy), P(f[4], f[5], dy), P(f[6], f[7], dy) },
                WithAlpha(m_colors.OnAccent, faces[i].Shade * e * alpha));
        }

        // Seams in the tile colour keep the faces readable at 18 px.
        if (reveal >= 1.0f) {
            const float seam = (std::max)(1.0f, s * 0.035f);
            const D2D1_COLOR_F tile = WithAlpha(m_colors.Accent, alpha);
            DrawLine(P(0.50f, 0.51f, 0.0f), P(0.50f, 0.83f, 0.0f), tile, seam);
            DrawLine(P(0.50f, 0.51f, 0.0f), P(0.22f, 0.35f, 0.0f), tile, seam);
            DrawLine(P(0.50f, 0.51f, 0.0f), P(0.78f, 0.35f, 0.0f), tile, seam);
        }
    }

    float UIRenderer::DrawLogo(const Math::Vector2& pos, float markSize) {
        DrawLogoMark(pos, markSize);
        const float gap = markSize * 0.45f + 2.0f;
        float x = pos.x + markSize + gap;
        const float numW = MeasureText(L"34", FontBold);
        const float wordW = MeasureText(L"Cheat", FontBold);
        DrawText(L"34", { x, pos.y }, { numW + 2.0f, markSize }, m_colors.TextPrimary, FontBold);
        DrawText(L"Cheat", { x + numW, pos.y }, { wordW + 2.0f, markSize }, m_colors.TextSecondary, FontBold);
        return markSize + gap + numW + wordW;
    }

    // Layered rounded rects approximate a soft, downward-offset shadow without an effects pipeline.
    void UIRenderer::DrawShadow(const Math::Vector2& pos, const Math::Vector2& size, float radius, float offsetY, float spread, float alpha) {
        const int steps = 8;
        for (int i = steps; i >= 1; --i) {
            float t = static_cast<float>(i) / steps;
            float e = spread * t;
            float a = alpha * (1.0f - t) * (1.0f - t) / steps * 4.0f;
            FillRoundedRect({ pos.x - e, pos.y - e + offsetY }, { size.x + e * 2.0f, size.y + e * 2.0f }, radius + e, D2D1::ColorF(0.0f, 0.0f, 0.0f, a));
        }
    }

    IDWriteTextFormat* UIRenderer::Format(int fontStyle) const {
        if (fontStyle < 0 || fontStyle >= FontCount) fontStyle = FontBody;
        return m_fonts[fontStyle];
    }

    void UIRenderer::DrawText(const std::wstring& text, const Math::Vector2& pos, const Math::Vector2& size, const D2D1_COLOR_F& color, int fontStyle, DWRITE_TEXT_ALIGNMENT align) {
        if (!m_pRenderTarget || !m_pSolidBrush || text.empty()) return;
        IDWriteTextFormat* format = Format(fontStyle);
        if (!format) return;
        m_pSolidBrush->SetColor(color);
        format->SetTextAlignment(align);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F rect = D2D1::RectF(pos.x, pos.y, pos.x + size.x, pos.y + size.y);
        m_pRenderTarget->DrawText(text.c_str(), static_cast<UINT32>(text.length()), format, rect, m_pSolidBrush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    float UIRenderer::MeasureText(const std::wstring& text, int fontStyle) {
        IDWriteTextFormat* format = Format(fontStyle);
        if (!m_pDWriteFactory || !format || text.empty()) return 0.0f;
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(m_pDWriteFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.length()), format, 4096.0f, 64.0f, &layout)) || !layout) {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        layout->Release();
        return std::ceil(metrics.widthIncludingTrailingWhitespace);
    }

    void UIRenderer::PushClip(const Math::Vector2& pos, const Math::Vector2& size) {
        D2D1_RECT_F r{ pos.x, pos.y, pos.x + size.x, pos.y + size.y };
        if (!m_clipStack.empty()) {
            const auto& p = m_clipStack.back();
            r = { (std::max)(r.left, p.left), (std::max)(r.top, p.top), (std::min)(r.right, p.right), (std::min)(r.bottom, p.bottom) };
        }
        m_clipStack.push_back(r);
        if (m_pRenderTarget) m_pRenderTarget->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }

    void UIRenderer::PopClip() {
        if (m_clipStack.empty()) return;
        m_clipStack.pop_back();
        if (m_pRenderTarget) m_pRenderTarget->PopAxisAlignedClip();
    }

    // Widgets
    bool UIRenderer::RenderToggle(const Math::Vector2& pos, const std::wstring& label, bool& state) {
        const uint64_t id = HashId(label, 0x70661E^m_idScope);
        const float width = m_itemWidth;
        bool hovered = IsHovered({ pos.x - 8.0f, pos.y }, { width + 16.0f, kRowHeight });
        bool changed = false;

        if (hovered && Clicked()) {
            state = !state;
            changed = true;
        }

        float hover = Animate(id, 0, hovered ? 1.0f : 0.0f, 22.0f);
        float on = Animate(id, 1, state ? 1.0f : 0.0f);

        if (hover > 0.01f) {
            FillRoundedRect({ pos.x - 8.0f, pos.y }, { width + 16.0f, kRowHeight }, 6.0f, WithAlpha(m_colors.BgElevated, 0.55f * hover));
        }

        DrawText(label, pos, { width - 48.0f, kRowHeight }, Mix(m_colors.TextSecondary, m_colors.TextPrimary, (std::max)(hover, on)), FontBody);

        Math::Vector2 trackSize{ 32.0f, 18.0f };
        Math::Vector2 trackPos{ pos.x + width - trackSize.x, pos.y + (kRowHeight - trackSize.y) * 0.5f };

        D2D1_COLOR_F offTrack = Mix(m_colors.BgElevated, D2D1::ColorF(D2D1::ColorF::White), 0.04f * hover);
        FillRoundedRect(trackPos, trackSize, 9.0f, Mix(offTrack, m_colors.Accent, on));
        if (on < 0.99f) {
            DrawRoundedRect(trackPos, trackSize, 9.0f, WithAlpha(hovered ? m_colors.BorderHover : m_colors.BorderSubtle, 1.0f - on), 1.0f);
        }

        float thumbX = trackPos.x + 9.0f + (trackSize.x - 18.0f) * on;
        FillCircle({ thumbX, trackPos.y + 9.0f }, 6.0f, Mix(m_colors.TextSecondary, m_colors.OnAccent, on));
        return changed;
    }

    bool UIRenderer::RenderSlider(const Math::Vector2& pos, const std::wstring& label, float& value, float min, float max, const std::wstring& suffix, int decimals) {
        const uint64_t id = HashId(label, 0x511DE^m_idScope);
        const float width = m_itemWidth;

        Math::Vector2 hitPos{ pos.x - 4.0f, pos.y + 18.0f };
        Math::Vector2 hitSize{ width + 8.0f, kSliderHeight - 18.0f };
        bool hovered = IsHovered(hitPos, hitSize);

        if (hovered && Clicked()) m_activeSlider = id;
        bool active = (m_activeSlider == id) && m_mouseDown;

        float before = value;
        Math::Vector2 trackPos{ pos.x, pos.y + 31.0f };
        if (active && max > min) {
            float pct = std::clamp((m_mousePos.x - trackPos.x) / width, 0.0f, 1.0f);
            value = min + pct * (max - min);
        }

        float emphasis = Animate(id, 0, (hovered || active) ? 1.0f : 0.0f, 22.0f);

        std::wstringstream ss;
        ss << std::fixed << std::setprecision(decimals) << value << suffix;
        float valueWidth=std::clamp(MeasureText(ss.str(),FontMono)+8,72.0f,(std::max)(72.0f,width*.5f));
        DrawText(label, { pos.x, pos.y }, { width-valueWidth-8, 20.0f }, Mix(m_colors.TextSecondary, m_colors.TextPrimary, emphasis), FontBody);
        DrawText(ss.str(), { pos.x + width-valueWidth, pos.y }, { valueWidth, 20.0f }, m_colors.TextPrimary, FontMono, DWRITE_TEXT_ALIGNMENT_TRAILING);

        float pct = (max > min) ? std::clamp((value - min) / (max - min), 0.0f, 1.0f) : 0.0f;
        float fillW = width * pct;

        FillRoundedRect(trackPos, { width, 4.0f }, 2.0f, m_colors.BgElevated);
        if (fillW > 0.5f) {
            FillRoundedRect(trackPos, { (std::max)(fillW, 4.0f), 4.0f }, 2.0f, m_colors.Accent);
        }

        Math::Vector2 knob{ trackPos.x + fillW, trackPos.y + 2.0f };
        float r = 5.5f + 1.5f * emphasis;
        FillCircle(knob, r, m_colors.TextPrimary);
        if (active) {
            DrawCircle(knob, r + 3.0f, m_colors.AccentGlow, 3.0f);
        }

        return value != before;
    }

    bool UIRenderer::RenderIntSlider(const Math::Vector2& pos, const std::wstring& label, int& value, int min, int max, const std::wstring& suffix) {
        float fVal = static_cast<float>(value);
        bool changed = RenderSlider(pos, label, fVal, static_cast<float>(min), static_cast<float>(max), suffix, 0);
        value = static_cast<int>(std::lround(fVal));
        return changed;
    }

    bool UIRenderer::RenderButton(const Math::Vector2& pos, const Math::Vector2& size, const std::wstring& label, bool primary, bool enabled) {
        const uint64_t id = HashId(label, static_cast<uint64_t>(pos.x * 131.0f + pos.y)^m_idScope);
        bool hovered = enabled && IsHovered(pos, size);
        bool pressed = hovered && m_mouseDown;
        float hover = Animate(id, 0, hovered ? 1.0f : 0.0f, 22.0f);

        D2D1_COLOR_F bg, text, border;
        if (!enabled) {
            bg = m_colors.BgElevated;
            text = m_colors.TextTertiary;
            border = m_colors.BorderSubtle;
        } else if (primary) {
            bg = Mix(m_colors.Accent, m_colors.AccentHover, hover);
            text = m_colors.OnAccent;
            border = WithAlpha(bg, 0.0f);
        } else {
            bg = Mix(m_colors.BgElevated, Mix(m_colors.BgElevated, D2D1::ColorF(D2D1::ColorF::White), 0.06f), hover);
            text = Mix(m_colors.TextSecondary, m_colors.TextPrimary, hover);
            border = Mix(m_colors.BorderSubtle, m_colors.BorderHover, hover);
        }
        if (pressed) bg = Mix(bg, D2D1::ColorF(0x000000, 1.0f), 0.12f);

        FillRoundedRect(pos, size, 6.0f, bg);
        if (border.a > 0.0f) DrawRoundedRect(pos, size, 6.0f, border, 1.0f);
        DrawText(label, { pos.x + 10.0f, pos.y }, { size.x - 20.0f, size.y }, text, FontBold, DWRITE_TEXT_ALIGNMENT_CENTER);

        return hovered && Clicked();
    }

    bool UIRenderer::RenderIconButton(const Math::Vector2& pos, const Math::Vector2& size, Icon icon, bool danger) {
        const uint64_t id = HashId(L"icon", static_cast<uint64_t>(icon) * 7919u + static_cast<uint64_t>(pos.x * 131.0f + pos.y));
        bool hovered = IsHovered(pos, size);
        float hover = Animate(id, 0, hovered ? 1.0f : 0.0f, 22.0f);

        if (hover > 0.01f) {
            D2D1_COLOR_F fill = danger ? m_colors.Danger : m_colors.BgElevated;
            FillRoundedRect(pos, size, 6.0f, WithAlpha(fill, fill.a * hover));
        }
        D2D1_COLOR_F iconColor = danger ? Mix(m_colors.TextSecondary, D2D1::ColorF(D2D1::ColorF::White), hover)
                                        : Mix(m_colors.TextSecondary, m_colors.TextPrimary, hover);
        DrawIcon(icon, { pos.x + size.x * 0.5f, pos.y + size.y * 0.5f }, 11.0f, iconColor);
        return hovered && Clicked();
    }

    std::wstring UIRenderer::KeyName(int vk) {
        switch (vk) {
        case 0:           return L"None";
        case VK_LBUTTON:  return L"Mouse 1";
        case VK_RBUTTON:  return L"Mouse 2";
        case VK_MBUTTON:  return L"Mouse 3";
        case VK_XBUTTON1: return L"Mouse 4";
        case VK_XBUTTON2: return L"Mouse 5";
        default: break;
        }

        UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
        switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT:
        case VK_UP: case VK_DOWN: case VK_RCONTROL: case VK_RMENU:
        case VK_DIVIDE: case VK_NUMLOCK:
            scan |= 0x100;
            break;
        default: break;
        }

        wchar_t name[64]{};
        if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) {
            std::wstring result = name;
            // GetKeyNameText returns upper-case for many keys; keep only the first letter capital.
            for (size_t i = 1; i < result.size(); ++i) {
                if (result[i - 1] != L' ') result[i] = static_cast<wchar_t>(towlower(result[i]));
            }
            return result;
        }
        return L"Key " + std::to_wstring(vk);
    }

    bool UIRenderer::RenderTextInput(const Math::Vector2& pos,const std::wstring& label,std::wstring& value,size_t limit) {
        const auto id=HashId(label,(0x7E57)^m_idScope);
        DrawText(label,pos,{m_itemWidth,20},m_colors.TextSecondary,FontBody);
        const Math::Vector2 box{pos.x,pos.y+22},size{m_itemWidth,30};
        const bool hovered=IsHovered(box,size);
        if(Clicked()) {
            if(hovered){m_textFocus=id;m_textSelectAll=true;m_listeningKey=0;}
            else if(m_textFocus==id)m_textFocus=0;
        }
        bool changed=false;
        const bool focused=m_textFocus==id;
        if(focused) {
            m_textSeen=true;
            auto append=[&](std::wstring_view text) {
                if(m_textSelectAll){value.clear();m_textSelectAll=false;}
                for(auto ch:text)if(ch>=32&&value.size()<limit){value+=ch;changed=true;}
            };
            for(auto ch:m_textChars) {
                if(ch==1)m_textSelectAll=true;
                else if(ch==22) {
                    if(OpenClipboard(nullptr)) {
                        if(auto handle=GetClipboardData(CF_UNICODETEXT)) {
                            if(auto text=static_cast<const wchar_t*>(GlobalLock(handle))){append(text);GlobalUnlock(handle);}
                        }
                        CloseClipboard();
                    }
                } else if(ch==8) {
                    if(m_textSelectAll){value.clear();m_textSelectAll=false;changed=true;}
                    else if(!value.empty()) {
                        const auto last=value.back();value.pop_back();
                        if(last>=0xdc00&&last<=0xdfff&&!value.empty()&&value.back()>=0xd800&&value.back()<=0xdbff)value.pop_back();
                        changed=true;
                    }
                } else if(ch==13||ch==27)m_textFocus=0;
                else if(ch>=32)append(std::wstring_view(&ch,1));
            }
        }
        FillRoundedRect(box,size,6,m_colors.BgElevated);
        DrawRoundedRect(box,size,6,focused?m_colors.BorderActive:hovered?m_colors.BorderHover:m_colors.BorderSubtle,1);
        if(focused&&m_textSelectAll&&!value.empty())FillRect({box.x+8,box.y+5},{(std::min)(MeasureText(value),size.x-16),20},WithAlpha(m_colors.Accent,.25f));
        const float textWidth=MeasureText(value),scroll=focused?(std::max)(0.0f,textWidth-size.x+20):0;
        PushClip({box.x+8,box.y},{size.x-16,size.y});
        DrawText(value.empty()?label:value,{box.x+8-scroll,box.y},{(std::max)(textWidth+4,size.x-16),size.y},value.empty()?m_colors.TextTertiary:m_colors.TextPrimary,FontBody);
        PopClip();
        if(focused&&!m_textSelectAll&&std::fmod(m_time,1.0f)<.6f) {
            float caret=box.x+8+(std::min)(MeasureText(value),size.x-18);
            DrawLine({caret,box.y+7},{caret,box.y+23},m_colors.TextPrimary,1);
        }
        return changed;
    }

    bool UIRenderer::RenderKeybind(const Math::Vector2& pos, const std::wstring& label, int& key) {
        const uint64_t id = HashId(label, 0x4B3B^m_idScope);
        const float width = m_itemWidth;
        bool listening = (m_listeningKey == id);
        if (listening) m_listeningSeen = true;

        DrawText(label, pos, { width - 112.0f, kRowHeight }, m_colors.TextSecondary, FontBody);

        std::wstring keyText = listening ? L"Press a key" : KeyName(key);
        float boxW = std::clamp(MeasureText(keyText, FontBody) + 24.0f, 72.0f, 120.0f);
        Math::Vector2 boxPos{ pos.x + width - boxW, pos.y + 3.0f };
        Math::Vector2 boxSize{ boxW, kRowHeight - 6.0f };

        bool hovered = IsHovered(boxPos, boxSize);
        float hover = Animate(id, 0, hovered ? 1.0f : 0.0f, 22.0f);

        FillRoundedRect(boxPos, boxSize, 6.0f, listening ? m_colors.AccentGlow : m_colors.BgElevated);
        DrawRoundedRect(boxPos, boxSize, 6.0f, listening ? m_colors.BorderActive : Mix(m_colors.BorderSubtle, m_colors.BorderHover, hover), 1.0f);
        DrawText(keyText, boxPos, boxSize, listening ? m_colors.Accent : m_colors.TextPrimary, FontBody, DWRITE_TEXT_ALIGNMENT_CENTER);

        bool changed = false;
        if (!listening && hovered && Clicked()) {
            m_listeningKey = id;
            m_listeningSeen = true;
            // Snapshot held keys so the click that started listening is not captured.
            for (int k = 1; k < 256; ++k) m_keyWasDown[k] = (GetAsyncKeyState(k) & 0x8000) != 0;
        } else if (listening) {
            for (int k = 1; k < 256; ++k) {
                bool down = (GetAsyncKeyState(k) & 0x8000) != 0;
                bool pressed = down && !m_keyWasDown[k];
                m_keyWasDown[k] = down;
                if (!pressed) continue;

                if (k == VK_ESCAPE) {
                    m_listeningKey = 0;
                } else if (k == VK_LBUTTON && !hovered) {
                    m_listeningKey = 0; // clicking elsewhere cancels
                } else if (k == VK_BACK) {
                    key = 0;
                    m_listeningKey = 0;
                    changed = true;
                } else {
                    key = k;
                    m_listeningKey = 0;
                    changed = true;
                }
                break;
            }
        }
        return changed;
    }

    bool UIRenderer::RenderCombo(const Math::Vector2& pos, const std::wstring& label, int& selectedIndex, const std::vector<std::wstring>& options) {
        const uint64_t id = HashId(label, 0xC0B0^m_idScope);
        const float width = m_itemWidth;

        if (m_comboChangedId == id) {
            m_comboChangedId = 0;
            if (m_comboChangedIndex >= 0 && m_comboChangedIndex < static_cast<int>(options.size())) {
                selectedIndex = m_comboChangedIndex;
                return true;
            }
        }

        float boxW = (std::min)(160.0f, width * 0.55f);
        Math::Vector2 boxPos{ pos.x + width - boxW, pos.y + 3.0f };
        Math::Vector2 boxSize{ boxW, kRowHeight - 6.0f };
        bool open = (m_openCombo == id);
        if (open) m_openComboSeen = true;

        DrawText(label, pos, { width - boxW - 12.0f, kRowHeight }, m_colors.TextSecondary, FontBody);

        bool hovered = IsHovered(boxPos, boxSize);
        float hover = Animate(id, 0, (hovered || open) ? 1.0f : 0.0f, 22.0f);

        FillRoundedRect(boxPos, boxSize, 6.0f, m_colors.BgElevated);
        DrawRoundedRect(boxPos, boxSize, 6.0f, open ? m_colors.BorderActive : Mix(m_colors.BorderSubtle, m_colors.BorderHover, hover), 1.0f);

        std::wstring current = (selectedIndex >= 0 && selectedIndex < static_cast<int>(options.size())) ? options[selectedIndex] : L"Choose";
        DrawText(current, { boxPos.x + 10.0f, boxPos.y }, { boxSize.x - 34.0f, boxSize.y }, m_colors.TextPrimary, FontBody);
        DrawIcon(Icon::Chevron, { boxPos.x + boxSize.x - 14.0f, boxPos.y + boxSize.y * 0.5f }, 9.0f, m_colors.TextSecondary);

        if (hovered && Clicked()) {
            m_openCombo = id;
            m_textFocus=0;
            m_comboScroll=std::clamp(selectedIndex-4,0,(std::max)(0,static_cast<int>(options.size())-10));
            m_openColor=0;m_colorValue=nullptr;
            m_openComboSeen = true;
            m_comboAnchor = { boxPos.x, boxPos.y, boxPos.x + boxSize.x, boxPos.y + boxSize.y };
            m_comboOptions = options;
            m_comboSelected = selectedIndex;
            SetAnim(id, 9, 0.0f);
        }
        return false;
    }

    D2D1_RECT_F UIRenderer::PopupRect() const {
        float w = m_comboAnchor.right - m_comboAnchor.left;
        float h = static_cast<float>((std::min)(m_comboOptions.size(),size_t{10})) * kPopupItemHeight + 8.0f;
        float top = m_comboAnchor.bottom + 4.0f;
        if (m_pRenderTarget) {
            D2D1_SIZE_F rt = m_pRenderTarget->GetSize();
            if (top + h > rt.height - 4.0f) top = m_comboAnchor.top - 4.0f - h;
        }
        top=(std::max)(4.0f,top);
        return { m_comboAnchor.left, top, m_comboAnchor.left + w, top + h };
    }

    D2D1_RECT_F UIRenderer::ColorPopupRect() const {
        const auto size=m_pRenderTarget->GetSize();
        constexpr float w=252,h=284;
        float x=std::clamp(m_colorAnchor.x-w+28,4.0f,(std::max)(4.0f,size.width-w-4));
        float y=m_colorAnchor.y+26;
        if(y+h>size.height-4)y=m_colorAnchor.y-h-6;
        y=std::clamp(y,4.0f,(std::max)(4.0f,size.height-h-4));
        return {x,y,x+w,y+h};
    }

    void UIRenderer::RenderColorButton(const Math::Vector2& pos,const std::wstring& label,Config::Color4& color) {
        const auto id=HashId(label,(47)^m_idScope);
        if(m_openColor==id) {m_colorSeen=true;m_colorValue=&color;m_colorAnchor=pos;}
        const bool hover=IsHovered(pos,{28,20});
        FillRoundedRect(pos,{28,20},4,m_colors.TextTertiary);
        FillRoundedRect(pos,{28,20},4,FromColor(color));
        DrawRoundedRect(pos,{28,20},4,hover?m_colors.TextPrimary:m_colors.BorderHover,1);
        if(hover&&Clicked()) {
            m_openCombo=0;m_activeSlider=0;
            m_openColor=id;m_colorValue=&color;m_colorAnchor=pos;m_colorLabel=label;m_colorSeen=true;
            m_clickEatenByPopup=true;
        }
    }

    void UIRenderer::RenderPopups() {
        if(m_openColor) {
            if(!m_colorSeen||!m_colorValue){ClosePopups();return;}
            m_inPopupPass=true;
            const auto r=ColorPopupRect();
            const Math::Vector2 p{r.left,r.top},size{r.right-r.left,r.bottom-r.top};
            DrawShadow(p,size,8,6,14,.55f);
            FillRoundedRect(p,size,8,m_colors.BgElevated);
            DrawRoundedRect(p,size,8,m_colors.BorderHover,1);
            DrawText(m_colorLabel,{p.x+12,p.y+8},{size.x-50,24},m_colors.TextPrimary,FontBold);
            if(RenderIconButton({r.right-32,p.y+7},{24,24},Icon::Close)) {
                ClosePopups();m_inPopupPass=false;return;
            }
            FillRoundedRect({p.x+12,p.y+38},{size.x-24,28},4,m_colors.BgBase);
            FillRoundedRect({p.x+12,p.y+38},{size.x-24,28},4,FromColor(*m_colorValue));
            const float previousWidth=m_itemWidth;m_itemWidth=size.x-24;
            RenderSlider({p.x+12,p.y+78},L"Red",m_colorValue->r,0,1,L"",2);
            RenderSlider({p.x+12,p.y+126},L"Green",m_colorValue->g,0,1,L"",2);
            RenderSlider({p.x+12,p.y+174},L"Blue",m_colorValue->b,0,1,L"",2);
            RenderSlider({p.x+12,p.y+222},L"Opacity",m_colorValue->a,0,1,L"",2);
            m_itemWidth=previousWidth;m_inPopupPass=false;
            return;
        }
        if (!m_openCombo) return;
        if (!m_openComboSeen) {
            m_openCombo = 0; // owner was not drawn this frame (tab changed)
            return;
        }

        m_inPopupPass = true;
        D2D1_RECT_F r = PopupRect();
        float appear = Animate(m_openCombo, 9, 1.0f, 26.0f);
        float lift = (1.0f - appear) * -4.0f;

        Math::Vector2 pos{ r.left, r.top + lift };
        Math::Vector2 size{ r.right - r.left, r.bottom - r.top };

        // Surface is opaque from the first frame so nothing behind bleeds through; only contents fade.
        DrawShadow(pos, size, 8.0f, 6.0f, 14.0f, 0.55f);
        FillRoundedRect(pos, size, 8.0f, Mix(m_colors.BgElevated, D2D1::ColorF(D2D1::ColorF::White), 0.02f));
        DrawRoundedRect(pos, size, 8.0f, m_colors.BorderHover, 1.0f);

        const auto visibleCount=(std::min)(m_comboOptions.size(),size_t{10});
        for (size_t row=0;row<visibleCount;++row) {
            const size_t i=row+m_comboScroll;
            if(i>=m_comboOptions.size())break;
            Math::Vector2 itemPos{ pos.x + 4.0f, pos.y + 4.0f + row * kPopupItemHeight };
            Math::Vector2 itemSize{ size.x - 8.0f, kPopupItemHeight };
            bool hovered = IsHovered(itemPos, itemSize);
            bool selected = static_cast<int>(i) == m_comboSelected;

            if (hovered) FillRoundedRect(itemPos, itemSize, 5.0f, WithAlpha(D2D1::ColorF(D2D1::ColorF::White), 0.06f * appear));

            D2D1_COLOR_F text = (hovered || selected) ? m_colors.TextPrimary : m_colors.TextSecondary;
            DrawText(m_comboOptions[i], { itemPos.x + 8.0f, itemPos.y }, { itemSize.x - 32.0f, itemSize.y }, WithAlpha(text, appear), FontBody);
            if (selected) {
                DrawIcon(Icon::Check, { itemPos.x + itemSize.x - 14.0f, itemPos.y + itemSize.y * 0.5f }, 10.0f, WithAlpha(m_colors.Accent, appear));
            }

            if (hovered && m_mouseClicked) {
                m_comboChangedId = m_openCombo;
                m_comboChangedIndex = static_cast<int>(i);
                m_openCombo = 0;
                break;
            }
        }
        if(m_comboOptions.size()>10) {
            const float track=size.y-8,thumb=track*10/static_cast<float>(m_comboOptions.size());
            const float offset=(track-thumb)*m_comboScroll/static_cast<float>(m_comboOptions.size()-10);
            FillRoundedRect({pos.x+size.x-4,pos.y+4+offset},{2,thumb},1,m_colors.BorderHover);
        }
        m_inPopupPass = false;
    }

    void UIRenderer::RenderBadge(const Math::Vector2& pos, const std::wstring& text, const D2D1_COLOR_F& color) {
        float w = MeasureText(text, FontSmall) + 14.0f;
        float h = 18.0f;
        FillRoundedRect(pos, { w, h }, 4.0f, WithAlpha(color, 0.14f));
        DrawText(text, pos, { w, h }, color, FontSmall, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    void UIRenderer::RenderProgressBar(const Math::Vector2& pos, const Math::Vector2& size, float progress, const D2D1_COLOR_F& fillColor) {
        float radius = size.y * 0.5f;
        FillRoundedRect(pos, size, radius, m_colors.BgElevated);
        float fillW = size.x * std::clamp(progress, 0.0f, 1.0f);
        if (fillW > 0.5f) {
            FillRoundedRect(pos, { (std::max)(fillW, size.y), size.y }, radius, fillColor);
        }
    }

    void UIRenderer::RenderCard(const Math::Vector2& pos, const Math::Vector2& size, const std::wstring& title, const std::wstring& subtitle) {
        FillRoundedRect(pos, size, 8.0f, m_colors.BgSurface);
        DrawRoundedRect(pos, size, 8.0f, m_colors.BorderSubtle, 1.0f);
        DrawText(title, { pos.x + 14.0f, pos.y + 10.0f }, { size.x - 28.0f, 20.0f }, m_colors.TextPrimary, FontHeader);
        if (!subtitle.empty()) {
            DrawText(subtitle, { pos.x + 14.0f, pos.y + 30.0f }, { size.x - 28.0f, 16.0f }, m_colors.TextTertiary, FontSmall);
        }
    }

} // namespace UI
