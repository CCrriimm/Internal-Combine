#pragma once

// Supplied NEW SDK/bs_bypass entry points. The standalone DllMain is removed;
// CometInternal owns module lifetime and calls these from its worker threads.
extern "C" void bypass_init();
extern "C" void bypass_shutdown();

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "psapi.lib")
