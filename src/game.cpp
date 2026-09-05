#include "game.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "log.h"
#include "mem.h"

namespace f3cg::game {
namespace {

// --- how the engine is reached ----------------------------------------------
// The engine registers Lua-visible methods through Despair::Reflection method
// proxies, keyed on the method-name string. Two code shapes exist:
//
//  Pattern A (GameScriptFEARScore and most classes):
//      B8 <impl>            mov  eax, impl              ; the C++ member function
//      89 47 20             mov  [edi+20h], eax
//      33 C0                xor  eax, eax
//      C7 47 10 <name>      mov  [edi+10h], "Name"
//      89 5F 14             mov  [edi+14h], ebx         ; ebx = the static class descriptor
//      89 77 18             mov  [edi+18h], esi
//      C7 07 <vtable>       mov  [edi], proxy vtable
//    and the registering function starts  83 EC xx 53 55 56 33 F6 57 BB <static>.
//
//  Pattern B (GameScriptGame):
//      B8 <fn> 50           mov eax, fn / push eax
//      68 <name>            push "Name"
//      E8 rel32             call make_proxy
//      83 C4 0C 50          add esp, 0Ch / push eax
//      B9 <static>          mov ecx, &GameScriptGame
//      E8 rel32             call AddMethod
//
// The static objects are class descriptors, not instances: the proxy's Invoke
// receives the instance from the script host. So the script wrappers are only
// read (for the vtable slots and offsets they encode) and never called; the mod
// calls the managers' virtual functions directly on the live objects, which it
// finds by their vtables (see instance_loop). HasPlayerStartedLevel is the one
// wrapper that is called: it uses no `this` at all.
using LevelFn = bool(__stdcall*)(int player);  // ignores this, one stack arg, ret 4
using ThisCall0 = void*(__fastcall*)(void* self, void* edx);
using ThisCall0U = unsigned(__fastcall*)(void* self, void* edx);
using ThisCall0B = bool(__fastcall*)(void* self, void* edx);
using MgrAwardFn = void(__fastcall*)(void* self, void* edx, int player, int count, unsigned challenge);
using MgrAchievedFn = bool(__fastcall*)(void* self, void* edx, int player, unsigned challenge);

uintptr_t g_base = 0;
mem::Range g_text;
std::vector<mem::Range> g_data;

uintptr_t g_fearscore_static = 0;  // GameScriptFEARScore class descriptor
uintptr_t g_game_static = 0;       // GameScriptGame class descriptor
uintptr_t g_award_impl = 0;        // GameScriptFEARScore::AwardChallengeRequirement (read, not called)
uintptr_t g_achieved_impl = 0;     // GameScriptFEARScore::HasPlayerAchievedChallenge (read, not called)
uintptr_t g_pause_impl = 0;        // GameScriptGame::IsPauseMenuShowing (read, not called)
LevelFn g_level = nullptr;         // GameScriptGame::HasPlayerStartedLevel (called)

// Parsed out of the wrappers (values for this build as documentation).
uint32_t g_off_scoremgr = 0;     // [ctx+0x188] -> IFearScoreMgr*
uint32_t g_off_menumgr = 0;      // [ctx+0x238] -> MenuMgr subobject
int g_slot_globaldata = -1;      // IFearScoreMgr vtbl[17]() -> FearScoreGlobalDataComponent iface
int g_slot_numchallenges = -1;   // iface vtbl[21]() -> count
int g_slot_award = -1;           // IFearScoreMgr vtbl[37](player, count, challenge)
int g_slot_achieved = -1;        // IFearScoreMgr vtbl[40](player, challenge)
int g_slot_pause = -1;           // MenuMgr sub-vtbl[69]()

// The live objects, found by vtable scan on the mod thread.
std::vector<mem::VtableInfo> g_fsm_vts, g_mm_vts;
uintptr_t g_fsm_vt0 = 0, g_mm_vt0 = 0, g_mm_sub_off = 0, g_mm_sub_vt = 0;
volatile uintptr_t g_fsm_obj = 0;
volatile uintptr_t g_mm_obj = 0;

bool g_ready = false;
char g_status[160] = "not started";

constexpr const char* kRttiFearScoreMgr = ".?AVFearScoreMgr@Despair@@";
constexpr const char* kRttiMenuMgr = ".?AVMenuMgr@Despair@@";

void set_status(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_status, sizeof(g_status), fmt, args);
  va_end(args);
}

uintptr_t rva(uintptr_t a) { return a ? a - g_base : 0; }

mem::Pattern with_imm32(const char* head, uint32_t imm, const char* tail = "") {
  mem::Pattern p = mem::parse_pattern(head);
  for (int i = 0; i < 4; ++i) p.bytes.push_back(static_cast<int16_t>((imm >> (8 * i)) & 0xFF));
  mem::Pattern t = mem::parse_pattern(tail);
  p.bytes.insert(p.bytes.end(), t.bytes.begin(), t.bytes.end());
  return p;
}

bool find_a(const char* name, uintptr_t* impl, uintptr_t* site) {
  const uintptr_t s = mem::find_cstring(g_data, name);
  if (!s) {
    logf("ERROR: name string '%s' not found in the exe", name);
    return false;
  }
  const mem::Pattern p = with_imm32("B8 ?? ?? ?? ?? 89 47 20 33 C0 C7 47 10", static_cast<uint32_t>(s));
  const uintptr_t a = mem::find_pattern(g_text, p);
  if (!a) {
    logf("ERROR: registration of '%s' not found (string at exe+0x%X)", name, rva(s));
    return false;
  }
  const uintptr_t fn = mem::read<uint32_t>(a + 1);
  if (!g_text.contains(fn)) {
    logf("ERROR: '%s' impl %p is outside .text", name, reinterpret_cast<void*>(fn));
    return false;
  }
  *impl = fn;
  *site = a;
  logf("found %-28s impl=exe+0x%06X (site exe+0x%06X)", name, rva(fn), rva(a));
  return true;
}

bool find_owner_a(uintptr_t site, uintptr_t* out) {
  const mem::Pattern prologue = mem::parse_pattern("83 EC ?? 53 55 56 33 F6 57 BB");
  const mem::Range whole = mem::module_range(reinterpret_cast<HMODULE>(g_base));
  const uintptr_t low = site > g_text.begin + 0x4000 ? site - 0x4000 : g_text.begin;
  for (uintptr_t a = site; a >= low; --a) {
    const mem::Range r{a, a + prologue.bytes.size()};
    if (mem::find_pattern(r, prologue) != a) continue;
    const uintptr_t obj = mem::read<uint32_t>(a + 10);
    if (!whole.contains(obj) || g_text.contains(obj)) continue;
    *out = obj;
    return true;
  }
  logf("ERROR: no registering-function prologue found above site exe+0x%06X", rva(site));
  return false;
}

bool find_b(const char* name, uintptr_t* fn, uintptr_t* owner) {
  const uintptr_t s = mem::find_cstring(g_data, name);
  if (!s) {
    logf("ERROR: name string '%s' not found in the exe", name);
    return false;
  }
  const mem::Pattern p = with_imm32("B8 ?? ?? ?? ?? 50 68", static_cast<uint32_t>(s),
                                    "E8 ?? ?? ?? ?? 83 C4 0C 50 B9 ?? ?? ?? ?? E8");
  const uintptr_t a = mem::find_pattern(g_text, p);
  if (!a) {
    logf("ERROR: registration of '%s' not found (string at exe+0x%X)", name, rva(s));
    return false;
  }
  const uintptr_t f = mem::read<uint32_t>(a + 1);
  const uintptr_t o = mem::read<uint32_t>(a + 21);
  const mem::Range whole = mem::module_range(reinterpret_cast<HMODULE>(g_base));
  if (!g_text.contains(f) || !whole.contains(o) || g_text.contains(o)) {
    logf("ERROR: '%s' fn %p / owner %p look wrong", name, reinterpret_cast<void*>(f),
         reinterpret_cast<void*>(o));
    return false;
  }
  *fn = f;
  *owner = o;
  logf("found %-28s fn=exe+0x%06X owner=exe+0x%06X", name, rva(f), rva(o));
  return true;
}

// The structural constants come out of the wrapper bytes, never from a table.
bool parse_layout() {
  const mem::Range achieved{g_achieved_impl, g_achieved_impl + 0x60};
  if (uintptr_t a = mem::find_pattern(achieved, "8B 47 08 8B 88 ?? ?? ?? ??"))
    g_off_scoremgr = mem::read<uint32_t>(a + 5);
  if (uintptr_t a = mem::find_pattern(achieved, "8B 92 ?? ?? ?? ?? 50 56 FF D2"))
    g_slot_achieved = static_cast<int>(mem::read<uint32_t>(a + 2) / 4);

  const mem::Range awardr{g_award_impl, g_award_impl + 0x1A0};
  if (uintptr_t a = mem::find_pattern(awardr, "8B 11 8B 42 ?? FF D0 85 C0 75"))
    g_slot_globaldata = mem::read<uint8_t>(a + 4) / 4;
  if (uintptr_t a = mem::find_pattern(awardr, "8B 10 8B C8 8B 42 ?? FF D0 8B 54 24 ?? 52 3B D0 73"))
    g_slot_numchallenges = mem::read<uint8_t>(a + 6) / 4;
  if (uintptr_t a = mem::find_pattern(awardr, "8B 01 8B 80 ?? ?? ?? ?? 52 57 FF D0"))
    g_slot_award = static_cast<int>(mem::read<uint32_t>(a + 4) / 4);

  const mem::Range pauser{g_pause_impl, g_pause_impl + 0x20};
  if (uintptr_t a = mem::find_pattern(pauser, "8B 49 08 E8 ?? ?? ?? ?? 8B 10 8B C8 8B 82 ?? ?? ?? ?? FF E0")) {
    g_slot_pause = static_cast<int>(mem::read<uint32_t>(a + 14) / 4);
    const uintptr_t getter = a + 8 + mem::read<int32_t>(a + 4);
    const mem::Range gr{getter, getter + 8};
    if (g_text.contains(getter) && mem::find_pattern(gr, "8B 81 ?? ?? ?? ?? C3") == getter)
      g_off_menumgr = mem::read<uint32_t>(getter + 2);
  }
  logf("layout: ctx+0x%X=IFearScoreMgr ctx+0x%X=MenuMgr | slots: globaldata=%d numchallenges=%d "
       "award=%d achieved=%d pausemenu=%d",
       g_off_scoremgr, g_off_menumgr, g_slot_globaldata, g_slot_numchallenges, g_slot_award,
       g_slot_achieved, g_slot_pause);
  if (g_slot_globaldata < 0 || g_slot_numchallenges < 0 || g_slot_award < 0 || g_slot_achieved < 0 ||
      g_slot_pause < 0) {
    logf("ERROR: could not parse the vtable slots out of the script wrappers");
    return false;
  }
  return true;
}

// The classes' vtables, from their RTTI. MenuMgr's IsPauseMenuShowing lives on
// a secondary vtable (the interface subobject at +4 in this build): pick the one
// whose slot g_slot_pause is the `cmp byte [ecx+..], 0` flag test.
bool resolve_class_vtables() {
  HMODULE exe = reinterpret_cast<HMODULE>(g_base);
  g_fsm_vts = mem::class_vtables(exe, kRttiFearScoreMgr);
  g_mm_vts = mem::class_vtables(exe, kRttiMenuMgr);
  for (const mem::VtableInfo& v : g_fsm_vts) {
    logf("FearScoreMgr vtable exe+0x%X (subobject +0x%X)", rva(v.vtable), v.offset);
    if (v.offset == 0) g_fsm_vt0 = v.vtable;
  }
  for (const mem::VtableInfo& v : g_mm_vts) {
    logf("MenuMgr vtable exe+0x%X (subobject +0x%X)", rva(v.vtable), v.offset);
    if (v.offset == 0) g_mm_vt0 = v.vtable;
    uintptr_t fn = 0;
    if (mem::read_safe(v.vtable + static_cast<uintptr_t>(g_slot_pause) * 4, &fn) && g_text.contains(fn)) {
      const mem::Range r{fn, fn + 8};
      if (mem::find_pattern(r, "80 79 ?? 00 74") == fn) {
        g_mm_sub_off = v.offset;
        g_mm_sub_vt = v.vtable;
      }
    }
  }
  if (!g_fsm_vt0 || !g_mm_vt0 || !g_mm_sub_vt) {
    logf("ERROR: class vtables not resolved (FearScoreMgr=%p MenuMgr=%p pause sub=%p)",
         reinterpret_cast<void*>(g_fsm_vt0), reinterpret_cast<void*>(g_mm_vt0),
         reinterpret_cast<void*>(g_mm_sub_vt));
    return false;
  }
  // The award/achieved slots must exist on the primary FearScoreMgr vtable.
  uintptr_t f1 = 0, f2 = 0, f3 = 0;
  if (!mem::read_safe(g_fsm_vt0 + g_slot_award * 4, &f1) || !g_text.contains(f1) ||
      !mem::read_safe(g_fsm_vt0 + g_slot_achieved * 4, &f2) || !g_text.contains(f2) ||
      !mem::read_safe(g_fsm_vt0 + g_slot_globaldata * 4, &f3) || !g_text.contains(f3)) {
    logf("ERROR: FearScoreMgr vtable lacks the expected slots");
    return false;
  }
  logf("FearScoreMgr::AwardChallengeRequirement=exe+0x%X HasPlayerAchievedChallenge=exe+0x%X "
       "GetGlobalData=exe+0x%X; MenuMgr::IsPauseMenuShowing on subobject +0x%X",
       rva(f1), rva(f2), rva(f3), g_mm_sub_off);
  return true;
}

// An object is the class if every one of its vtable pointers is in place.
bool is_instance(uintptr_t obj, const std::vector<mem::VtableInfo>& vts) {
  for (const mem::VtableInfo& v : vts) {
    uintptr_t vp = 0;
    if (!mem::read_safe(obj + v.offset, &vp) || vp != v.vtable) return false;
  }
  return true;
}

uintptr_t scan_for(const char* what, uintptr_t vt0, const std::vector<mem::VtableInfo>& vts) {
  const DWORD t0 = GetTickCount();
  size_t scanned = 0;
  std::vector<uintptr_t> hits = mem::find_objects_by_vtable(vt0, 16, &scanned);
  uintptr_t found = 0;
  for (uintptr_t h : hits)
    if (is_instance(h, vts)) {
      found = h;
      break;
    }
  if (found)
    logf("%s instance %p (%u candidate(s), %u MB scanned in %lu ms)", what, reinterpret_cast<void*>(found),
         static_cast<unsigned>(hits.size()), static_cast<unsigned>(scanned >> 20), GetTickCount() - t0);
  else if (config::get().trace)
    logf("%s not found yet (%u candidate(s), %u MB scanned in %lu ms)", what, static_cast<unsigned>(hits.size()),
         static_cast<unsigned>(scanned >> 20), GetTickCount() - t0);
  return found;
}

bool scan() {
  HMODULE exe = GetModuleHandleA(nullptr);
  g_base = reinterpret_cast<uintptr_t>(exe);
  g_text = mem::section(exe, ".text");
  g_data = mem::data_sections(exe);
  if (g_text.empty() || g_data.empty()) {
    set_status("exe sections not found");
    logf("ERROR: could not read the exe's section table");
    return false;
  }
  logf("exe base %p, .text exe+0x%X..0x%X, %u data sections", reinterpret_cast<void*>(g_base),
       rva(g_text.begin), rva(g_text.end), static_cast<unsigned>(g_data.size()));

  uintptr_t award_site = 0, achieved_site = 0;
  if (!find_a("AwardChallengeRequirement", &g_award_impl, &award_site)) return false;
  if (!find_a("HasPlayerAchievedChallenge", &g_achieved_impl, &achieved_site)) return false;
  uintptr_t owner_a = 0, owner_b = 0;
  if (!find_owner_a(award_site, &owner_a) || !find_owner_a(achieved_site, &owner_b) || owner_a != owner_b) {
    logf("ERROR: the two FEARScore methods do not share one class descriptor (exe+0x%X vs exe+0x%X)",
         rva(owner_a), rva(owner_b));
    return false;
  }
  uintptr_t level = 0, game_owner = 0, game_owner2 = 0;
  if (!find_b("IsPauseMenuShowing", &g_pause_impl, &game_owner)) return false;
  if (!find_b("HasPlayerStartedLevel", &level, &game_owner2)) return false;
  if (game_owner != game_owner2) {
    logf("ERROR: the two Game methods do not share one class descriptor (exe+0x%X vs exe+0x%X)",
         rva(game_owner), rva(game_owner2));
    return false;
  }
  g_fearscore_static = owner_a;
  g_game_static = game_owner;
  g_level = reinterpret_cast<LevelFn>(level);
  logf("class descriptors: GameScriptFEARScore@exe+0x%X GameScriptGame@exe+0x%X", rva(owner_a), rva(game_owner));
  return parse_layout() && resolve_class_vtables();
}

// The class descriptors carry the class name at +0xC once constructed; that is
// the check that the objects our signature scan named are what we think.
bool identity_ok(uintptr_t obj, const char* expected, bool* pending) {
  uintptr_t p = 0;
  *pending = false;
  if (!mem::read_safe(obj + 0xC, &p)) return false;
  if (!p) {
    *pending = true;
    return false;
  }
  const size_t n = std::strlen(expected) + 1;
  if (!mem::readable(reinterpret_cast<void*>(p), n)) return false;
  return std::memcmp(reinterpret_cast<const void*>(p), expected, n) == 0;
}

bool verify_identity() {
  const DWORD start = GetTickCount();
  for (;;) {
    bool pa = false, pb = false;
    const bool a = identity_ok(g_fearscore_static, "GameScriptFEARScore", &pa);
    const bool b = identity_ok(g_game_static, "GameScriptGame", &pb);
    if (a && b) {
      logf("class descriptors identified by name");
      return true;
    }
    if (!pa && !pb) {
      set_status("script classes are not what the scan expected");
      logf("ERROR: class descriptor names do not match (FEARScore ok=%d, Game ok=%d) - refusing to continue", a, b);
      return false;
    }
    if (GetTickCount() - start > 60000) {
      set_status("script classes never constructed");
      logf("ERROR: the class descriptors were not constructed within 60 s");
      return false;
    }
    Sleep(50);
  }
}

uintptr_t score_mgr_obj() {
  const uintptr_t o = g_fsm_obj;
  if (!o) return 0;
  uintptr_t vp = 0;
  if (!mem::read_safe(o, &vp) || vp != g_fsm_vt0) {
    g_fsm_obj = 0;  // gone; the mod thread rescans
    logf("score manager %p is gone - rescanning", reinterpret_cast<void*>(o));
    return 0;
  }
  return o;
}

uintptr_t menu_mgr_obj() {
  const uintptr_t o = g_mm_obj;
  if (!o) return 0;
  uintptr_t vp = 0;
  if (!mem::read_safe(o, &vp) || vp != g_mm_vt0) {
    g_mm_obj = 0;
    logf("menu manager %p is gone - rescanning", reinterpret_cast<void*>(o));
    return 0;
  }
  return o;
}

void* score_mgr() { return reinterpret_cast<void*>(score_mgr_obj()); }

}  // namespace

