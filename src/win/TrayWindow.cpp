#include "TrayWindow.h"

#include "capture/SaveImage.h"
#include "preview/PreviewWindow.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

#pragma comment(lib, "propsys.lib")

namespace screencap::win {
namespace {

// AUMID used for toast notifications (must match wWinMain.cpp).
constexpr wchar_t kAppId[] = L"ScreenCap";

// ── Start-menu shortcut (required for toast notifications) ──────────
//
// Unpackaged Win32 apps cannot show WinRT toasts unless a .lnk with
// System.AppUserModel.ID exists in the Start Menu Programs folder.
// We create it once, silently, on first run.

void EnsureStartMenuShortcut()
{
    wchar_t appData[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) == 0) return;

    std::wstring lnkPath = std::wstring(appData)
        + L"\\Microsoft\\Windows\\Start Menu\\Programs\\ScreenCap.lnk";

    // Already exists — nothing to do.
    if (::GetFileAttributesW(lnkPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    wchar_t exePath[MAX_PATH]{};
    if (::GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return;

    Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
    HRESULT hr = ::CoCreateInstance(
        CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink));
    if (FAILED(hr)) return;

    hr = shellLink->SetPath(exePath);
    if (FAILED(hr)) return;

    // Set System.AppUserModel.ID on the shortcut.
    Microsoft::WRL::ComPtr<IPropertyStore> propStore;
    hr = shellLink.As(&propStore);
    if (FAILED(hr)) return;

    PROPVARIANT pv{};
    hr = ::InitPropVariantFromString(kAppId, &pv);
    if (FAILED(hr)) return;

    hr = propStore->SetValue(PKEY_AppUserModel_ID, pv);
    ::PropVariantClear(&pv);
    if (FAILED(hr)) return;

    hr = propStore->Commit();
    if (FAILED(hr)) return;

    Microsoft::WRL::ComPtr<IPersistFile> persistFile;
    hr = shellLink.As(&persistFile);
    if (FAILED(hr)) return;

    (void)persistFile->Save(lnkPath.c_str(), TRUE);
}

constexpr UINT kTrayCallbackMsg = WM_APP + 100;
constexpr UINT kTrayIconId = 1;

enum class MenuId : UINT {
    CaptureRegion = 1001,
    CaptureWindow = 1002,
    CaptureFullDesktop = 1003,
    CopyToClipboard = 1010,
    Exit = 1099,
};

// Custom message posted by the low-level keyboard hook.
constexpr UINT kHookCaptureMsg = WM_APP + 200;

// LL keyboard hook state (must be file-scoped for the callback).
HWND  g_hookTargetHwnd = nullptr;
HHOOK g_keyboardHook   = nullptr;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION &&
        (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (kb->vkCode == VK_SNAPSHOT && g_hookTargetHwnd) {
            // Determine which capture mode based on modifier keys.
            const bool alt  = (::GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
            const bool ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

            UINT cmd{};
            if (ctrl)     cmd = static_cast<UINT>(MenuId::CaptureFullDesktop);
            else if (alt) cmd = static_cast<UINT>(MenuId::CaptureWindow);
            else          cmd = static_cast<UINT>(MenuId::CaptureRegion);

            ::PostMessageW(g_hookTargetHwnd, kHookCaptureMsg, cmd, 0);
            return 1; // Swallow the key — prevent Windows/Snipping Tool from handling it.
        }
    }
    return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
}

constexpr const wchar_t* kRegKey = L"Software\\ScreenCap";
constexpr const wchar_t* kRegValueClipboard = L"CopyToClipboard";

} // namespace

TrayWindow::TrayWindow()
{
    taskbarCreatedMsg_ = ::RegisterWindowMessageW(L"TaskbarCreated");
    LoadSettings();
}

TrayWindow::~TrayWindow()
{
    icon_.reset();

    if (menu_) {
        ::DestroyMenu(menu_);
        menu_ = nullptr;
    }

    if (hwnd_ && ::IsWindow(hwnd_)) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool TrayWindow::InitD3D11()
{
    const D3D_FEATURE_LEVEL requestedLevel = D3D_FEATURE_LEVEL_12_1;
    HRESULT hr = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        &requestedLevel, 1,
        D3D11_SDK_VERSION,
        &d3dDevice_, nullptr, nullptr);
    return SUCCEEDED(hr);
}

int TrayWindow::Run()
{
    EnsureStartMenuShortcut();

    if (!CreateHiddenWindow() || !CreateMenu()) {
        ::MessageBoxW(nullptr, L"Failed to initialize tray window/menu.", L"ScreenCap", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!InitD3D11()) {
        ::MessageBoxW(nullptr, L"Failed to initialize Direct3D.", L"ScreenCap", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!duplicator_.Init(d3dDevice_.Get())) {
        ::MessageBoxW(nullptr, L"Failed to initialize desktop capture.", L"ScreenCap", MB_OK | MB_ICONERROR);
        return 1;
    }

    EnsureTrayIcon();
    InstallKeyboardHook();

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    RemoveKeyboardHook();
    return static_cast<int>(msg.wParam);
}

bool TrayWindow::CreateHiddenWindow()
{
    const auto hinst = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayWindow::WndProcThunk;
    wc.hInstance = hinst;
    wc.lpszClassName = L"ScreenCap.TrayWindow";

    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // Real top-level window (never shown) so SetForegroundWindow works.
    hwnd_ = ::CreateWindowExW(
        0,
        wc.lpszClassName,
        L"ScreenCap",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr,
        nullptr,
        hinst,
        this);

    return hwnd_ != nullptr;
}

bool TrayWindow::CreateMenu()
{
    menu_ = ::CreatePopupMenu();
    if (!menu_) {
        return false;
    }

    (void)::AppendMenuW(menu_, MF_STRING, static_cast<UINT_PTR>(MenuId::CaptureRegion), L"Capture Region...\tPrtScn");
    (void)::AppendMenuW(menu_, MF_STRING, static_cast<UINT_PTR>(MenuId::CaptureWindow), L"Capture Window...\tAlt+PrtScn");
    (void)::AppendMenuW(menu_, MF_STRING, static_cast<UINT_PTR>(MenuId::CaptureFullDesktop), L"Capture Full Desktop...\tCtrl+PrtScn");
    (void)::AppendMenuW(menu_, MF_SEPARATOR, 0, nullptr);
    (void)::AppendMenuW(menu_, MF_STRING | (copyToClipboard_ ? MF_CHECKED : MF_UNCHECKED),
                         static_cast<UINT_PTR>(MenuId::CopyToClipboard), L"Copy to Clipboard");
    (void)::AppendMenuW(menu_, MF_SEPARATOR, 0, nullptr);
    (void)::AppendMenuW(menu_, MF_STRING, static_cast<UINT_PTR>(MenuId::Exit), L"Exit");

    return true;
}

// ── Window procedure ────────────────────────────────────────────────

LRESULT CALLBACK TrayWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<TrayWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    auto* self = reinterpret_cast<TrayWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    return self->WndProc(hwnd, msg, wparam, lparam);
}

LRESULT TrayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    // Explorer re-created the taskbar (e.g. after crash) — re-add icon.
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_ != 0) {
        icon_.reset();
        EnsureTrayIcon();
        return 0;
    }

