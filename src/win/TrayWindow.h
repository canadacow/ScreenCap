#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <optional>

#include "TrayIcon.h"
#include "capture/DesktopDuplicator.h"

struct ID3D11Device;

namespace screencap::win {

class TrayWindow final {
public:
    TrayWindow();
    ~TrayWindow();

    TrayWindow(const TrayWindow&) = delete;
    TrayWindow& operator=(const TrayWindow&) = delete;
    TrayWindow(TrayWindow&&) = delete;
    TrayWindow& operator=(TrayWindow&&) = delete;

    [[nodiscard]] int Run();

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    [[nodiscard]] bool InitD3D11();
    [[nodiscard]] bool CreateHiddenWindow();
    [[nodiscard]] bool CreateMenu();
    void EnsureTrayIcon();
    void ShowContextMenu();
    void OnCommand(UINT cmd);
    void NotifyResult(bool saved);

    // Capture with automatic re-init on stale duplications.
    [[nodiscard]] std::optional<capture::FrameData> CaptureDesktop();

    // Low-level keyboard hook to intercept PrtScn before Windows/Snipping Tool.
    void InstallKeyboardHook();
    void RemoveKeyboardHook();

    // Persisted settings via HKCU\Software\ScreenCap.
    void LoadSettings();
    void SaveSettings();

    HWND hwnd_{};
    HMENU menu_{};
    UINT taskbarCreatedMsg_{};
    std::optional<TrayIcon> icon_{};
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    capture::DesktopDuplicator duplicator_;
    bool copyToClipboard_{false};
};

} // namespace screencap::win