bool discover() {
  set_status("scanning");
  if (!scan()) {
    if (!std::strcmp(g_status, "scanning")) set_status("signature scan failed - see log");
    return false;
  }
  if (!verify_identity()) return false;
  g_ready = true;
  set_status("ready");
  logf("game layer ready - looking for the score and menu managers");
  return true;
}

void instance_loop() {
  if (!g_ready) return;
  for (;;) {
    if (!g_fsm_obj) g_fsm_obj = scan_for("FearScoreMgr", g_fsm_vt0, g_fsm_vts);
    if (!g_mm_obj) g_mm_obj = scan_for("MenuMgr", g_mm_vt0, g_mm_vts);
    Sleep(g_fsm_obj && g_mm_obj ? 1000 : 2000);
  }
}

bool ready() { return g_ready; }
bool context_ready() { return g_ready && g_fsm_obj && g_mm_obj; }

bool pause_menu_showing() {
  const uintptr_t m = menu_mgr_obj();
  if (!m) return false;
  const uintptr_t sub = m + g_mm_sub_off;
  uintptr_t vp = 0;
  if (!mem::read_safe(sub, &vp) || vp != g_mm_sub_vt) return false;
  auto fn = mem::read<ThisCall0B>(g_mm_sub_vt + static_cast<uintptr_t>(g_slot_pause) * 4);
  return fn(reinterpret_cast<void*>(sub), nullptr);
}

