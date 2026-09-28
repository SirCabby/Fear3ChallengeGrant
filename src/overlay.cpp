#include "overlay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "challenges.h"
#include "config.h"
#include "dispatch.h"
#include "game.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "adopt.h"
#include "log.h"
#include "mem.h"
#include "version.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace f3cg::overlay {
namespace {

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_orig_gpa = nullptr;

Backend g_backend = Backend::kNone;
HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;
bool g_context_ready = false;
bool g_visible = false;
bool g_user_hidden = false;  // the toggle key, while the pause menu is up
char g_filter[64] = {};

bool contains_ci(const char* haystack, const char* needle) {
  if (!needle || !*needle) return true;
  for (const char* h = haystack; *h; ++h) {
    const char *a = h, *b = needle;
    while (*a && *b && (*a | 0x20) == (*b | 0x20)) ++a, ++b;
    if (!*b) return true;
  }
  return false;
}

// The filter box matches the name, the internal name and the category.
bool passes_filter(const challenges::Info& c) {
  return contains_ci(c.display, g_filter) || contains_ci(c.internal, g_filter) ||
         (c.category >= 0 && contains_ci(challenges::category_name(c.category), g_filter));
}

FARPROC WINAPI hk_get_proc_address(HMODULE module, LPCSTR name) {
  FARPROC real = g_orig_gpa(module, name);
  if (!real || !name || !HIWORD(reinterpret_cast<uintptr_t>(name))) return real;
  if (FARPROC w = dx11::wrap(name, real)) return w;
  if (FARPROC w = dx9::wrap(name, real)) return w;
  return real;
}

LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  const dispatch::Snapshot s = dispatch::snapshot();
  const bool showable = s.show_panel || config::get().always_show;
  if (msg == WM_KEYDOWN && static_cast<int>(wp) == config::get().toggle_key && showable) {
    g_user_hidden = !g_user_hidden;
    logf("panel %s by the toggle key", g_user_hidden ? "hidden" : "shown");
    return 0;
  }
  if (g_visible && g_context_ready) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    const ImGuiIO& io = ImGui::GetIO();
    // Swallow only what ImGui is using: clicks on the panel, typing in the
    // filter box. Everything else - Escape to resume, clicks on the game's own
    // pause menu beside the panel - goes through untouched.
    if (io.WantCaptureMouse && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0;
    if (io.WantTextInput && msg >= WM_KEYFIRST && msg <= WM_KEYLAST && wp != VK_ESCAPE) return 0;
  }
  return CallWindowProcA(g_orig_wndproc, hwnd, msg, wp, lp);
}

enum : ImGuiID { kColName = 1, kColCategory, kColPoints, kColStatus, kColAction };

int compare_ci(const char* a, const char* b) {
  for (;; ++a, ++b) {
    const int ca = (*a | 0x20), cb = (*b | 0x20);
    if (!*a || !*b || ca != cb) return (*a ? ca : 0) - (*b ? cb : 0);
  }
}

// Three-way comparison of two rows for a sort column; ties fall back to the
// name, then the game's order, so the result is stable and predictable.
int compare_rows(int a, int b, ImGuiID column) {
  const challenges::Info ca = challenges::info(a), cb = challenges::info(b);
  int r = 0;
  switch (column) {
    case kColCategory:
      r = compare_ci(ca.category >= 0 ? challenges::category_name(ca.category) : "",
                     cb.category >= 0 ? challenges::category_name(cb.category) : "");
      break;
    case kColPoints: r = (ca.points > cb.points) - (ca.points < cb.points); break;
    case kColStatus: {
      const int sa = static_cast<int>(challenges::status(a)), sb = static_cast<int>(challenges::status(b));
      r = (sa > sb) - (sa < sb);
      break;
    }
    default: break;
  }
  if (!r) r = compare_ci(ca.display, cb.display);
  if (!r) r = (a > b) - (a < b);
  return r;
}

const char* status_label(challenges::Status s) {
  switch (s) {
    case challenges::Status::kAvailable: return "Available";
    case challenges::Status::kQueued: return "Queued";
    case challenges::Status::kAwarded: return "Awarded";
    default: return "?";
  }
}

}  // namespace

bool claim(Backend b) {
  if (g_backend == Backend::kNone) {
    g_backend = b;
    logf("overlay: %s is the active renderer", b == Backend::kDx11 ? "D3D11" : "D3D9");
  }
  return g_backend == b;
}

