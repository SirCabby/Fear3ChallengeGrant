# Fear3ChallengeGrant — project guide

A client-side mod for **F.E.A.R. 3** (Steam appid 21100, 32-bit `F.E.A.R. 3.exe`, Day 1 Studios'
Despair engine, Steam CEG-protected), cross-built on Linux with mingw-w64. It draws a Dear ImGui
panel over the in-game pause menu listing every campaign challenge; clicking one queues it, and the
game's own award path grants it the moment the player unpauses. `README.md` is the user-facing doc;
this file records **what was reverse-engineered**, so none of it has to be rediscovered.

## Build & deploy

```sh
make            # -> build/binkw32.dll   (config.mk sets GAME_DIR; gitignored)
make install    # rename stock binkw32.dll -> binkw32_orig.dll (once), deploy ours atomically
make uninstall  # restore the stock DLL
make version X.Y.Z  # set the version (make rev X.Y.Z is the same);  make package -> dist/Fear3ChallengeGrant_vX.Y.Z.zip
python3 tools/find_regs.py --exe "$GAME_DIR/F.E.A.R. 3.exe" --list   # every script-function registration
```

The log (`Fear3ChallengeGrant.log`), ini (`Fear3ChallengeGrant.ini`) and ImGui layout file
(`Fear3ChallengeGrant.imgui.ini`) sit beside the DLL in the game folder. `Trace = 1` adds thread ids and the `_purecall` hook; `AlwaysShow = 1` draws the panel
outside the pause menu (rendering test); `Disable = overlay,dispatch,game` bisects a fault.

`tests/test_adopt.cpp` runs the renderer hooks under Wine with the Steam overlay's way of hooking
played by the test (build and run commands in its header; `F3CG_ADOPT=1` makes the proxy take the
Windows path under Wine). See "Renderer capture".

## ⛔ Rules

- **Never patch `F.E.A.R. 3.exe` on disk.** It is Steam CEG-protected (per-user generated, with
  `CEG_Protect` wrappers on a few functions). Proxy DLL plus in-memory hooks only.
- **Never use absolute addresses.** Everything is found by signature at runtime from the method-name
  strings (see `src/game.cpp`), verified with MSVC RTTI before use, and logged as `exe+RVA`. The VAs
  in this file are documentation for build id 3576 (`ResVersion 16.00.20.0275`), not inputs.
- **Game calls only on the main thread.** `dispatch.cpp`'s `PeekMessageA` hook is the one place
  that talks to the engine; the render thread only reads snapshots under the lock.
- **Back up the saves before experiments:** `~/.local/share/Steam/userdata/<id>/21100/local/`
  (`FEAR3*.dsSave`, `User.profile`). Challenge grants update lifetime stats in there.