bool level_started(int player) {
  if (!g_ready || player < 0 || player >= 16) return false;
  return g_level(player);
}

bool achieved(int player, unsigned challenge) {
  const uintptr_t m = score_mgr_obj();
  if (!m || player < 0 || player >= 16) return false;
  auto fn = mem::read<MgrAchievedFn>(g_fsm_vt0 + static_cast<uintptr_t>(g_slot_achieved) * 4);
  return fn(reinterpret_cast<void*>(m), nullptr, player, challenge);
}

void award(int player, unsigned challenge, int count) {
  const uintptr_t m = score_mgr_obj();
  if (!m || player < 0 || player >= 16) return;
  const unsigned n = num_challenges();
  if (challenge >= n) {
    logf("ERROR: award(%u) refused - the game has %u challenges", challenge, n);
    return;
  }
  auto fn = mem::read<MgrAwardFn>(g_fsm_vt0 + static_cast<uintptr_t>(g_slot_award) * 4);
  fn(reinterpret_cast<void*>(m), nullptr, player, count, challenge);
}

void* score_global_data() {
  void* mgr = score_mgr();
  if (!mgr || g_slot_globaldata < 0) return nullptr;
  auto fn = mem::read<ThisCall0>(g_fsm_vt0 + static_cast<uintptr_t>(g_slot_globaldata) * 4);
  if (!fn) return nullptr;
  void* gd = fn(mgr, nullptr);
  static void* logged = nullptr;
  if (gd && gd != logged) {
    logged = gd;
    logf("score global data %p (%s)", gd, mem::rtti_short(mem::rtti_name(gd)));
  }
  return gd;
}

