#pragma once

#include <cstdint>
#include <vector>

// The game side: finds the engine's script-facing objects and functions by
// signature, verifies them through MSVC RTTI, and wraps the handful of calls the
// mod needs. Nothing in here is an absolute address - everything is located in
// the running image, so a Steam CEG per-user build or a relocation changes
// nothing.
namespace f3cg::game {

// Scans the exe and then waits (polling) for the game's static script objects to
// be constructed. Called once on the mod thread; true when every call below is
// usable.
bool discover();
bool ready();           // signatures resolved; calls are safe (they no-op without the managers)
bool context_ready();   // both managers have been found (a level is loaded)
// Mod-thread loop that finds (and re-finds) the live manager objects. Never returns.
void instance_loop();

// Only valid after ready(), and meant for the game's main thread (see
// dispatch.cpp) - these call into live engine objects.
bool pause_menu_showing();
bool level_started(int player);
bool achieved(int player, unsigned challenge);
void award(int player, unsigned challenge, int count);

// FearScoreGlobalDataComponent* (the challenge definitions) and how many
// challenges it holds. Null/0 until the score manager exists.
void* score_global_data();
unsigned num_challenges();

// One challenge definition as the game holds it (a 156-byte "BoostChallengeInfo"
// record) plus its localised name, and the challenge categories.
struct ChallengeDef {
  unsigned index = 0;
  int text_id = -1;            // boostName text id
  int points = 0;              // pointValue (16-bit in the record)
  int needed = 1;              // requirementNumber (+0x14), at least 1
  int req_type = 0;            // the requirement kind id (+0x94)
  bool repeatable = false;     // isRepeatable
  bool reset_on_load = false;  // resetOnLoad
  int category = -1;           // index into the category table, -1 if unmatched
  char internal[48] = {};      // internalName, e.g. "HardBoiled"
  char name[96] = {};          // localised boostName ("Hard Boiled"), UTF-8
  char description[256] = {};  // localised requirement text ("Kill 15 enemies with any melee attack")
  char category_internal[48] = {};  // boostCategoryInternalName ("Aggressive")
  char icon_internal[48] = {};      // boostIconInternalName ("Aggressive Kills")
  int raw[5] = {};             // the record's dwords at +0x0c..+0x1c, for the log
};
struct CategoryDef {
  int text_id = -1;
  char internal[48] = {};      // "Aggressive", "Tactical", "Special", "Exploration"
  char name[64] = {};          // localised, UTF-8
};
// Reads the whole table from the live game object. False (and a log line) when
// the layout does not verify, or the score manager is not there yet.
bool read_challenges(std::vector<ChallengeDef>& out, std::vector<CategoryDef>& cats);

// Trace = 1: vtable hooks on the score manager's award/achieved slots that log
// the game's own calls (thread, arguments) and pass straight through.
void install_trace_hooks();
void remove_trace_hooks();

uintptr_t exe_base();
// Short human-readable status for the panel/log ("ready", or what is missing).
const char* status_text();

}  // namespace f3cg::game
