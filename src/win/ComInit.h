#pragma once

#include <windows.h>

namespace screencap::win {

enum class ComApartment {
    MTA,
    STA,
};

class ComInit final {
public:
    explicit ComInit(ComApartment apt);
    ~ComInit();

    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;
    ComInit(ComInit&&) = delete;
    ComInit& operator=(ComInit&&) = delete;

    [[nodiscard]] HRESULT hr() const noexcept { return hr_; }

private:
    HRESULT hr_{};
    bool did_init_{};
};

} // namespace screencap::win