unsigned num_challenges() {
  void* gd = score_global_data();
  if (!gd || g_slot_numchallenges < 0) return 0;
  auto vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(gd));
  auto fn = mem::read<ThisCall0U>(vt + static_cast<uintptr_t>(g_slot_numchallenges) * 4);
  if (!fn) return 0;
  const unsigned n = fn(gd, nullptr);
  return n < 1024 ? n : 0;  // anything larger is not a challenge count
}

uintptr_t exe_base() { return g_base; }
const char* status_text() { return g_status; }

// --- the challenge table -----------------------------------------------------
// FearScoreGlobalDataComponent exposes (through the interface subobject the
// score manager hands out) a vector of 156-byte BoostChallengeInfo records and
// a vector of 40-byte ChallengeBoostCategoryInfo records. The offsets below were
// read off the interface's accessor functions and the class's reflection
// registrations; each one is verified against the accessor bytes before the
// table is trusted.
namespace {

constexpr uint32_t kChallengeVecBegin = 0x2c, kChallengeVecEnd = 0x30, kChallengeStride = 0x9c;
constexpr uint32_t kCategoryVecBegin = 0x20, kCategoryVecEnd = 0x24, kCategoryStride = 0x28;
constexpr int kSlotNumCategories = 11, kSlotCategoryTextId = 12, kSlotCategoryName = 13, kSlotNumChallenges = 21,
              kSlotChallengeTextId = 22, kSlotChallengeName = 23, kSlotChallengeInternal = 24,
              kSlotChallengePoints = 30, kSlotChallengeReqType = 32;
constexpr uint32_t kRecTextId = 0x08, kRecNeeded = 0x14, kRecPoints = 0x20, kRecInternal = 0x24,
                   kRecCategoryStr = 0x40, kRecStr5c = 0x5c, kRecIconStr = 0x78, kRecReqType = 0x94,
                   kRecRepeatable = 0x98, kRecResetOnLoad = 0x99;
constexpr uint32_t kCatInternal = 0x08, kCatTextId = 0x24;  // {?, ?, std::string internalName, int textId}

// MSVC 2008 std::string / std::wstring: { u32 alloc; union { T buf[16/sizeof T]; T* ptr; }; u32 size; u32 res; }
struct MsvcString {
  uint32_t alloc;
  union {
    char buf[16];
    char* ptr;
  };
  uint32_t size;
  uint32_t res;
};
struct MsvcWString {
  uint32_t alloc;
  union {
    wchar_t buf[8];
    wchar_t* ptr;
  };
  uint32_t size;
  uint32_t res;
};
static_assert(sizeof(MsvcString) == 0x1c && sizeof(MsvcWString) == 0x1c, "std::string layout");

bool read_msvc_string(uintptr_t at, char* out, size_t n) {
  MsvcString s{};
  out[0] = '\0';
  if (!mem::read_safe(at, &s)) return false;
  if (s.res < 15 || s.size > s.res || s.size > 4096) return false;
  const char* src = s.res < 16 ? s.buf : s.ptr;
  if (s.res >= 16 && !mem::readable(src, s.size + 1)) return false;
  const size_t len = s.size < n - 1 ? s.size : n - 1;
  std::memcpy(out, src, len);
  out[len] = '\0';
  return true;
}

// UTF-16 std::wstring -> UTF-8, which is what ImGui draws.
bool wstring_to_utf8(const MsvcWString& s, char* out, size_t n) {
  out[0] = '\0';
  if (s.size > 4096 || s.size > s.res) return false;
  const wchar_t* src = s.res < 8 ? s.buf : s.ptr;
  if (s.res >= 8 && !mem::readable(src, (s.size + 1) * sizeof(wchar_t))) return false;
  if (!s.size) return true;
  const int w = WideCharToMultiByte(CP_UTF8, 0, src, static_cast<int>(s.size), out, static_cast<int>(n - 1), nullptr, nullptr);
  out[w > 0 ? w : 0] = '\0';
  return w > 0;
}

// The game's own Name-object -> localised std::wstring conversion, found as the
// call inside the challenge-name accessor (slot 23). __thiscall on the Name
// object with the hidden return slot as its one argument.
using NameToWStringFn = void*(__fastcall*)(void* name_obj, void* edx, MsvcWString* out);
using ChallengeNameFn = void*(__fastcall*)(void* self, void* edx, MsvcWString* out, unsigned i);
NameToWStringFn g_name_to_wstring = nullptr;

bool slot_matches(void* obj, int slot, const char* pattern) {
  uintptr_t vt = 0;
  if (!mem::read_safe(reinterpret_cast<uintptr_t>(obj), &vt)) return false;
  uintptr_t fn = 0;
  if (!mem::read_safe(vt + static_cast<uintptr_t>(slot) * 4, &fn) || !g_text.contains(fn)) return false;
  const mem::Pattern p = mem::parse_pattern(pattern);
  const mem::Range r{fn, fn + p.bytes.size()};
  return mem::find_pattern(r, p) == fn;
}

void* slot_fn(void* obj, int slot) {
  uintptr_t vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(obj));
  return mem::read<void*>(vt + static_cast<uintptr_t>(slot) * 4);
}

