#pragma once

namespace f3cg::config {

struct Settings {
  int toggle_key = 0x77;      // VK_F8: hide/show the panel while the pause menu is up
  int player_index = 0;       // local player slot the grants go to (0 in single player)
  int grant_delay_ms = 500;   // wait this long after unpausing before granting
  bool trace = false;         // verbose diagnostics (thread ids, every call)
  bool always_show = false;   // debug: draw the panel regardless of pause/level state
  // "Disable = overlay,dispatch,game" turns whole subsystems off so a fault can
  // be bisected to one of them.
  bool disable_overlay = false;
  bool disable_dispatch = false;
  bool disable_game = false;
};

const Settings& get();

// Fear3ChallengeGrant.ini next to the DLL. Written with documented defaults the
// first time so the options are discoverable.
void load(const char* dir);
const char* dir();  // the DLL's directory, with a trailing separator

}  // namespace f3cg::config