    switch (msg) {
    case WM_DESTROY:
        icon_.reset();
        if (menu_) {
            ::DestroyMenu(menu_);
            menu_ = nullptr;
        }
        ::PostQuitMessage(0);
        return 0;

    case kHookCaptureMsg:
        OnCommand(static_cast<UINT>(wparam));
        return 0;

    case kTrayCallbackMsg:
        // Classic callback: lParam is the mouse message directly.
        if (lparam == WM_RBUTTONUP) {
            ShowContextMenu();
        }
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ── Icon management ─────────────────────────────────────────────────

void TrayWindow::EnsureTrayIcon()
{
    if (!hwnd_) {
        return;
    }

    if (!icon_) {
        icon_.emplace(hwnd_, kTrayCallbackMsg, kTrayIconId);
    }

    if (!icon_->IsAdded()) {
        // Load the embedded app icon at the system's small-icon size (typically 16 or 20px).
        const auto hicon = static_cast<HICON>(::LoadImageW(
            ::GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(1),
            IMAGE_ICON,
            ::GetSystemMetrics(SM_CXSMICON),
            ::GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR));
        (void)icon_->Add(hicon ? hicon : ::LoadIconW(nullptr, IDI_APPLICATION), L"ScreenCap");
    }
}

// ── Context menu ────────────────────────────────────────────────────

void TrayWindow::ShowContextMenu()
{
    if (!menu_ || !hwnd_) {
        return;
    }

    POINT pt{};
    ::GetCursorPos(&pt);

    // Required so the menu dismisses when the user clicks elsewhere.
    ::SetForegroundWindow(hwnd_);

    const auto cmd = ::TrackPopupMenuEx(
        menu_,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN,
        pt.x,
        pt.y,
        hwnd_,
        nullptr);

    // Pair with SetForegroundWindow per MS docs (KB135788).
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);

    if (cmd != 0) {
        OnCommand(static_cast<UINT>(cmd));
    }
}

void TrayWindow::NotifyResult(bool saved)
{
    if (!saved) return;

    try {
        namespace notif = winrt::Windows::UI::Notifications;
        namespace xml   = winrt::Windows::Data::Xml::Dom;

        const wchar_t* message = copyToClipboard_
            ? L"Image copied to clipboard."
            : L"Image saved to file.";

        // Build the image file URI (backslashes → forward slashes).
        auto thumbPath = capture::GetThumbnailTempPath();
        std::wstring imageUri = L"file:///";
        for (wchar_t c : thumbPath) {
            imageUri += (c == L'\\') ? L'/' : c;
        }

        std::wstring toastXml =
            L"<toast><visual><binding template=\"ToastGeneric\">"
            L"<text>ScreenCap</text>"
            L"<text>" + std::wstring(message) + L"</text>"
            L"<image src=\"" + imageUri + L"\"/>"
            L"</binding></visual></toast>";

        xml::XmlDocument doc;
        doc.LoadXml(toastXml);

        notif::ToastNotification toast{doc};
        auto notifier = notif::ToastNotificationManager::CreateToastNotifier(kAppId);
        notifier.Show(toast);
    } catch (...) {
        // Toast unavailable — silently ignore.
    }
}

std::optional<capture::FrameData> TrayWindow::CaptureDesktop()
{
    auto frame = duplicator_.CaptureFullDesktop();
    if (frame) return frame;

    // Capture failed — the output duplications are likely stale
    // (desktop format change, resolution change, monitor hotplug, etc.).
    // Re-initialise and retry once.  DwmFlush() forces a composition
    // so the new duplication has a real frame ready to acquire.
    if (duplicator_.Init(d3dDevice_.Get())) {
        ::DwmFlush();
        frame = duplicator_.CaptureFullDesktop();
        if (frame) return frame;
    }

    return std::nullopt;
}

void TrayWindow::InstallKeyboardHook()
{
    if (g_keyboardHook) return;
    g_hookTargetHwnd = hwnd_;
    g_keyboardHook = ::SetWindowsHookExW(
        WH_KEYBOARD_LL,
        LowLevelKeyboardProc,
        ::GetModuleHandleW(nullptr),
        0);
}

void TrayWindow::RemoveKeyboardHook()
{
    if (g_keyboardHook) {
        ::UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    g_hookTargetHwnd = nullptr;
}

void TrayWindow::OnCommand(UINT cmd)
{
    switch (static_cast<MenuId>(cmd)) {
    case MenuId::CaptureRegion:
    case MenuId::CaptureWindow:
    case MenuId::CaptureFullDesktop: {
        auto frame = CaptureDesktop();
        if (!frame) {
            ::MessageBoxW(nullptr, L"Desktop capture failed.", L"ScreenCap", MB_OK | MB_ICONERROR);
            break;
        }
        bool ok = false;
        switch (static_cast<MenuId>(cmd)) {
        case MenuId::CaptureRegion:
            ok = preview::ShowRegion(std::move(*frame), d3dDevice_.Get(), copyToClipboard_);
            break;
        case MenuId::CaptureWindow:
            ok = preview::ShowWindowCapture(std::move(*frame), d3dDevice_.Get(), copyToClipboard_);
            break;
        case MenuId::CaptureFullDesktop:
            ok = preview::Show(std::move(*frame), d3dDevice_.Get(), copyToClipboard_);
            break;
        default:
            break;
        }
        NotifyResult(ok);
        break;
    }
    case MenuId::CopyToClipboard:
        copyToClipboard_ = !copyToClipboard_;
        ::CheckMenuItem(menu_, static_cast<UINT>(MenuId::CopyToClipboard),
                        MF_BYCOMMAND | (copyToClipboard_ ? MF_CHECKED : MF_UNCHECKED));
        SaveSettings();
        break;
    case MenuId::Exit:
        ::DestroyWindow(hwnd_);
        break;
    default:
        break;
    }
}

// ── Persisted settings ──────────────────────────────────────────────

void TrayWindow::LoadSettings()
{
    HKEY key{};
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return; // Key doesn't exist yet — use defaults.
    }

    DWORD val = 0;
    DWORD size = sizeof(val);
    DWORD type = 0;
    if (::RegQueryValueExW(key, kRegValueClipboard, nullptr, &type,
                           reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        copyToClipboard_ = (val != 0);
    }

    ::RegCloseKey(key);
}

void TrayWindow::SaveSettings()
{
    HKEY key{};
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr,
                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                          &key, nullptr) != ERROR_SUCCESS) {
        return;
    }

    const DWORD val = copyToClipboard_ ? 1 : 0;
    (void)::RegSetValueExW(key, kRegValueClipboard, 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&val), sizeof(val));

    ::RegCloseKey(key);
}

} // namespace screencap::win