// A localised name. The std::wstring the game builds may own a buffer from the
// game's allocator; the table is read once per session, so those few bytes are
// left to it rather than guessing at the allocator's free.
void challenge_name(void* iface, unsigned i, char* out, size_t n) {
  MsvcWString s{};
  auto fn = reinterpret_cast<ChallengeNameFn>(slot_fn(iface, kSlotChallengeName));
  out[0] = '\0';
  if (!fn) return;
  fn(iface, nullptr, &s, i);
  wstring_to_utf8(s, out, n);
}

// Any text id, through the same helper the name accessor uses: the record's own
// name object (its first 12 bytes: {?, text context, text id}) is copied and only
// the text id swapped, so the helper sees exactly the inputs it always sees.
void record_text(uintptr_t rec, int text_id, char* out, size_t n) {
  out[0] = '\0';
  if (!g_name_to_wstring || text_id <= 0) return;
  uint32_t name_obj[3] = {};
  if (!mem::read_safe(rec, &name_obj)) return;
  name_obj[2] = static_cast<uint32_t>(text_id);
  MsvcWString s{};
  g_name_to_wstring(name_obj, nullptr, &s);
  wstring_to_utf8(s, out, n);
}

void category_name(void* iface, unsigned c, char* out, size_t n) {
  MsvcWString s{};
  auto fn = reinterpret_cast<ChallengeNameFn>(slot_fn(iface, kSlotCategoryName));
  out[0] = '\0';
  if (!fn) return;
  fn(iface, nullptr, &s, c);
  wstring_to_utf8(s, out, n);
}

