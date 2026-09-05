#include "challenges.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "game.h"
#include "log.h"

namespace f3cg::challenges {
namespace {

struct Lock {
  static CRITICAL_SECTION& cs() {
    static CRITICAL_SECTION s = [] {
      CRITICAL_SECTION c;
      InitializeCriticalSection(&c);
      return c;
    }();
    return s;
  }
  Lock() { EnterCriticalSection(&cs()); }
  ~Lock() { LeaveCriticalSection(&cs()); }
};

std::vector<Info> g_table;
std::vector<Status> g_status;
struct CatName {
  char s[32];
};
std::vector<CatName> g_categories;
bool g_loaded = false;
DWORD g_last_attempt = 0;

// "HardBoiled" -> "Hard Boiled"; runs of capitals ("MP", "CAS") stay together.
void prettify(const char* src, char* out, size_t n) {
  size_t o = 0;
  for (size_t i = 0; src[i] && o + 2 < n; ++i) {
    const char c = src[i];
    if (c == '_') {
      out[o++] = ' ';
      continue;
    }
    const bool upper = c >= 'A' && c <= 'Z';
    const bool prev_lower = i > 0 && ((src[i - 1] >= 'a' && src[i - 1] <= 'z') ||
                                      (src[i - 1] >= '0' && src[i - 1] <= '9'));
    const bool next_lower = src[i + 1] >= 'a' && src[i + 1] <= 'z';
    const bool prev_upper = i > 0 && src[i - 1] >= 'A' && src[i - 1] <= 'Z';
    if (o > 0 && out[o - 1] != ' ' && upper && (prev_lower || (prev_upper && next_lower)))
      out[o++] = ' ';
    out[o++] = c;
  }
  out[o] = '\0';
}

// The table comes straight from the game's FearScoreGlobalDataComponent, so
// the indices are exactly what AwardChallengeRequirement accepts and the names
// are the game's own localised text. A challenge whose name the text table did
// not resolve falls back to a prettified internal name.
bool build_table_locked() {
  std::vector<game::ChallengeDef> defs;
  std::vector<game::CategoryDef> cats;
  if (!game::read_challenges(defs, cats) || defs.empty()) return false;

  g_categories.clear();
  g_categories.resize(cats.size());
  for (size_t c = 0; c < cats.size(); ++c) {
    char* out = g_categories[c].s;
    if (cats[c].name[0]) std::snprintf(out, sizeof(g_categories[c].s), "%s", cats[c].name);
    else prettify(cats[c].internal, out, sizeof(g_categories[c].s));
    // The game's labels are column headers ("Aggression:"); drop the trailing colon.
    for (size_t e = std::strlen(out); e > 0 && (out[e - 1] == ':' || out[e - 1] == ' '); --e) out[e - 1] = '\0';
  }

  g_table.clear();
  g_status.clear();
  for (const game::ChallengeDef& d : defs) {
    Info c;
    c.index = d.index;
    c.text_id = d.text_id;
    c.points = d.points;
    c.needed = d.needed > 0 ? d.needed : 1;  // requirementNumber (+0x14)
    c.category = d.category;
    c.repeatable = d.repeatable;
    std::snprintf(c.internal, sizeof(c.internal), "%s", d.internal[0] ? d.internal : "?");
    if (d.name[0]) std::snprintf(c.display, sizeof(c.display), "%s", d.name);
    else prettify(c.internal, c.display, sizeof(c.display));
    std::snprintf(c.description, sizeof(c.description), "%s", d.description);
    g_table.push_back(c);
  }
  g_status.assign(g_table.size(), Status::kUnknown);
  g_loaded = true;

  unsigned described = 0;
  for (const game::ChallengeDef& d : defs) described += d.description[0] != 0;
  logf("challenge table: %u challenges (%u with descriptions), %u categories", static_cast<unsigned>(defs.size()),
       described, static_cast<unsigned>(cats.size()));
  for (size_t c = 0; c < cats.size(); ++c)
    logf("  category[%u] textid=%d internal=%-20s name=%s", static_cast<unsigned>(c), cats[c].text_id,
         cats[c].internal, cats[c].name);
  for (const game::ChallengeDef& d : defs)
    logf("  [%02u] %-26s %-30s pts=%-5d need=%-3d type=%-2d cat=%d(%s) rep=%d rol=%d textid=%d raw=%d,%d,%d,%d,%d icon=%s",
         d.index, d.internal, d.name, d.points, d.needed, d.req_type, d.category, d.category_internal,
         d.repeatable, d.reset_on_load, d.text_id, d.raw[0], d.raw[1], d.raw[2], d.raw[3], d.raw[4], d.icon_internal);
  return true;
}

}  // namespace

bool ensure_loaded() {
  Lock l;
  if (g_loaded) return true;
  const DWORD now = GetTickCount();
  if (now - g_last_attempt < 1000) return false;
  g_last_attempt = now;
  return build_table_locked();
}

bool loaded() {
  Lock l;
  return g_loaded;
}

int count() {
  Lock l;
  return static_cast<int>(g_table.size());
}

Info info(int i) {
  Lock l;
  return (i >= 0 && i < static_cast<int>(g_table.size())) ? g_table[i] : Info{};
}

Status status(int i) {
  Lock l;
  return (i >= 0 && i < static_cast<int>(g_status.size())) ? g_status[i] : Status::kUnknown;
}

int num_categories() {
  Lock l;
  return static_cast<int>(g_categories.size());
}

const char* category_name(int c) {
  Lock l;
  return (c >= 0 && c < static_cast<int>(g_categories.size())) ? g_categories[c].s : "";
}

bool set_queued(int i, bool queued) {
  Lock l;
  if (i < 0 || i >= static_cast<int>(g_status.size())) return false;
  Status& s = g_status[i];
  if (s != (queued ? Status::kAvailable : Status::kQueued)) return false;
  s = queued ? Status::kQueued : Status::kAvailable;
  return true;
}

void clear_queue() {
  Lock l;
  for (Status& s : g_status)
    if (s == Status::kQueued) s = Status::kAvailable;
}

int queued_count() {
  Lock l;
  int n = 0;
  for (Status s : g_status) n += (s == Status::kQueued);
  return n;
}

int awarded_count() {
  Lock l;
  int n = 0;
  for (Status s : g_status) n += (s == Status::kAwarded);
  return n;
}

void refresh_statuses(int player) {
  // Query outside the lock: the game calls must not hold it (the render thread
  // only ever takes it briefly, but the game side can be slow).
  std::vector<Info> table;
  {
    Lock l;
    if (!g_loaded) return;
    table = g_table;
  }
  std::vector<bool> achieved(table.size());
  for (size_t i = 0; i < table.size(); ++i) achieved[i] = game::achieved(player, table[i].index);
  Lock l;
  for (size_t i = 0; i < table.size() && i < g_status.size(); ++i) {
    if (achieved[i])
      g_status[i] = Status::kAwarded;
    else if (g_status[i] != Status::kQueued)
      g_status[i] = Status::kAvailable;
  }
}

int take_queue(unsigned* out, int max) {
  Lock l;
  int n = 0;
  for (size_t i = 0; i < g_status.size() && n < max; ++i) {
    if (g_status[i] != Status::kQueued) continue;
    out[n++] = g_table[i].index;
    g_status[i] = Status::kAvailable;  // refresh_statuses flips it to awarded
  }
  return n;
}

}  // namespace f3cg::challenges