- **Unhook before teardown** on a FreeLibrary, in order: WndProc, vtables, ImGui, import table. On a
  process exit DllMain does nothing (see Renderer capture). `binkw32_orig.dll` is loaded and pinned
  in `DllMain` so it is torn down late (Fear2AwardUnlocker's R6025 lesson).
- **On Windows, no function of the mod's in a vtable another hooker reads** (Renderer capture).
- The user commits every repo himself — do not `git commit`/`push` unless asked.

## What the game does

### Loading: the Bink proxy
The exe statically imports 24 `_Bink*@N` symbols (RAD's raw stdcall names, leading underscore
included) from `binkw32.dll`, which ships with the game and is not a Wine builtin — so a proxy is a
drop-in under Proton and Windows alike (same reasoning as F2's `LTMemory.dll`). `binkw32.def`
aliases each name onto a `jmp [table]` thunk (`src/bink_proxy.cpp`). Because we are a static import,
`DllMain` runs before `WinMain`, which is what makes the import-table hooks below race-free.

### Script-function registrations (the address source)
Engine classes expose methods to Lua through `Despair::Reflection::ClassMethodProxy_*` objects,
registered by name string. Two code shapes, both signature-scannable from the string:

| Pattern | Bytes | Yields |
| --- | --- | --- |
| A (`GameScriptFEARScore` etc.) | `B8 <impl> 89 47 20 33 C0 C7 47 10 <name> 89 5F 14 89 77 18 C7 07 <proxyVtbl>` | `impl` = the `__thiscall` member; `ebx` = the static **class descriptor**, loaded by the registering function's prologue `83 EC ?? 53 55 56 33 F6 57 BB <static>` |
| B (`GameScriptGame`) | `B8 <fn> 50 68 <name> E8 rel32 83 C4 0C 50 B9 <static> E8` | `fn` and the static descriptor |

The proxy vtable's RTTI name spells out the signature (`H`=int, `I`=unsigned, `K`=unsigned long,
`M`=float, `_N`=bool). **The statics are class descriptors, not instances** (`GameScriptFEARScore`
at `0x190B218`, `GameScriptGame` at `0x18762E8`; layout `{0, methodList, 0, "ClassName"}`, no
vtable). The proxy's Invoke (`0x783D20`) adds the member-pointer adjustor at `proxy+0x24` to an
instance pointer supplied by the script host, so the wrappers cannot be called through the
descriptors. The mod therefore only *reads* the wrappers for the constants they encode and calls
the managers directly:

| Wrapper | impl (this build) | What it encodes |
| --- | --- | --- |
| `AwardChallengeRequirement` | `0x4A9B80` | `IFearScoreMgr` = `[ctx+0x188]`; slot 17 `GetGlobalData()`, slot 21 on that = `GetNumChallenges()`, slot 37 = `AwardChallengeRequirement(player, count, challenge)` |
| `HasPlayerAchievedChallenge` | `0x50DC90` | slot 40 = `HasPlayerAchievedChallenge(player, challenge)` |
| `IsPauseMenuShowing` | `0x4CB820` | `MenuMgr` = `[ctx+0x238]`; slot 69 on the `+4` subobject = `[sub+0x60] != 0 && [sub+0x64] == 1` |
| `HasPlayerStartedLevel` | `0x616950` | called as is: uses no `this` (`GetPlayer(idx)` at `0x9EF2D0` walks the global player list, then `player->vtbl[40]()`) |

GCC has no `__thiscall`; calls are `__fastcall` with a dummy `edx` parameter, which has the same
register/stack shape and callee cleanup.

### Finding the managers
Both managers are created through the reflection factory (`CentralMaker`), so there is no global
pointer to them. The mod resolves each class's vtables from its RTTI name (`TypeDescriptor <-
COL <- vtable[-1]`; FearScoreMgr: `0x18EAA1C`/`+0x8 0x18EAA14`/`+0xC 0x18EAA08`; MenuMgr:
`0x1849FCC`/`+0x4 0x1849DF4`/`+0x10 0x1849DEC`/`+0x14 0x1849DE0`) and scans committed
non-image RW memory for the primary vtable pointer, accepting a hit only when every secondary
vtable sits at its offset. Measured: ~257 MB in ~50 ms; both exist already in the main menu. Reads
go through `ReadProcessMemory` so a vanishing page cannot fault the mod thread; the scan repeats
every 2 s until found and re-validates the cached pointers on use.

### The score manager (`FearScoreMgr`, vtable `0x18EAA1C`)
- slot 17 `GetGlobalData()` -> the `IFearScoreGlobalDataComponent` interface (stored at `mgr+0x18`).
- slot 37 `AwardChallengeRequirement(player, count, challenge)` (`0x682240`): gated on
  `[mgr+0x5C]->vtbl[47]()` true, `->vtbl[54]()` false and `[mgr+0xD4] == 0` (a scores-locked flag);
  looks up the per-player entry (`0xACD690`, keyed by the 64-bit player key from `0xD88DA0`),
  `rec = entry[0x54] + challenge*8` = `{int progress, bool achieved}`; if already achieved and not
  `isRepeatable` -> return; else `progress += count`, and if `0xC15AC0(challenge, old, new)` says the
  threshold (`requirementNumber`, record `+0x14`) was crossed, `0xE79140(key, challenge, 0)` fires:
  `mgr->vtbl[38](player, challenge)` (mark achieved) and a `ChallengeUpdateMessage` (HUD pop-up,
  points). **So one call with `count = requirementNumber` completes a challenge exactly like the
  game does, and a repeat is a no-op.**
- slot 40 `HasPlayerAchievedChallenge` (`0xAC4C60`): `entry[0x6C] + challenge*8 + 4` byte, gated on
  `[mgr+0xD4] == 0`.
- slot 39 `ResetChallengeProgress`: zeroes `entry[0x54] + challenge*8`.

### The challenge definitions (`FearScoreGlobalDataComponent`)
`GetGlobalData()` returns the interface subobject at `component+0x28` (vtable `0x18F5C8C`, 44
slots). Relative to that pointer:

| Offset | What | Accessor |
| --- | --- | --- |
| `+0x2C/+0x30` | `vector<BoostChallengeInfo>`, **156-byte** records, 78 of them | slot 21 count, 22 text id, 23 localised name, 24 internalName, 30 points, 32 requirement kind |
| `+0x20/+0x24` | `vector<ChallengeBoostCategoryInfo>`, **40-byte** records: `+0x08 internalName` (`std::string`: Aggressive, Tactical, Special, Exploration), `+0x24 textId` (from the reflection registrations at `0x12DA2B0`) | slot 11 count, 12 text id, 13 `std::wstring GetName(i)`, 14 `&internalName` |
| `+0x44/+0x48` | `vector<MissionScoreCategory>`, 44-byte records (Kills, Souls, ...; 17 of them) — *not* challenge categories | slots 36-39 |
| `+0x08/+0x0C`, `+0x14/+0x18` | rank tables (36/56-byte records) | slots 0-10 |

`BoostChallengeInfo` (reflection registrations at `0x12DA860` plus the accessors), verified against
the live table: `+0x00` name object `{?, ctx*, textId}` (`boostName`; text id duplicated at
`+0x08`), `+0x0C boostRequirementName` text id (the requirement wording shown in the panel's
tooltip; localised by copying the record's name object and swapping in this id), `+0x10
boostDescription` (0 for every record), **`+0x14 requirementNumber`** (50 for "The
Killer", 15 for "Sweet Science"), `+0x18` display-requirement number, `+0x1C` display increment
(-1 = none), **`+0x20 pointValue` (16-bit)**, `+0x24 internalName` (`std::string`, e.g.
`SadKiller` = Hard Boiled, `MeleeKills` = Sweet Science), `+0x40 boostCategoryInternalName`
(`Aggressive`/`Tactical`/`Special`/`Exploration`), `+0x5C` (empty), `+0x78 boostIconInternalName`
(`Aggressive Kills`, `Tactical Cover`, ...), `+0x94` requirement kind id (what slot 32 returns as
`max(1, x)`; **not** a count), `+0x98 isRepeatable` (the psychic-link ones), `+0x99 resetOnLoad`.
Indices 0-62 are the campaign set, 63-77 the Wade DLC ones.

Localised names are **`std::wstring`** (MSVC 2008 layout `{u32; union{wchar_t[8]; wchar_t*};
size; res}`, inline while `res < 8`). Slot 23 is `std::wstring GetName(unsigned i)` (hidden return
pointer first) and calls `0xE4A8B0(nameObj, out)`: `TextTable = 0x7787C0([[obj+4]+0x2C])->vtbl[43]();
wide = TextTable->vtbl[2](textId)`. Category names come from slot 13, which has the same shape. (Calling the
name helper on a guessed category name object crashed the game at startup — every game call needs
its layout confirmed from the registrations or the accessor bytes first.) Strings over 7 characters own a
buffer from the game's allocator; the table is read once per session and those bytes are left to
it. Every offset above is verified against the accessor bytes before the table is read
(`verify_table_layout` in `src/game.cpp`); a mismatch disables the reader and logs once.

Challenge names are **not** in the exe; they live in `resources/Boot.dsPack` (`"mgf "` container).

### Renderer capture
The game resolves `CreateDXGIFactory1`, `D3D11CreateDevice` and `Direct3DCreate9` through
`GetProcAddress` (D3D11 by default; D3D9 only with `-d3d9` in `options.cfg`, which is a command-line
override file). An import-table hook on the exe's `GetProcAddress` returns wrappers: from the DXGI
factory, `IDXGIFactory::CreateSwapChain` (slot 10) is vtable-hooked, then the swap chain's
`Present` (8) / `ResizeBuffers` (13); for D3D9, `IDirect3D9::CreateDevice` (16) then
`EndScene` (42) / `Reset` (16). Whichever API presents first becomes the active backend. Under
DXVK those vtables are shared per class. The DX11 ImGui backend compiles its shaders
through `d3dcompiler_43` — the same DLL the game imports, so it is always present.

The account of the Windows crash is in Fear3CabbyCodes' `CLAUDE.md` ("The panel's hooks on Windows"):
the Steam overlay (`gameoverlayrenderer.dll`) hooks each new swap chain by writing a jump into
whatever function each slot of its vtable points to at that moment and keeps one saved original per
hook; the game makes its swap chain twice at start, and with the mod's Present/ResizeBuffers in
DXGI's class vtable the second pass took the mod's functions for its originals - the two called
each other until the stack ran out (`0xC00000FD`) before the first frame. Hence, on Windows only
(`adopt::enabled()`: not Wine, or `F3CG_ADOPT` in the environment), each swap chain the factory makes for
**the game's window** (a window of this process made by the thread that loaded the mod) and each
D3D9 device gets a private copy of its vtable with the mod's hooks (`src/adopt.cpp`; 64 / 192
slots), and the hooks call on through the vtable the object had, as it is at the time; the classes'
vtables are left alone. The factory's `CreateSwapChain` and `IDirect3D9::CreateDevice` stay hooked
in place (the overlay skips a factory slot outside `dxgi.dll` and wraps IDirect3D9 in its own
object). Under Wine the class vtables are hooked as before. On a process exit DllMain does nothing:
1.1.0's teardown there never finished (the log's last line was always `unloading - removing hooks`,
under Proton too; under Wine the test process hung there until killed).
`tests/test_adopt.cpp` plays the overlay: 1.1.0 loops (`LOOP`, D3D11 and D3D9) and hangs on exit;
the fix passes, and the Wine path still takes the class hook. Not yet seen in game on Windows.

### Pause / in-level gating and grant timing
`show_panel = HasPlayerStartedLevel(player) && IsPauseMenuShowing()`, evaluated every ~16 ms on the
primary thread from the `PeekMessageA` hook (the game renders and pumps messages on that same
thread, so ImGui, the WndProc subclass and the manager calls all share it). On the paused -> unpaused edge with a non-empty queue the
grants fire after `GrantDelayMs` (default 500), each followed by a re-query so the log says whether
the game accepted it. Leaving the level (`HasPlayerStartedLevel` false) drops the queue.

## Tooling notes
- `tools/find_regs.py` reproduces every address in this file from the exe (registrations, RTTI
  vtables, string cross-references). Pure stdlib + `i686-w64-mingw32-objdump`.
- Ghidra headless (`/opt/ghidra/support/analyzeHeadless`, `MAXMEM=12G`) analyses the exe in about
  12 minutes. Several small functions (the interface getters, `IsPauseMenuShowing`'s body) are not
  auto-defined; create them from the preceding `int3` padding before decompiling.
- The RTTI is complete (`.?AV...@Despair@@` names everywhere); walking `vtable[-1] -> COL ->
  TypeDescriptor` is the reliable way to both find vtables offline and verify objects at runtime.
- Write byte patterns from `objdump` **raw bytes**, never from the mnemonic text: the SIB byte of
  `lea eax,[ecx+eax+0xc]` is `01`, of `[eax+ecx+0x8]` it is `08`. One transcribed pattern cost a
  restart cycle.
- The gamescope wrapper may relaunch the game up to four times at startup (compositor race, exit
  code 134 after ~6 s); each attempt truncates the log, so read the last block.