bool verify_table_layout(void* iface) {
  struct Check {
    int slot;
    const char* bytes;
    const char* what;
  };
  static const Check kChecks[] = {
      {kSlotNumChallenges, "56 8B 71 30 2B 71 2C B8 D3 20 0D D2", "challenge vector at +0x2c/+0x30, stride 0x9c"},
      {kSlotChallengeTextId, "8B 44 24 04 8B 49 2C 69 C0 9C 00 00 00 8B 44 08 08 C2 04 00", "text id at +0x08"},
      {kSlotChallengeInternal, "8B 44 24 04 8B 49 2C 69 C0 9C 00 00 00 8D 44 08 24 C2 04 00", "internalName at +0x24"},
      {kSlotChallengePoints, "8B 44 24 04 8B 49 2C 69 C0 9C 00 00 00 66 8B 44 08 20 C2 04 00", "pointValue at +0x20"},
      {kSlotChallengeReqType, "8B 54 24 04 8B C2 69 C0 9C 00 00 00 56 8B 71 2C 8B 84 30 94 00 00 00 83 F8 01", "requirement kind at +0x94"},
      {kSlotChallengeName, "8B C1 8B 4C 24 08 69 C9 9C 00 00 00 03 48 2C 56 8B 74 24 08 56 E8", "name accessor shape"},
      {kSlotNumCategories, "8B 51 24 2B 51 20 B8 67 66 66 66", "category vector at +0x20/+0x24, stride 0x28"},
      {kSlotCategoryTextId, "8B 44 24 04 8B 49 20 8D 04 80 8B 44 C1 24 C2 04 00", "category text id at +0x24"},
      {kSlotCategoryName, "8B 44 24 08 8B 49 20 56 8B 74 24 08 8D 04 80 56 8D 0C C1 E8", "category name accessor shape"},
  };
  for (const Check& c : kChecks) {
    if (!slot_matches(iface, c.slot, c.bytes)) {
      logf("ERROR: challenge table layout check failed at slot %d (%s) - not reading the table",
           c.slot, c.what);
      return false;
    }
  }
  // The Name -> wstring helper is the call inside the name accessor.
  const auto acc = reinterpret_cast<uintptr_t>(slot_fn(iface, kSlotChallengeName));
  const uintptr_t call = acc + 21;  // the E8 after the verified prefix
  const uintptr_t target = call + 5 + mem::read<int32_t>(call + 1);
  if (mem::read<uint8_t>(call) != 0xE8 || !g_text.contains(target)) {
    logf("ERROR: name helper call not found in the name accessor");
    return false;
  }
  g_name_to_wstring = reinterpret_cast<NameToWStringFn>(target);
  logf("name helper exe+0x%X", rva(target));
  return true;
}

}  // namespace

