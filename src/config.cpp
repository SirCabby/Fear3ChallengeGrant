#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace f3cg::config {
namespace {

Settings g_settings;
char g_path[MAX_PATH] = {};
char g_dir[MAX_PATH] = {};

bool truthy(const char* v) {
  return !_stricmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "true") || !_stricmp(v, "yes");
}

void write_defaults() {
  FILE* f = std::fopen(g_path, "w");
  if (!f) return;
  std::fprintf(f,
               "# Fear3ChallengeGrant\n"
               "#\n"
               "#   ToggleKey    = 0x77   ; virtual-key code that hides/shows the panel while paused (0x77 = F8)\n"
               "#   PlayerIndex  = 0      ; local player slot that receives the challenges (0 in single player)\n"
               "#   GrantDelayMs = 500    ; delay after unpausing before queued challenges are granted\n"
               "#   AlwaysShow   = 0      ; debug: draw the panel even outside the pause menu\n"
               "#   Trace        = 0      ; verbose diagnostics in Fear3ChallengeGrant.log\n"
               "#   Disable      =        ; comma list of subsystems to turn off: overlay,dispatch,game\n"
               "ToggleKey = 0x77\n"
               "PlayerIndex = 0\n"
               "GrantDelayMs = 500\n"
               "AlwaysShow = 0\n"
               "Trace = 0\n"
               "Disable =\n");
  std::fclose(f);
}

}  // namespace

const Settings& get() { return g_settings; }

const char* dir() { return g_dir; }

void load(const char* dir) {
  std::snprintf(g_dir, sizeof(g_dir), "%s", dir);
  std::snprintf(g_path, sizeof(g_path), "%sFear3ChallengeGrant.ini", dir);
  FILE* f = std::fopen(g_path, "r");
  if (!f) {
    write_defaults();
    logf("config: no %s - wrote one with the defaults", g_path);
    return;
  }
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == ';' || *p == '[' || *p == '\n' || *p == '\r' || !*p) continue;
    char* eq = std::strchr(p, '=');
    if (!eq) continue;
    *eq = '\0';
    char* key = p;
    char* val = eq + 1;
    for (char* e = key + std::strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t');) *--e = '\0';
    while (*val == ' ' || *val == '\t') ++val;
    for (char* e = val + std::strlen(val);
         e > val && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t');)
      *--e = '\0';

    if (!_stricmp(key, "ToggleKey")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v > 0 && v < 256) g_settings.toggle_key = v;
    } else if (!_stricmp(key, "PlayerIndex")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v >= 0 && v < 16) g_settings.player_index = v;
    } else if (!_stricmp(key, "GrantDelayMs")) {
      int v = static_cast<int>(std::strtol(val, nullptr, 0));
      if (v >= 0 && v <= 60000) g_settings.grant_delay_ms = v;
    } else if (!_stricmp(key, "AlwaysShow")) {
      g_settings.always_show = truthy(val);
    } else if (!_stricmp(key, "Trace")) {
      g_settings.trace = truthy(val);
    } else if (!_stricmp(key, "Disable")) {
      g_settings.disable_overlay = std::strstr(val, "overlay") != nullptr;
      g_settings.disable_dispatch = std::strstr(val, "dispatch") != nullptr;
      g_settings.disable_game = std::strstr(val, "game") != nullptr;
    } else {
      logf("config: unknown key '%s' ignored", key);
    }
  }
  std::fclose(f);
  logf("config: ToggleKey=0x%02X PlayerIndex=%d GrantDelayMs=%d AlwaysShow=%d Trace=%d "
       "Disable(overlay=%d dispatch=%d game=%d)",
       g_settings.toggle_key, g_settings.player_index, g_settings.grant_delay_ms,
       g_settings.always_show, g_settings.trace, g_settings.disable_overlay,
       g_settings.disable_dispatch, g_settings.disable_game);
}

}  // namespace f3cg::config