bool ensure_context(HWND hwnd) {
  if (g_context_ready) return true;
  if (!hwnd) return false;
  g_hwnd = hwnd;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // Window position/size and column layout persist beside the DLL, under the
  // mod's own name rather than a stray imgui.ini in the game folder.
  static char ini_path[MAX_PATH] = {};
  std::snprintf(ini_path, sizeof(ini_path), "%sFear3ChallengeGrant.imgui.ini", config::dir());
  io.IniFilename = ini_path;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();
  ImGui::GetStyle().WindowRounding = 4.0f;

  if (!ImGui_ImplWin32_Init(hwnd)) {
    logf("ERROR: ImGui Win32 backend init failed");
    ImGui::DestroyContext();
    return false;
  }
  g_orig_wndproc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hk_wndproc)));
  g_context_ready = true;
  logf("overlay ready (hwnd=%p, render thread %lu)", hwnd, GetCurrentThreadId());
  return true;
}

bool wants_draw() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const bool showable = s.show_panel || config::get().always_show;
  if (!showable) g_user_hidden = false;  // the next pause starts visible again
  g_visible = g_context_ready && showable && !g_user_hidden;
  ImGui::GetIO().MouseDrawCursor = g_visible;  // the game may hide the OS cursor
  return g_visible;
}

void draw_panel() {
  const dispatch::Snapshot s = dispatch::snapshot();
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowSize(ImVec2(700, 560), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 720, 60), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("F.E.A.R. 3 - Challenge Grant  v" F3CG_VERSION)) {
    ImGui::End();
    return;
  }

  if (!s.game_ready) {
    ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "Game hooks: %s", game::status_text());
    ImGui::End();
    return;
  }
  if (!s.table_loaded) {
    if (!s.context_ready) ImGui::TextDisabled("Waiting for the game's score and menu managers (a level must be loaded)...");
    else ImGui::TextDisabled("Waiting for the score manager / challenge table...");
    ImGui::End();
    return;
  }

  const int n = challenges::count();
  const int awarded = challenges::awarded_count();
  const int queued = challenges::queued_count();
  // What Grant all would queue: every available row the filter lets through.
  std::vector<int> grantable;
  for (int i = 0; i < n; ++i)
    if (challenges::status(i) == challenges::Status::kAvailable && passes_filter(challenges::info(i)))
      grantable.push_back(i);

  ImGui::Text("%d of %d challenges achieved this mission", awarded, n);
  ImGui::SameLine(0, 20);
  ImGui::SetNextItemWidth(180);
  ImGui::InputTextWithHint("##filter", "filter...", g_filter, sizeof(g_filter));
  ImGui::SameLine();
  // With a filter typed the button scopes itself to the listed rows and says so.
  const bool filtered = g_filter[0] != '\0';
  ImGui::BeginDisabled(grantable.empty());
  if (ImGui::Button(filtered ? "Grant shown" : "Grant all")) {
    int added = 0;
    for (int i : grantable) added += challenges::set_queued(i, true);
    logf("queued %d challenge(s) with %s", added, filtered ? "Grant shown" : "Grant all");
  }
  ImGui::EndDisabled();
  if (grantable.empty())
    ImGui::SetItemTooltip("Nothing left to queue%s", filtered ? " in the filtered list" : "");
  else
    ImGui::SetItemTooltip("Queue %s %d challenge%s still available; they are granted once you unpause",
                          filtered ? "the" : "all", static_cast<int>(grantable.size()),
                          grantable.size() == 1 ? "" : "s");
  ImGui::SameLine();
  if (ImGui::Button("Clear queue")) {
    challenges::clear_queue();
    dispatch::request_refresh();
  }
  ImGui::Separator();

  const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                                ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                                ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
                                ImGuiTableFlags_SortTristate | ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("challenges", 5, flags, ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.5f))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Challenge", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 3.0f, kColName);
    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 1.2f, kColCategory);
    ImGui::TableSetupColumn("Points", ImGuiTableColumnFlags_WidthStretch, 0.7f, kColPoints);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.9f, kColStatus);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoSort, 0.8f, kColAction);
    ImGui::TableHeadersRow();

    // Row order: the game's own order unless a header was clicked. Rows are
    // few, so the order is rebuilt every frame (statuses change under it).
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
      const ImGuiTableColumnSortSpecs& sp = specs->Specs[0];
      const bool desc = sp.SortDirection == ImGuiSortDirection_Descending;
      std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const int r = compare_rows(a, b, sp.ColumnUserID);
        return desc ? r > 0 : r < 0;
      });
    }

    for (int row = 0; row < n; ++row) {
      const int i = order[row];
      const challenges::Info c = challenges::info(i);
      if (!passes_filter(c)) continue;
      const challenges::Status st = challenges::status(i);
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      // A selectable spanning the row makes the whole row the hover target for
      // the description; AllowOverlap keeps the Grant button clickable on top.
      if (st == challenges::Status::kAwarded) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
      ImGui::Selectable(c.display, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
      if (st == challenges::Status::kAwarded) ImGui::PopStyleColor();
      if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(420.0f);
        ImGui::TextUnformatted(c.display);
        if (c.description[0]) {
          ImGui::Separator();
          ImGui::TextUnformatted(c.description);
        }
        ImGui::Separator();
        ImGui::TextDisabled("%d points%s%s", c.points, c.repeatable ? "  |  repeatable" : "",
                            c.category >= 0 ? "  |  " : "");
        if (c.category >= 0) {
          ImGui::SameLine(0, 0);
          ImGui::TextDisabled("%s", challenges::category_name(c.category));
        }
        ImGui::TextDisabled("%s (index %u, requirement %d)", c.internal, c.index, c.needed);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", c.category >= 0 ? challenges::category_name(c.category) : "");
      ImGui::TableNextColumn();
      if (c.points > 0) ImGui::Text("%d", c.points);
      else ImGui::TextDisabled("-");
      ImGui::TableNextColumn();
      switch (st) {
        case challenges::Status::kAwarded:
          ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "Awarded");
          break;
        case challenges::Status::kQueued:
          ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1), "Queued");
          break;
        default:
          ImGui::TextUnformatted(status_label(st));
          break;
      }
      ImGui::TableNextColumn();
      if (st == challenges::Status::kAvailable) {
        if (ImGui::SmallButton("Grant") && challenges::set_queued(i, true))
          logf("queued [%02u] %s", c.index, c.display);
      } else if (st == challenges::Status::kQueued) {
        if (ImGui::SmallButton("Cancel") && challenges::set_queued(i, false))
          logf("unqueued [%02u] %s", c.index, c.display);
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  if (queued > 0)
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1),
                       "%d queued - granted %d ms after you unpause (reloading a checkpoint or leaving discards them)",
                       queued, config::get().grant_delay_ms);
  else if (s.pending_ms >= 0)
    ImGui::TextDisabled("Granting in %d ms...", s.pending_ms);
  else
    ImGui::TextDisabled("Click Grant on a challenge, or Grant all; they are awarded once you unpause. Awarded ones reset with the checkpoint/level.");
  ImGui::End();
}