bool read_challenges(std::vector<ChallengeDef>& out, std::vector<CategoryDef>& cats) {
  void* iface = score_global_data();
  if (!iface) return false;
  static void* verified = nullptr;
  static void* rejected = nullptr;
  if (rejected == iface) return false;  // already reported; do not retry every second
  if (verified != iface) {
    if (!verify_table_layout(iface)) {
      rejected = iface;
      return false;
    }
    verified = iface;
    logf("challenge table layout verified on %p", iface);
  }
  const auto base = reinterpret_cast<uintptr_t>(iface);
  uintptr_t cb = 0, ce = 0, kb = 0, ke = 0;
  if (!mem::read_safe(base + kChallengeVecBegin, &cb) || !mem::read_safe(base + kChallengeVecEnd, &ce) ||
      !mem::read_safe(base + kCategoryVecBegin, &kb) || !mem::read_safe(base + kCategoryVecEnd, &ke))
    return false;
  if (!cb || ce < cb || (ce - cb) % kChallengeStride || !kb || ke < kb || (ke - kb) % kCategoryStride) {
    logf("ERROR: challenge vectors look wrong (%p..%p, %p..%p)", reinterpret_cast<void*>(cb),
         reinterpret_cast<void*>(ce), reinterpret_cast<void*>(kb), reinterpret_cast<void*>(ke));
    return false;
  }
  const unsigned n = (ce - cb) / kChallengeStride, nc = (ke - kb) / kCategoryStride;
  if (!n || n > 512 || nc > 64 || !mem::readable(reinterpret_cast<void*>(cb), ce - cb) ||
      (nc && !mem::readable(reinterpret_cast<void*>(kb), ke - kb)))
    return false;

  cats.clear();
  for (unsigned c = 0; c < nc; ++c) {
    const uintptr_t rec = kb + c * kCategoryStride;
    CategoryDef d;
    d.text_id = mem::read<int>(rec + kCatTextId);
    read_msvc_string(rec + kCatInternal, d.internal, sizeof(d.internal));
    category_name(iface, c, d.name, sizeof(d.name));
    cats.push_back(d);
  }

  out.clear();
  for (unsigned i = 0; i < n; ++i) {
    const uintptr_t rec = cb + i * kChallengeStride;
    ChallengeDef d;
    d.index = i;
    d.text_id = mem::read<int>(rec + kRecTextId);
    d.points = mem::read<uint16_t>(rec + kRecPoints);
    const int needed = mem::read<int>(rec + kRecNeeded);
    d.needed = needed > 0 ? needed : 1;
    d.req_type = mem::read<int>(rec + kRecReqType);
    d.repeatable = mem::read<uint8_t>(rec + kRecRepeatable) != 0;
    d.reset_on_load = mem::read<uint8_t>(rec + kRecResetOnLoad) != 0;
    for (int k = 0; k < 5; ++k) d.raw[k] = mem::read<int>(rec + 0x0c + k * 4);
    read_msvc_string(rec + kRecInternal, d.internal, sizeof(d.internal));
    read_msvc_string(rec + kRecCategoryStr, d.category_internal, sizeof(d.category_internal));
    read_msvc_string(rec + kRecIconStr, d.icon_internal, sizeof(d.icon_internal));
    challenge_name(iface, i, d.name, sizeof(d.name));
    record_text(rec, d.raw[0], d.description, sizeof(d.description));  // +0x0c: the requirement text
    for (unsigned c = 0; c < nc; ++c)
      if (cats[c].internal[0] && !std::strcmp(cats[c].internal, d.category_internal)) {
        d.category = static_cast<int>(c);
        break;
      }
    out.push_back(d);
  }
  return true;
}

