#include "dispatch.h"

#include <windows.h>

#include "challenges.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include "mem.h"

namespace f3cg::dispatch {
namespace {

using PeekMessageAFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
PeekMessageAFn g_orig_peek = nullptr;
DWORD g_main_thread = 0;  // the process's primary thread: DllMain(PROCESS_ATTACH) runs on it

Snapshot g_snap;
volatile LONG g_refresh_requested = 0;
bool g_paused_prev = false;
bool g_in_level_prev = false;
DWORD g_last_tick = 0;
DWORD g_last_refresh = 0;
DWORD g_flush_at = 0;
bool g_tick_logged = false;

void flush_queue(int player) {
  unsigned queue[256];
  const int n = challenges::take_queue(queue, 256);
  if (!n) return;
  logf("granting %d queued challenge(s) to player %d", n, player);
  for (int i = 0; i < n; ++i) {
    const unsigned idx = queue[i];
    if (game::achieved(player, idx)) {
      logf("  [%02u] already achieved - skipped", idx);
      continue;
    }
    const challenges::Info info = challenges::info(static_cast<int>(idx));
    const int count = info.needed > 0 ? info.needed : 1;
    game::award(player, idx, count);
    const bool ok = game::achieved(player, idx);
    logf("  [%02u] %-28s award(count=%d) -> %s", idx, info.display, count,
         ok ? "achieved" : "NOT achieved (the game did not accept it)");
  }
  challenges::refresh_statuses(player);
}

void tick() {
  const DWORD now = GetTickCount();
  if (now - g_last_tick < 16) return;  // the pump spins many times per frame
  g_last_tick = now;

  Snapshot s;
  s.main_thread = GetCurrentThreadId();
  s.ticks = g_snap.ticks + 1;
  s.game_ready = game::ready();
  if (!g_tick_logged) {
    g_tick_logged = true;
    logf("main-thread tick running on thread %lu", s.main_thread);
  }
  if (!s.game_ready) {
    g_snap = s;
    return;
  }
  if (config::get().trace) game::install_trace_hooks();

  const int player = config::get().player_index;
  s.context_ready = game::context_ready();
  s.table_loaded = challenges::ensure_loaded();
  s.in_level = game::level_started(player);
  s.paused = s.in_level && game::pause_menu_showing();

  if (s.in_level != g_in_level_prev) {
    logf("level %s (player %d)", s.in_level ? "started" : "ended", player);
    g_in_level_prev = s.in_level;
    if (!s.in_level) {
      if (challenges::queued_count()) logf("level ended - dropping the pending queue");
      challenges::clear_queue();
      g_flush_at = 0;
    }
  }

  if (s.table_loaded && s.paused && !g_paused_prev) {
    logf("pause menu opened - refreshing challenge statuses");
    challenges::refresh_statuses(player);
    g_last_refresh = now;
  }
  if (s.table_loaded && !s.paused && g_paused_prev && s.in_level) {
    const int queued = challenges::queued_count();
    if (queued) {
      g_flush_at = now + static_cast<DWORD>(config::get().grant_delay_ms);
      if (!g_flush_at) g_flush_at = 1;
      logf("unpaused with %d queued - granting in %d ms", queued, config::get().grant_delay_ms);
    }
  }
  g_paused_prev = s.paused;

  if (g_flush_at && static_cast<LONG>(now - g_flush_at) >= 0) {
    g_flush_at = 0;
    if (s.in_level && !s.paused) flush_queue(player);
    else logf("grant window missed (in_level=%d paused=%d) - queue kept", s.in_level, s.paused);
  }

  if (s.table_loaded && s.paused) {
    const bool wanted = InterlockedExchange(&g_refresh_requested, 0) != 0;
    if (wanted || now - g_last_refresh > 2000) {
      challenges::refresh_statuses(player);
      g_last_refresh = now;
    }
  }

  s.pending_ms = g_flush_at ? static_cast<int>(g_flush_at - now) : -1;
  s.show_panel = s.table_loaded && s.paused && s.in_level;
  g_snap = s;
}

BOOL WINAPI hk_peek_message(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove) {
  // Other threads pump messages too (the Steam overlay, worker windows); the
  // game runs on the primary thread, and so must everything that calls into it.
  if (GetCurrentThreadId() == g_main_thread) tick();
  return g_orig_peek(msg, hwnd, min, max, remove);
}

}  // namespace

bool install() {
  g_main_thread = GetCurrentThreadId();
  void* prev = mem::iat_hook(GetModuleHandleA(nullptr), "USER32.dll", "PeekMessageA",
                             reinterpret_cast<void*>(&hk_peek_message));
  if (!prev) {
    logf("ERROR: could not hook PeekMessageA in the exe's import table - no main-thread tick");
    return false;
  }
  g_orig_peek = reinterpret_cast<PeekMessageAFn>(prev);
  logf("dispatch: PeekMessageA import hooked (original %p)", prev);
  return true;
}

void uninstall() {
  if (!g_orig_peek) return;
  mem::iat_hook(GetModuleHandleA(nullptr), "USER32.dll", "PeekMessageA",
                reinterpret_cast<void*>(g_orig_peek));
  g_orig_peek = nullptr;
}

Snapshot snapshot() { return g_snap; }

void request_refresh() { InterlockedExchange(&g_refresh_requested, 1); }

}  // namespace f3cg::dispatch
