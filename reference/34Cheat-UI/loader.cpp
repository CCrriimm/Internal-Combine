// 34cheat/src/loader.cpp
// context: C++20 / loader.cpp / MSVC / x64 / Windows 10/11 / version-5a21b87bbc2641cf
#include "loader.hpp"
#include "../include/obfuscation.hpp"
#include "../include/memory.hpp"
#include "../include/sdk.hpp"
#include "../include/updater.hpp"
#include <d3d11.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <thread>
#include <sstream>
#include <iomanip>

namespace Loader {

    namespace {
        enum Step { StepSyscalls = 0, StepOffsets, StepProcess, StepDataModel, StepCount };

        constexpr float kPi = 3.14159265f;
        constexpr UINT_PTR kMoveLoopTimer = 1;
        constexpr float kAutoLaunchDelay = 1.2f;
        constexpr const wchar_t* kAppVersion = L"1.0.0"; // keep in step with resources/app.rc

        // Intro timeline in seconds. The hand-over to the loader runs from kIntroExit to kIntroEnd.
        constexpr float kIntroExit = 2.85f;
        constexpr float kIntroEnd = 3.30f;

        float Phase(float t, float from, float to) {
            return std::clamp((t - from) / (to - from), 0.0f, 1.0f);
        }
        float EaseOut(float x) { return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x); }
        float EaseInOut(float x) { return x < 0.5f ? 4.0f * x * x * x : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) / 2.0f; }
    }

    static LRESULT WINAPI LoaderWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_NCHITTEST: {
            // Title strip drags the window natively (snap, multi-monitor) except over the buttons.
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hWnd, &pt);
            const auto& size = g_Loader.GetWindowSize();
            if (pt.y >= 0 && pt.y < ExternalLoader::kTitlebarHeight && pt.x < size.x - ExternalLoader::kTitlebarButtonsWidth) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_KEYDOWN:
            g_Loader.OnKey(wParam);
            return 0;
        // Holding the title strip runs Windows' modal move loop, which starves Run(). A timer keeps frames coming.
        case WM_ENTERSIZEMOVE:
            SetTimer(hWnd, kMoveLoopTimer, USER_TIMER_MINIMUM, nullptr);
            return 0;
        case WM_EXITSIZEMOVE:
            KillTimer(hWnd, kMoveLoopTimer);
            return 0;
        case WM_TIMER:
            if (wParam == kMoveLoopTimer) {
                g_Loader.Tick();
                return 0;
            }
            break;
        case WM_DESTROY:
            // Launch destroys only the loader window; the overlay reuses this thread.
            if (!g_Loader.IsLaunched()) PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    void ExternalLoader::InitSteps() {
        m_steps = {
            { L"System calls", L"Binding read and open calls directly", StepStatus::Working },
            { L"Offsets", L"Checking for offsets that match the current build", StepStatus::Pending },
            { L"Roblox", L"Waiting for RobloxPlayerBeta.exe", StepStatus::Pending },
            { L"Game data", L"Reading DataModel and the render view", StepStatus::Pending },
        };

        DWORD volSerial = 0;
        GetVolumeInformationA("C:\\", NULL, 0, &volSerial, NULL, NULL, NULL, 0);
        std::wstringstream ss;
        ss << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0') << volSerial;
        m_hwidHash = ss.str();
        m_syncedVersion = L"Checking";
    }

    bool ExternalLoader::Initialize(HINSTANCE hInstance) {
        InitSteps();

        const wchar_t* className = L"34Cheat_Loader_Class";
        ZeroMemory(&m_wc, sizeof(m_wc));
        m_wc.cbSize = sizeof(WNDCLASSEXW);
        m_wc.style = CS_HREDRAW | CS_VREDRAW;
        m_wc.lpfnWndProc = LoaderWndProc;
        m_wc.hInstance = hInstance;
        m_wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); // IDC_ARROW, independent of UNICODE
        // IDI_APP (101) from resources/app.rc: taskbar, Alt+Tab and title bar use the logo, not the default exe icon.
        m_wc.hIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
        m_wc.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        m_wc.lpszClassName = className;

        RegisterClassExW(&m_wc);

        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int posX = (screenW - static_cast<int>(m_windowSize.x)) / 2;
        int posY = (screenH - static_cast<int>(m_windowSize.y)) / 2;

        m_hwnd = CreateWindowExW(
            WS_EX_APPWINDOW,
            className,
            L"34Cheat",
            WS_POPUP | WS_MINIMIZEBOX,
            posX, posY,
            static_cast<int>(m_windowSize.x),
            static_cast<int>(m_windowSize.y),
            NULL, NULL, hInstance, NULL
        );

        if (!m_hwnd) return false;

        // Native shadow plus Windows 11 rounded corners; older builds ignore the attribute.
        MARGINS margins = { 1, 1, 1, 1 };
        DwmExtendFrameIntoClientArea(m_hwnd, &margins);
        const DWORD kCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
        int round = 2;                      // DWMWCP_ROUND
        DwmSetWindowAttribute(m_hwnd, kCornerPreference, &round, sizeof(round));
        BOOL dark = TRUE;
        DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);

        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = static_cast<UINT>(m_windowSize.x);
        sd.BufferDesc.Height = static_cast<UINT>(m_windowSize.y);
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = m_hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
            D3D11_SDK_VERSION, &sd, &m_pSwapChain, &m_pd3dDevice, &featureLevel, &m_pd3dDeviceContext
        );

        if (FAILED(hr)) return false;

        CreateRenderTarget();
        return UI::g_UI.Initialize(m_pSwapChain);
    }

    void ExternalLoader::CreateRenderTarget() {
        ID3D11Texture2D* pBackBuffer = nullptr;
        if (m_pSwapChain) {
            m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            if (pBackBuffer) {
                m_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &m_pRenderTargetView);
                pBackBuffer->Release();
            }
        }
    }

    void ExternalLoader::CleanupRenderTarget() {
        if (m_pRenderTargetView) {
            m_pRenderTargetView->Release();
            m_pRenderTargetView = nullptr;
        }
    }

    void ExternalLoader::OnKey(WPARAM key) {
        if (key == VK_ESCAPE) {
            m_running = false;
        } else if (key == VK_RETURN && m_introDone && IsReady()) {
            m_launchRequested = true;
        }
    }

    void ExternalLoader::CompleteStep(float progress) {
        Diagnostics::Log("Loader step completed index=", m_currentStepIndex, " progress=", progress);
        m_steps[m_currentStepIndex].Status = StepStatus::Completed;
        m_currentStepIndex++;
        if (m_currentStepIndex < static_cast<int>(m_steps.size())) {
            m_steps[m_currentStepIndex].Status = StepStatus::Working;
        }
        m_progress = progress;
    }

    void ExternalLoader::AdvanceStep() {
        if (m_currentStepIndex >= static_cast<int>(m_steps.size())) return;

        switch (m_currentStepIndex) {
        case StepSyscalls:
            if (Obfuscation::InitSyscalls()) {
                CompleteStep(0.2f);
            } else {
                m_steps[StepSyscalls].Status = StepStatus::Failed;
                m_steps[StepSyscalls].Detail = L"Could not resolve ntdll exports. Restart and try again";
            }
            break;

        case StepOffsets: {
            // The HTTP request runs off the UI thread so the window keeps drawing.
            if (!m_syncTask.valid()) {
                m_syncTask = std::async(std::launch::async, [] { return Updater::g_Offsets.Sync(); });
                break;
            }
            if (m_syncTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) break;

            auto sync = m_syncTask.get();
            Diagnostics::Log("Offset sync success=", sync.Success, " updated=", sync.Updated,
                " loaded=", Updater::g_Offsets.GetTotalLoaded(), " reported_count=", sync.OffsetCount,
                " version=", sync.LiveVersion, " cached_version=", sync.CachedVersion,
                " error=", sync.ErrorMessage);
            std::string live = (sync.Success && !sync.LiveVersion.empty()) ? sync.LiveVersion : Offsets::ClientVersion;
            m_syncedVersion = std::wstring(live.begin(), live.end());

            std::wstringstream dss;
            switch (sync.Source) {
            case Updater::OffsetSource::Downloaded:
                dss << L"Downloaded " << sync.OffsetCount << L" offsets";
                break;
            case Updater::OffsetSource::Saved:
                if (!sync.Online) dss << L"Offline. Using " << sync.OffsetCount << L" saved offsets";
                else if (sync.ErrorMessage.empty()) dss << L"Up to date, " << sync.OffsetCount << L" offsets";
                else dss << L"Update failed. Using " << sync.OffsetCount << L" saved offsets";
                break;
            case Updater::OffsetSource::Bundled:
                dss << (sync.Online ? L"Update failed. Using built-in offsets" : L"Offline. Using built-in offsets");
                break;
            }
            m_steps[StepOffsets].Detail = dss.str();
            CompleteStep(0.45f);
            break;
        }

        case StepProcess:
            if (Memory::g_Memory.Attach(_XOR("RobloxPlayerBeta.exe"))) {
                m_steps[StepProcess].Detail = L"Attached to process " + std::to_wstring(Memory::g_Memory.GetPid());
                CompleteStep(0.7f);
            }
            break;

        case StepDataModel:
            if (SDK::g_Roblox.Initialize()) {
                m_steps[StepDataModel].Detail = L"Connected to the running experience";
                CompleteStep(1.0f);
            }
            break;
        }
    }

    void ExternalLoader::RenderStep(const PipelineStep& step, const Math::Vector2& pos, float width, bool last) {
        auto& ui = UI::g_UI;
        const auto& colors = ui.GetColors();
        const float rowH = 58.0f;
        Math::Vector2 marker{ pos.x + 10.0f, pos.y + 11.0f };

        // Connector to the next marker, filled once this step is done.
        if (!last) {
            ui.DrawLine({ marker.x, marker.y + 14.0f }, { marker.x, pos.y + rowH + 1.0f },
                step.Status == StepStatus::Completed ? UI::WithAlpha(colors.Success, 0.45f) : colors.BorderSubtle, 1.5f);
        }

        D2D1_COLOR_F titleColor = colors.TextPrimary;
        switch (step.Status) {
        case StepStatus::Completed:
            ui.FillCircle(marker, 9.0f, colors.Success);
            ui.DrawIcon(UI::Icon::Check, marker, 10.0f, D2D1::ColorF(0x0F1012, 1.0f));
            break;
        case StepStatus::Working:
            ui.DrawCircle(marker, 8.0f, colors.BorderSubtle, 2.0f);
            ui.DrawArc(marker, 8.0f, ui.Time() * 5.0f, 3.14159f * 1.2f, colors.Accent, 2.0f);
            break;
        case StepStatus::Failed:
            ui.FillCircle(marker, 9.0f, colors.Danger);
            ui.DrawIcon(UI::Icon::Cross, marker, 9.0f, D2D1::ColorF(0xFFFFFF, 1.0f));
            break;
        case StepStatus::Pending:
            ui.DrawCircle(marker, 8.0f, colors.BorderHover, 1.5f);
            titleColor = colors.TextTertiary;
            break;
        }

        float textX = pos.x + 34.0f;
        ui.DrawText(step.Title, { textX, pos.y + 1.0f }, { width - 34.0f, 20.0f }, titleColor, UI::FontBold);
        ui.DrawText(step.Detail, { textX, pos.y + 21.0f }, { width - 34.0f, 18.0f },
            step.Status == StepStatus::Failed ? colors.Danger : colors.TextSecondary, UI::FontSmall);
    }

    void ExternalLoader::RenderStatusMark(const Math::Vector2& pos, float size, bool ready, bool failed) {
        auto& ui = UI::g_UI;
        const auto& colors = ui.GetColors();
        const Math::Vector2 c{ pos.x + size * 0.5f, pos.y + size * 0.5f };
        const float r = size * 0.5f + 7.0f;

        ui.DrawLogoMark(pos, size);
        if (failed) {
            ui.DrawCircle(c, r, UI::WithAlpha(colors.Danger, 0.7f), 1.5f);
        } else if (ready) {
            ui.DrawCircle(c, r, UI::WithAlpha(colors.Success, 0.7f), 1.5f);
        } else {
            const float spin = ui.Time() * 2.4f;
            ui.DrawCircle(c, r, colors.BorderSubtle, 1.5f);
            ui.DrawArc(c, r, spin, kPi * 0.55f, colors.Accent, 2.0f);
            ui.DrawArc(c, r, spin + kPi, kPi * 0.55f, colors.Accent, 2.0f);
        }
    }

    void ExternalLoader::RenderTitleButtons() {
        auto& ui = UI::g_UI;
        const float W = m_windowSize.x;
        if (ui.RenderIconButton({ W - 80.0f, 8.0f }, { 32.0f, 28.0f }, UI::Icon::Minimize)) {
            ShowWindow(m_hwnd, SW_MINIMIZE);
        }
        if (ui.RenderIconButton({ W - 44.0f, 8.0f }, { 32.0f, 28.0f }, UI::Icon::Close, true)) {
            m_running = false;
        }
    }

    // exit 0..1 fades the intro out over the loader, which is already drawn underneath.
    void ExternalLoader::RenderIntro(float exit) {
        auto& ui = UI::g_UI;
        const auto& colors = ui.GetColors();
        const float t = m_introTime;
        const float W = m_windowSize.x;
        const float fade = 1.0f - EaseInOut(exit);
        const float lift = -EaseInOut(exit) * 14.0f;

        ui.FillRect({ 0.0f, 0.0f }, m_windowSize, UI::WithAlpha(colors.BgBase, fade));

        const float tile = 76.0f;
        const Math::Vector2 c{ W * 0.5f, 148.0f + lift };

        // 1. A square outline turns in from 135 degrees and opens to full size.
        const float open = EaseOut(Phase(t, 0.05f, 0.70f));
        const float fill = EaseOut(Phase(t, 0.45f, 0.95f));
        if (open > 0.0f && fill < 1.0f) {
            const float half = tile * 0.5f * (0.18f + 0.82f * open);
            const float rot = (1.0f - open) * kPi * 0.75f;
            std::vector<Math::Vector2> square;
            for (int i = 0; i < 4; ++i) {
                float a = rot + kPi * 0.25f + i * kPi * 0.5f;
                square.push_back({ c.x + std::cos(a) * half * 1.41421f, c.y + std::sin(a) * half * 1.41421f });
            }
            ui.DrawPolygon(square, UI::WithAlpha(colors.Accent, open * (1.0f - fill) * fade), 2.0f);
        }

        // 2. The tile fills from the centre, then the cube faces drop in.
        if (fill > 0.0f) {
            if (fill < 1.0f) {
                const float s = tile * fill;
                ui.FillRoundedRect({ c.x - s * 0.5f, c.y - s * 0.5f }, { s, s }, s * 0.28f, UI::WithAlpha(colors.Accent, fade));
            } else {
                ui.DrawLogoMark({ c.x - tile * 0.5f, c.y - tile * 0.5f }, tile, Phase(t, 0.80f, 1.55f), fade);
            }
        }

        // 3. Counter-rotating arcs orbit the tile while it loads.
        const float orbit = EaseOut(Phase(t, 1.00f, 1.60f));
        if (orbit > 0.0f) {
            const float spin = t * 2.4f;
            const float r1 = tile * 0.5f + 16.0f + (1.0f - orbit) * 10.0f;
            const float r2 = tile * 0.5f + 25.0f + (1.0f - orbit) * 16.0f;
            const D2D1_COLOR_F inner = UI::WithAlpha(colors.Accent, 0.9f * orbit * fade);
            ui.DrawArc(c, r1, -kPi * 0.85f + spin, kPi * 0.62f * orbit, inner, 2.5f);
            ui.DrawArc(c, r1, kPi * 0.15f + spin, kPi * 0.62f * orbit, inner, 2.5f);
            ui.DrawArc(c, r2, kPi * 0.40f - spin * 0.7f, kPi * 0.45f * orbit, UI::WithAlpha(colors.Accent, 0.35f * orbit * fade), 1.5f);
        }

        // 4. Wordmark and subtitle rise in under the mark.
        const float word = EaseOut(Phase(t, 1.20f, 1.70f));
        if (word > 0.0f) {
            const float wy = 236.0f + lift + (1.0f - word) * 10.0f;
            const float numW = ui.MeasureText(L"34", UI::FontTitle);
            const float wordW = ui.MeasureText(L"Cheat", UI::FontTitle);
            const float x = c.x - (numW + wordW) * 0.5f;
            ui.DrawText(L"34", { x, wy }, { numW + 2.0f, 30.0f }, UI::WithAlpha(colors.TextPrimary, word * fade), UI::FontTitle);
            ui.DrawText(L"Cheat", { x + numW, wy }, { wordW + 2.0f, 30.0f }, UI::WithAlpha(colors.TextSecondary, word * fade), UI::FontTitle);

            const float sub = EaseOut(Phase(t, 1.35f, 1.85f));
            ui.DrawText(L"Roblox external", { c.x - 120.0f, wy + 30.0f }, { 240.0f, 18.0f },
                UI::WithAlpha(colors.TextTertiary, sub * fade), UI::FontSmall, DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // 5. Progress line with what it is doing.
        const float bar = EaseOut(Phase(t, 1.45f, 1.85f));
        if (bar > 0.0f) {
            const float barW = 220.0f;
            const float by = 318.0f + lift;
            const float progress = EaseInOut(Phase(t, 1.55f, 2.75f));
            ui.FillRoundedRect({ c.x - barW * 0.5f, by }, { barW, 3.0f }, 1.5f, UI::WithAlpha(colors.BorderSubtle, colors.BorderSubtle.a * bar * fade));
            if (progress > 0.0f) {
                ui.FillRoundedRect({ c.x - barW * 0.5f, by }, { barW * progress, 3.0f }, 1.5f, UI::WithAlpha(colors.Accent, bar * fade));
            }
            const wchar_t* status = progress < 0.45f ? L"Loading interface" : (progress < 0.97f ? L"Preparing startup" : L"Opening loader");
            ui.DrawText(status, { c.x - 140.0f, by + 12.0f }, { 280.0f, 18.0f },
                UI::WithAlpha(colors.TextSecondary, bar * fade), UI::FontSmall, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    void ExternalLoader::RenderFrame() {
        auto& ui = UI::g_UI;
        const auto& colors = ui.GetColors();
        const float W = m_windowSize.x;
        const float H = m_windowSize.y;

        ui.FillRect({ 0.0f, 0.0f }, m_windowSize, colors.BgBase);

        // Title strip: logo, a quiet build tag, window buttons, and a hairline separating it from the content.
        const float logoW = ui.DrawLogo({ 20.0f, 11.0f }, 22.0f);
        {
            const std::wstring tag = L"v" + std::wstring(kAppVersion);
            const float tagW = ui.MeasureText(tag, UI::FontSmall) + 14.0f;
            const Math::Vector2 tagPos{ 20.0f + logoW + 10.0f, 13.0f };
            ui.FillRoundedRect(tagPos, { tagW, 18.0f }, 9.0f, colors.BgElevated);
            ui.DrawText(tag, tagPos, { tagW, 18.0f }, colors.TextTertiary, UI::FontSmall, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        ui.DrawLine({ 0.0f, kTitlebarHeight }, { W, kTitlebarHeight }, colors.BorderSubtle, 1.0f);
        RenderTitleButtons();

        // Left: state in words, the facts behind it, and the one action.
        const float leftX = 28.0f;
        const float leftW = 264.0f;
        const bool ready = IsReady();
        const bool failed = std::any_of(m_steps.begin(), m_steps.end(), [](const PipelineStep& s) { return s.Status == StepStatus::Failed; });
        const bool waitingForGame = m_currentStepIndex == StepProcess;

        std::wstring headline, line;
        if (failed) {
            headline = L"Setup stopped";
            line = L"See the step marked in red.";
        } else if (ready) {
            headline = L"Launching";
            line = UI::UIRenderer::KeyName(Config::g_Config.Customization.MenuHotkey) + L" opens the menu, "
                + UI::UIRenderer::KeyName(Config::g_Config.Customization.PanicHotkey) + L" unloads.";
        } else if (waitingForGame) {
            headline = L"Waiting for Roblox";
            line = L"Open Roblox and join a game.";
        } else {
            headline = L"Setting up";
            line = L"This takes a few seconds.";
        }

        float shown = ui.Animate(UI::HashId(L"loader.progress"), 0, m_progress, 8.0f);

        // The intro's mark stays on as the status: orbiting while working, a closed ring once settled.
        RenderStatusMark({ leftX, 66.0f }, 40.0f, ready, failed);

        ui.DrawText(headline, { leftX, 120.0f }, { leftW, 30.0f }, colors.TextPrimary, UI::FontTitle);
        ui.DrawText(line, { leftX, 152.0f }, { leftW, 20.0f }, colors.TextSecondary, UI::FontBody);

        auto factRow = [&](float y, const std::wstring& label, const std::wstring& value) {
            ui.DrawLine({ leftX, y }, { leftX + leftW, y }, colors.BorderSubtle, 1.0f);
            ui.DrawText(label, { leftX, y + 1.0f }, { 110.0f, 32.0f }, colors.TextTertiary, UI::FontBody);
            ui.DrawText(value, { leftX + 110.0f, y + 1.0f }, { leftW - 110.0f, 32.0f }, colors.TextPrimary, UI::FontMono, DWRITE_TEXT_ALIGNMENT_TRAILING);
        };
        // "version-5a21b87bbc2641cf" does not fit the column; the hash alone does and is what people compare.
        std::wstring build = m_syncedVersion;
        if (build.rfind(L"version-", 0) == 0) build = build.substr(8);
        const bool attached = m_currentStepIndex > StepProcess;
        factRow(190.0f, L"Roblox build", build);
        factRow(223.0f, L"Device", m_hwidHash);
        factRow(256.0f, L"Process", attached ? L"PID " + std::to_wstring(Memory::g_Memory.GetPid()) : L"Not found");
        ui.DrawLine({ leftX, 289.0f }, { leftX + leftW, 289.0f }, colors.BorderSubtle, 1.0f);

        std::wstringstream pct;
        pct << static_cast<int>(std::round(shown * 100.0f)) << L"%";
        ui.DrawText(pct.str(), { leftX + leftW - 80.0f, H - 106.0f }, { 80.0f, 16.0f },
            failed ? colors.Danger : (ready ? colors.Success : colors.TextSecondary), UI::FontMono, DWRITE_TEXT_ALIGNMENT_TRAILING);

        ui.RenderProgressBar({ leftX, H - 86.0f }, { leftW, 4.0f }, shown, ready ? colors.Success : colors.Accent);

        std::wstring buttonLabel = failed ? L"Close" : (ready ? L"Launch now" : L"Launch");
        if (ui.RenderButton({ leftX, H - 66.0f }, { leftW, 38.0f }, buttonLabel, ready || failed, ready || failed)) {
            if (failed) m_running = false;
            else m_launchRequested = true;
        }

        // Right: the pipeline as a timeline on its own surface.
        const float panelX = leftX + leftW + 32.0f;
        const float panelY = 58.0f;
        const float panelW = W - panelX - 16.0f;
        const float panelH = H - panelY - 16.0f;
        ui.FillRoundedRect({ panelX, panelY }, { panelW, panelH }, 10.0f, colors.BgSurface);
        ui.DrawRoundedRect({ panelX, panelY }, { panelW, panelH }, 10.0f, colors.BorderSubtle, 1.0f);

        ui.DrawText(L"Startup", { panelX + 20.0f, panelY + 16.0f }, { 160.0f, 20.0f }, colors.TextPrimary, UI::FontHeader);

        const int total = static_cast<int>(m_steps.size());
        const std::wstring counter = ready ? L"Done"
            : std::to_wstring((std::min)(m_currentStepIndex + 1, total)) + L" of " + std::to_wstring(total);
        ui.DrawText(counter, { panelX + panelW - 120.0f, panelY + 18.0f }, { 100.0f, 18.0f },
            ready ? colors.Success : colors.TextTertiary, UI::FontMono, DWRITE_TEXT_ALIGNMENT_TRAILING);
        ui.DrawLine({ panelX + 20.0f, panelY + 46.0f }, { panelX + panelW - 20.0f, panelY + 46.0f }, colors.BorderSubtle, 1.0f);

        float stepY = panelY + 62.0f;
        for (size_t i = 0; i < m_steps.size(); ++i) {
            RenderStep(m_steps[i], { panelX + 20.0f, stepY }, panelW - 40.0f, i + 1 == m_steps.size());
            stepY += 58.0f;
        }

        const wchar_t* hint = ready ? L"Enter launches now  \x00B7  Esc closes" : L"Esc closes this window";
        ui.DrawText(hint, { panelX + 20.0f, panelY + panelH - 32.0f }, { panelW - 40.0f, 18.0f }, colors.TextTertiary, UI::FontSmall);
    }

    bool ExternalLoader::Run() {
        MSG msg;
        ZeroMemory(&msg, sizeof(msg));

        while (m_running) {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) {
                    m_running = false;
                    return false;
                }
            }
            if (m_running) Tick();
        }

        // Do not leave the offsets request running past the loader's lifetime.
        if (m_syncTask.valid()) m_syncTask.wait();
        return m_launched;
    }

    void ExternalLoader::Tick() {
        AdvanceStep();

        POINT mousePt{};
        GetCursorPos(&mousePt);
        ScreenToClient(m_hwnd, &mousePt);
        bool mouseDown = (GetForegroundWindow() == m_hwnd) && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const Math::Vector2 mouse{ static_cast<float>(mousePt.x), static_cast<float>(mousePt.y) };

        if (!m_introDone) {
            m_introTime += UI::g_UI.DeltaTime();
            if (m_introTime >= kIntroEnd || Config::g_Config.Customization.ReducedMotion) m_introDone = true;
        }
        const bool inIntro = !m_introDone;
        const float exit = inIntro ? Phase(m_introTime, kIntroExit, kIntroEnd) : 0.0f;

        // The intro always plays to the end. Until then, only the title buttons see the pointer.
        const bool pointerLive = !inIntro || (mouse.y < kTitlebarHeight && mouse.x >= m_windowSize.x - kTitlebarButtonsWidth);
        UI::g_UI.UpdateInput(pointerLive ? mouse : Math::Vector2{ -1000.0f, -1000.0f }, pointerLive && mouseDown);

        const auto& bg = UI::g_UI.GetColors().BgBase;
        const float clearColor[4] = { bg.r, bg.g, bg.b, 1.0f };
        m_pd3dDeviceContext->OMSetRenderTargets(1, &m_pRenderTargetView, NULL);
        m_pd3dDeviceContext->ClearRenderTargetView(m_pRenderTargetView, clearColor);

        UI::g_UI.BeginDraw();
        if (!inIntro || exit > 0.0f) RenderFrame();
        if (inIntro) {
            RenderIntro(exit);
            // The intro covers the loader's title buttons, so they are drawn again on top.
            RenderTitleButtons();
        }
        UI::g_UI.RenderPopups();
        UI::g_UI.EndDraw();
        m_pSwapChain->Present(1, 0);

        // Once Roblox is found and setup finishes, launch on its own after a short beat showing the ready state.
        if (m_introDone && IsReady()) {
            m_readyTime += UI::g_UI.DeltaTime();
            if (m_readyTime >= kAutoLaunchDelay) m_launchRequested = true;
        }

        if (m_launchRequested && IsReady()) {
            m_launched = true;
            m_running = false;
        }
        m_launchRequested = false;
    }

    void ExternalLoader::Shutdown() {
        UI::g_UI.Shutdown();
        CleanupRenderTarget();
        if (m_pSwapChain) { m_pSwapChain->Release(); m_pSwapChain = nullptr; }
        if (m_pd3dDeviceContext) { m_pd3dDeviceContext->Release(); m_pd3dDeviceContext = nullptr; }
        if (m_pd3dDevice) { m_pd3dDevice->Release(); m_pd3dDevice = nullptr; }
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }

} // namespace Loader