// --- trace hooks -------------------------------------------------------------
namespace {

using MgrAwardFn = void(__fastcall*)(void* self, void* edx, int player, int count, unsigned challenge);
using MgrAchievedFn = bool(__fastcall*)(void* self, void* edx, int player, unsigned challenge);
MgrAwardFn g_trace_award = nullptr;
MgrAchievedFn g_trace_achieved = nullptr;
uintptr_t g_trace_vt = 0;
int g_trace_award_slot = -1, g_trace_achieved_slot = -1;

void __fastcall hk_mgr_award(void* self, void* edx, int player, int count, unsigned challenge) {
  logf("trace: FearScoreMgr::AwardChallengeRequirement(player=%d, count=%d, challenge=%u) on thread %lu",
       player, count, challenge, GetCurrentThreadId());
  g_trace_award(self, edx, player, count, challenge);
}

bool __fastcall hk_mgr_achieved(void* self, void* edx, int player, unsigned challenge) {
  static int shown = 0;
  const bool r = g_trace_achieved(self, edx, player, challenge);
  if (shown < 12) {
    ++shown;
    logf("trace: FearScoreMgr::HasPlayerAchievedChallenge(player=%d, challenge=%u) -> %d on thread %lu",
         player, challenge, r, GetCurrentThreadId());
  }
  return r;
}

}  // namespace

void install_trace_hooks() {
  if (g_trace_vt || g_slot_award < 0 || g_slot_achieved < 0) return;
  void* mgr = score_mgr();
  if (!mgr) return;
  const uintptr_t vt = mem::read<uintptr_t>(reinterpret_cast<uintptr_t>(mgr));
  g_trace_award = reinterpret_cast<MgrAwardFn>(mem::hook_vtable(vt, g_slot_award, reinterpret_cast<void*>(&hk_mgr_award)));
  g_trace_achieved = reinterpret_cast<MgrAchievedFn>(mem::hook_vtable(vt, g_slot_achieved, reinterpret_cast<void*>(&hk_mgr_achieved)));
  g_trace_vt = vt;
  g_trace_award_slot = g_slot_award;
  g_trace_achieved_slot = g_slot_achieved;
  logf("trace: hooked FearScoreMgr vtable slots %d/%d (vtable exe+0x%X)", g_slot_award, g_slot_achieved, rva(vt));
}

void remove_trace_hooks() {
  if (!g_trace_vt || !mem::readable(reinterpret_cast<void*>(g_trace_vt), 48 * sizeof(void*))) return;
  if (g_trace_award) mem::write<void*>(g_trace_vt + g_trace_award_slot * 4, reinterpret_cast<void*>(g_trace_award));
  if (g_trace_achieved) mem::write<void*>(g_trace_vt + g_trace_achieved_slot * 4, reinterpret_cast<void*>(g_trace_achieved));
  g_trace_vt = 0;
}

}  // namespace f3cg::game
