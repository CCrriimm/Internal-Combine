// 34cheat/src/loader.hpp
// context: C++20 / loader.hpp / MSVC / x64 / Windows 10/11 / version-5a21b87bbc2641cf
// Frameless startup window: runs the attach pipeline and hands off to the overlay.
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <future>
#include "ui_renderer.hpp"
#include "../include/updater.hpp"

namespace Loader {

    enum class StepStatus {
        Pending,
        Working,
        Completed,
        Failed
    };

    struct PipelineStep {
        std::wstring Title;
        std::wstring Detail;
        StepStatus Status{ StepStatus::Pending };
    };

    class ExternalLoader {
    private:
        HWND m_hwnd = nullptr;
        WNDCLASSEXW m_wc{};
        ID3D11Device* m_pd3dDevice = nullptr;
        ID3D11DeviceContext* m_pd3dDeviceContext = nullptr;
        IDXGISwapChain* m_pSwapChain = nullptr;
        ID3D11RenderTargetView* m_pRenderTargetView = nullptr;

        Math::Vector2 m_windowSize{ 680.0f, 420.0f };
        bool m_running = true;
        bool m_launched = false;
        bool m_launchRequested = false;
        float m_readyTime = 0.0f; // seconds spent ready, for the auto-launch

        std::vector<PipelineStep> m_steps;
        int m_currentStepIndex = 0;
        float m_progress = 0.0f;
        std::wstring m_hwidHash;
        std::wstring m_syncedVersion;
        std::future<Updater::SyncResult> m_syncTask;

        // Intro: the mark unfolds out of a square, loads, then hands over to the loader.
        float m_introTime = 0.0f;
        bool m_introDone = false;

        void InitSteps();
        void AdvanceStep();
        void CompleteStep(float progress);
        void CreateRenderTarget();
        void CleanupRenderTarget();
        void RenderFrame();
        void RenderStep(const PipelineStep& step, const Math::Vector2& pos, float width, bool last);
        void RenderStatusMark(const Math::Vector2& pos, float size, bool ready, bool failed);
        void RenderIntro(float exit);
        void RenderTitleButtons();

    public:
        static constexpr float kTitlebarHeight = 44.0f;
        static constexpr float kTitlebarButtonsWidth = 84.0f;

        ExternalLoader() = default;
        ~ExternalLoader() { Shutdown(); }

        bool Initialize(HINSTANCE hInstance);
        bool Run();
        void Tick(); // one frame: advance the pipeline, read input, draw, present
        void Shutdown();

        void OnKey(WPARAM key);
        bool IsReady() const { return m_progress >= 1.0f; }
        bool IsLaunched() const { return m_launched; }
        HWND GetHwnd() const { return m_hwnd; }
        const Math::Vector2& GetWindowSize() const { return m_windowSize; }
    };

    inline ExternalLoader g_Loader;

} // namespace Loader
