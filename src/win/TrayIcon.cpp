#include "TrayIcon.h"

#include <strsafe.h>

namespace screencap::win {

TrayIcon::TrayIcon(HWND hwnd, UINT callback_message, UINT icon_id) noexcept
{
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd;
    nid_.uID = icon_id;
    nid_.uFlags = NIF_MESSAGE;
    nid_.uCallbackMessage = callback_message;
}

TrayIcon::~TrayIcon()
{
    Remove();
}

bool TrayIcon::Add(HICON icon, std::wstring_view tooltip)
{
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.hIcon = icon;

    if (!tooltip.empty()) {
        (void)::StringCchCopyNW(nid_.szTip, _countof(nid_.szTip), tooltip.data(), tooltip.size());
    } else {
        nid_.szTip[0] = L'\0';
    }

    if (!::Shell_NotifyIconW(NIM_ADD, &nid_)) {
        added_ = false;
        return false;
    }

    added_ = true;
    return true;
}

void TrayIcon::Remove() noexcept
{
    if (!added_) {
        return;
    }

    (void)::Shell_NotifyIconW(NIM_DELETE, &nid_);
    added_ = false;
}

} // namespace screencap::win
