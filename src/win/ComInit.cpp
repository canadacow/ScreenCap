#include "ComInit.h"

#include <objbase.h>

namespace screencap::win {

ComInit::ComInit(ComApartment apt)
{
    const DWORD coinit = (apt == ComApartment::STA) ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED;
    hr_ = ::CoInitializeEx(nullptr, coinit);
    did_init_ = (hr_ == S_OK || hr_ == S_FALSE);
}

ComInit::~ComInit()
{
    if (did_init_) {
        ::CoUninitialize();
    }
}

} // namespace screencap::win
