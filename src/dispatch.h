#pragma once

// The main-thread tick. The game's own script calls into the score manager run
// on the thread that pumps window messages, so the mod's calls do too: an
// import-table hook on PeekMessageA gives a callback on that thread once per
// pump, and everything that talks to the game happens from there.
namespace f3cg::dispatch {

bool install();  // from DllMain (import-table patch only); records the primary thread
void uninstall();

struct Snapshot {
  bool game_ready = false;
  bool context_ready = false;  // a level is loaded (the game context exists)
  bool table_loaded = false;
  bool paused = false;
  bool in_level = false;
  bool show_panel = false;  // the visibility rule, evaluated on the main thread
  int pending_ms = -1;      // countdown to the queued grants (-1 = none pending)
  unsigned long main_thread = 0;
  unsigned long long ticks = 0;
};
Snapshot snapshot();

// The panel asks for a fresh status pass (after queue changes).
void request_refresh();

}  // namespace f3cg::dispatch
