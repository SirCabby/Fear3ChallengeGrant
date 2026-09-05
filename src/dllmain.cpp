// Fear3ChallengeGrant - proxy entry point.
//
// We ship as binkw32.dll (see bink_proxy.cpp for why). Because the exe imports
// us statically, DllMain runs before the game's own entry point - which is what
// lets the import-table hooks below (GetProcAddress for the renderer capture,
// PeekMessageA for the main-thread tick) be in place before any game code runs.
// Everything slow or that needs the game to be initialised happens on a thread.

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "bink_proxy.h"
#include "config.h"
#include "crash.h"
#include "dispatch.h"
#include "game.h"
#include "log.h"
#include "overlay.h"
#include "version.h"

namespace {

HMODULE g_self = nullptr;
char g_dir[MAX_PATH] = {};

// Directory this DLL was loaded from, with a trailing separator.
void resolve_own_dir() {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(g_self, path, MAX_PATH);
  char* slash = std::strrchr(path, '\\');
  if (!slash) slash = std::strrchr(path, '/');
  if (slash) {
    size_t len = static_cast<size_t>(slash - path) + 1;
    if (len < sizeof(g_dir)) {
      std::memcpy(g_dir, path, len);
      g_dir[len] = '\0';
    }
  }
}

// Off the loader lock: the game's static objects do not exist yet when DllMain
// runs, so discovery polls for them.
DWORD WINAPI mod_thread(LPVOID) {
  f3cg::logf("mod thread started (thread %lu)", GetCurrentThreadId());
  if (!f3cg::config::get().disable_game) {
    if (f3cg::game::discover()) f3cg::game::instance_loop();  // never returns
  } else {
    f3cg::logf("game hooks disabled by config");
  }
  return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH: {
      g_self = module;
      DisableThreadLibraryCalls(module);
      resolve_own_dir();
      f3cg::log_init(g_dir);
      f3cg::logf("Fear3ChallengeGrant " F3CG_VERSION " loaded (binkw32.dll proxy), dir=%s pid=%lu",
                 g_dir, GetCurrentProcessId());

      if (!f3cg::bink::load_original(g_dir)) {
        MessageBoxA(nullptr,
                    "Fear3ChallengeGrant: binkw32_orig.dll is missing or broken.\n\n"
                    "The mod ships as binkw32.dll and needs the game's original Bink DLL "
                    "beside it, renamed to binkw32_orig.dll. See INSTALL.txt / README.md, "
                    "or verify the game files in Steam and reinstall the mod.",
                    "Fear3ChallengeGrant", MB_OK | MB_ICONERROR);
        return FALSE;
      }

      f3cg::install_crash_logger();
      f3cg::config::load(g_dir);
      if (f3cg::config::get().trace) f3cg::install_purecall_logger();

      // Import-table patches only - no LoadLibrary under the loader lock.
      if (!f3cg::config::get().disable_overlay) f3cg::overlay::install();
      if (!f3cg::config::get().disable_dispatch) f3cg::dispatch::install();

      if (HANDLE t = CreateThread(nullptr, 0, mod_thread, nullptr, 0, nullptr)) CloseHandle(t);
      break;
    }
    case DLL_PROCESS_DETACH:
      // Undo every patch before this image goes away: anything still pointing
      // into the DLL would fault the moment the game touched it during its own
      // teardown.
      f3cg::logf("unloading - removing hooks");
      f3cg::dispatch::uninstall();
      f3cg::game::remove_trace_hooks();
      f3cg::overlay::uninstall();
      f3cg::remove_purecall_logger();
      f3cg::logf("hooks removed cleanly");
      f3cg::log_shutdown();
      break;
    default:
      break;
  }
  return TRUE;
}
