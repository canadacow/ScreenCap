#pragma once

#include <windows.h>

#include <shellapi.h>

#include <string_view>

namespace screencap::win {

class TrayIcon final {
public:
    TrayIcon(HWND hwnd, UINT callback_message, UINT icon_id) noexcept;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;
    TrayIcon(TrayIcon&&) = delete;
    TrayIcon& operator=(TrayIcon&&) = delete;

    [[nodiscard]] bool Add(HICON icon, std::wstring_view tooltip);
    void Remove() noexcept;

    [[nodiscard]] bool IsAdded() const noexcept { return added_; }

private:
    NOTIFYICONDATAW nid_{};
    bool added_{false};
};

} // namespace screencap::win

