#include "win/ComInit.h"
#include "win/TrayWindow.h"

#include <windows.h>
#include <ShObjIdl.h>

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrev*/, PWSTR /*cmdLine*/, int /*cmdShow*/)
{
    // Single-instance guard.
    const HANDLE hMutex = ::CreateMutexW(nullptr, TRUE, L"ScreenCap.SingleInstance");
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    // AUMID for WinRT toast notifications (must be set before any toast API calls).
    (void)::SetCurrentProcessExplicitAppUserModelID(L"ScreenCap");

    // Per-monitor DPI awareness so Desktop Duplication pixel dimensions
    // match window/screen metrics exactly.
    (void)::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const screencap::win::ComInit com{screencap::win::ComApartment::STA};
    if (FAILED(com.hr()) && com.hr() != RPC_E_CHANGED_MODE) {
        ::MessageBoxW(nullptr, L"COM initialization failed.", L"ScreenCap", MB_OK | MB_ICONERROR);
        return 1;
    }

    screencap::win::TrayWindow app;
    const int ret = app.Run();

    if (hMutex) ::CloseHandle(hMutex);
    return ret;
}
