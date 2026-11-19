#pragma once

#include "panel_protocol.h"
#include <string_view>

namespace panel {

using RequestHandler = std::string (*)(std::string_view);

// Win32 transport only. The handler always runs on the background application's window thread.
bool Start(HWND window, UINT message, RequestHandler handler);
void Complete();
void ExpireClient();
void Stop();

}
