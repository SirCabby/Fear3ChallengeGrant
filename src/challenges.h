#pragma once

#include <cstdint>

// The challenge table and the grant queue. All state here is shared between
// the game's main thread (dispatch.cpp, which talks to the game) and the render
// thread (overlay.cpp, which draws it), so every accessor takes the lock.
namespace f3cg::challenges {

struct Info {
  unsigned index = 0;      // the game's challenge index (what the award call takes)
  int text_id = -1;        // localisation text id of the name
  int points = 0;          // score awarded
  int needed = 1;          // requirement occurrences needed to complete it
  int category = -1;       // index into the category table, -1 if unknown
  bool repeatable = false;
  char internal[48] = {};  // e.g. "HardBoiled"
  char display[96] = {};   // the game's localised name, or a prettified internal name
  char description[256] = {};  // the game's localised requirement text
};

enum class Status : uint8_t {
  kUnknown = 0,  // not queried yet
  kAvailable,    // not achieved this mission
  kQueued,       // clicked; granted on unpause
  kAwarded,      // the game says it has been achieved this mission
};

// Main thread: build the table from the game once the score manager exists.
// Cheap to call repeatedly; returns true when the table is loaded.
bool ensure_loaded();
bool loaded();
int count();
Info info(int i);
Status status(int i);
int num_categories();
const char* category_name(int c);

// Panel side.
// Available -> queued (or back). True if the row changed; awarded rows never
// do, so queueing a whole list is safe against a status refresh in between.
bool set_queued(int i, bool queued);
void clear_queue();
int queued_count();
int awarded_count();

// Main thread.
void refresh_statuses(int player);
// Move every queued index into `out` (at most `max`), returning how many.
int take_queue(unsigned* out, int max);

}  // namespace f3cg::challenges
