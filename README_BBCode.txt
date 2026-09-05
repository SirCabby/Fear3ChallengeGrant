[size=6][b]Fear3ChallengeGrant[/b][/size]

An in-game mod for F.E.A.R. 3 that adds a [b]panel listing every campaign challenge[/b] to the pause menu - Sweet Science, Hard Boiled, Mommy!, all 78 of them - and lets you [b]grant any of them[/b] with a click.

[b]Pause the game during a mission and the panel is there[/b] - nothing to set up, nothing to press. Click Grant on a challenge, unpause, and the game awards it exactly as if you had earned it: the same pop-up, the same score, the same statistics.

This mod is open source! Check it out at [url=https://github.com/SirCabby/Fear3ChallengeGrant]https://github.com/SirCabby/Fear3ChallengeGrant[/url]

[size=5][b]What it does[/b][/size]
[list]
[*]A panel beside the pause menu with [b]every challenge[/b], its category, point value and whether it has already been achieved this mission. Hover a row for the game's own description of what the challenge requires.
[*][b]Grant[/b] any challenge that is still available. It is awarded the moment you unpause, through the game's own scoring code.
[*][b]Grant all[/b] queues every challenge still available in one click - or, with a filter typed, just the listed ones.
[*]Queued clicks can be [b]cancelled[/b] until you unpause. Reloading a checkpoint or leaving the level discards the queue.
[*][b]Filter[/b] box, [b]sortable[/b] columns (click a header), resizable and reorderable columns - the layout is remembered.
[*][b]F8[/b] hides or shows the panel while paused.
[/list]

[size=5][b]Install[/b][/size]

Copy into your F.E.A.R. 3 folder (the one with F.E.A.R. 3.exe):
[list=1]
[*]Rename the stock [b]binkw32.dll[/b] to [b]binkw32_orig.dll[/b]
[*]Drop this mod's [b]binkw32.dll[/b] in its place
[/list]

That is the whole install, on [b]Windows and Linux/Proton alike[/b] - no launch options, no WINEDLLOVERRIDES, no ASI loader. Direct3D 11 (the default) and Direct3D 9 (-d3d9 in options.cfg) both work.

To uninstall: delete binkw32.dll and rename binkw32_orig.dll back.

[size=5][b]Please read before using[/b][/size]
[list]
[*][b]A challenge can only be granted once per mission[/b], exactly as in vanilla. The game tracks that itself; the panel shows those rows as Awarded and locks them until you reload a checkpoint or start a level.
[*][b]Grants are real.[/b] They go through the game's own award path, so they count toward mission score, lifetime statistics and the challenge-related Steam achievements just like earned ones. Nothing is written to your saves by the mod itself.
[*][b]Steam's "Verify integrity of game files" removes the mod[/b] by restoring the stock DLL. Just re-copy the file.
[/list]

[size=5][b]Config[/b][/size]

[code]
ToggleKey    = 0x77   ; virtual-key code that hides/shows the panel while paused (0x77 = F8)
PlayerIndex  = 0      ; local player slot that receives the challenges (0 in single player)
GrantDelayMs = 500    ; delay after unpausing before queued challenges are granted
[/code]

Lives in [b]Fear3ChallengeGrant.ini[/b] next to F.E.A.R. 3.exe (created on first run). [b]Fear3ChallengeGrant.log[/b] beside it records what the mod found and did - attach it when reporting a problem.