void shutdown_imgui() {
  if (!g_context_ready) return;
  ImGui_ImplWin32_Shutdown();
  if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
  g_context_ready = false;
  g_visible = false;
}

bool install() {
  adopt::init();  // on the game's primary thread: the one that makes its window
  void* prev = mem::iat_hook(GetModuleHandleA(nullptr), "KERNEL32.dll", "GetProcAddress",
                             reinterpret_cast<void*>(&hk_get_proc_address));
  if (!prev) {
    logf("ERROR: could not hook GetProcAddress in the exe's import table - no overlay");
    return false;
  }
  g_orig_gpa = reinterpret_cast<GetProcAddressFn>(prev);
  logf("overlay: GetProcAddress import hooked (original %p)", prev);
  return true;
}

void uninstall() {
  // Window procedure first: a message arriving after our image is gone would
  // jump into freed memory.
  if (g_orig_wndproc && g_hwnd && IsWindow(g_hwnd)) {
    SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_wndproc));
    g_orig_wndproc = nullptr;
  }
  dx11::uninstall();
  dx9::uninstall();
  shutdown_imgui();
  if (g_orig_gpa) {
    mem::iat_hook(GetModuleHandleA(nullptr), "KERNEL32.dll", "GetProcAddress",
                  reinterpret_cast<void*>(g_orig_gpa));
    g_orig_gpa = nullptr;
  }
  logf("overlay hooks removed");
}

}  // namespace f3cg::overlay
