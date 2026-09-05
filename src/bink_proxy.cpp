// The mod ships as binkw32.dll, standing in front of the game's own Bink video
// DLL (renamed binkw32_orig.dll by `make install`). F.E.A.R. 3 imports 24
// functions from it, all stdcall. Rather than declare each one, every export
// is a one-instruction thunk that jumps through a table of the real addresses -
// the stack is untouched, so the callee cleans up exactly as before.
//
// Why binkw32 and not d3d9/dinput8/version: those are Wine builtins, so
// proxying them needs a WINEDLLOVERRIDES launch option under Proton. binkw32
// ships with the game, so our file simply wins the application-directory search
// on Windows and Wine alike - the mod is a pure drop-in on both.

#include "bink_proxy.h"

#include <windows.h>

#include <cstdio>

#include "log.h"

#define BINK_EXPORTS(X)                    \
  X(0, BinkClose, 4)                       \
  X(1, BinkControlBackgroundIO, 8)         \
  X(2, BinkDoFramePlane, 8)                \
  X(3, BinkFreeGlobals, 0)                 \
  X(4, BinkGetError, 0)                    \
  X(5, BinkGetFrameBuffersInfo, 8)         \
  X(6, BinkGetRealtime, 12)                \
  X(7, BinkGetSummary, 8)                  \
  X(8, BinkGoto, 12)                       \
  X(9, BinkNextFrame, 4)                   \
  X(10, BinkOpenDirectSound, 4)            \
  X(11, BinkOpenWaveOut, 4)                \
  X(12, BinkOpenWithOptions, 12)           \
  X(13, BinkOpenXAudio2, 4)                \
  X(14, BinkPause, 8)                      \
  X(15, BinkRegisterFrameBuffers, 8)       \
  X(16, BinkSetMemory, 8)                  \
  X(17, BinkSetSoundOnOff, 8)              \
  X(18, BinkSetSoundSystem, 8)             \
  X(19, BinkSetSpeakerVolumes, 20)         \
  X(20, BinkSetVolume, 12)                 \
  X(21, BinkSetWillLoop, 8)                \
  X(22, BinkShouldSkip, 4)                 \
  X(23, BinkWait, 4)

constexpr int kBinkExportCount = 24;

// Plain C symbol so the assembly below can name it as _g_bink_orig.
extern "C" {
void* g_bink_orig[kBinkExportCount] = {};
}

// One thunk per export: `jmp [g_bink_orig + i*4]`. Defined at file scope in
// assembly so no prologue/epilogue is ever emitted around the jump.
#define BINK_THUNK(i, name, argbytes)                      \
  asm(".text\n"                                            \
      ".globl _bink_" #name "\n"                           \
      "_bink_" #name ":\n"                                 \
      "\tjmp *_g_bink_orig+" #i "*4\n");
BINK_EXPORTS(BINK_THUNK)
#undef BINK_THUNK

namespace f3cg::bink {

bool load_original(const char* dir) {
  char path[MAX_PATH] = {};
  std::snprintf(path, sizeof(path), "%sbinkw32_orig.dll", dir);

  HMODULE orig = LoadLibraryA(path);
  if (!orig) {
    logf("FATAL: could not load %s (GetLastError=%lu). Did `make install` run?", path,
         GetLastError());
    return false;
  }
  // Pin it: nothing may drop the last reference while the game still uses it.
  HMODULE pinned = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, path, &pinned);

  struct Export {
    int index;
    const char* name;
  };
#define BINK_NAME(i, name, argbytes) {i, "_" #name "@" #argbytes},
  static const Export kExports[] = {BINK_EXPORTS(BINK_NAME)};
#undef BINK_NAME

  int missing = 0;
  for (const Export& e : kExports) {
    void* fn = reinterpret_cast<void*>(GetProcAddress(orig, e.name));
    g_bink_orig[e.index] = fn;
    if (!fn) {
      ++missing;
      logf("FATAL: %s lacks export %s", path, e.name);
    }
  }
  if (missing) return false;
  logf("forwarding %d Bink exports -> %s", kBinkExportCount, path);
  return true;
}

}  // namespace f3cg::bink
