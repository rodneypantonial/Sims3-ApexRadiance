# Paused after an Apex Launcher start: diagnostics

A save loaded after a Play in Apex Launcher on the EA app copy (1.69) can stay paused: the world is live (WorldManager
mode 1, active 1, never mode 2) and the speed keys reach the game window, but game time does not move. The same save
started from the EA app unpauses. A start of the same shape on a test machine without other mods unpauses, so the cause
is a launcher action together with the setup it meets (many script mods, S3SS, S3PP, the RTX Remix bridge). The
diagnostics build (`framework/start_diag.cpp`, build `diag-1127c`) logs one fact per candidate below and a verdict line.

## What a Play does that an EA app start does not

Status: verified in the launcher source, unless marked.

### Before the start

| Step | Files | Evidence |
|---|---|---|
| Safe fixes, asked first or made at once with "Fix without asking": a second Apex or ASI copy moved aside, a second ASI loader moved aside, `Mods\Resource.cfg` made or given the Packages lines, S3SS uncompressed textures off under the bridge, `bridge.conf` target, `dxvk.conf` keys | Game\Bin ASIs and loaders, `Resource.cfg`, `S3SS.toml`, `bridge.conf`, `dxvk.conf` | `installer/ApexSetup/MainWindow.Launcher.cs:238-249`, `installer/ApexSetup.Core/Compat.cs:35-170` |
| The full Health check starts in the background and runs while the game loads: merge recovery, save-change recovery (both skip once the game runs), the mod list and the mod conflict check, which read every package | reads `Mods`, `Saves` | `MainWindow.Launcher.cs:63-85`, `:250`; `ApexSetup.Core/ModMerge.cs:689-694`; `ApexSetup.Core/SaveCopies.cs:347-350`; `ApexSetup.Core/ModConflicts.cs:184` |
| Save backups due at Play; "Play without waiting" lets the copy run on into the start | reads `Saves`, writes the backup folder | `MainWindow.SaveBackups.cs:672-700` |
| Game options chosen before the game made `Options.ini`, once: in-game store, interactive loading, memories, Connect notifications and `requireloginbeforeload` | `Options.ini` | `MainWindow.GameSettings.cs:453-461`, `ApexSetup.Core/GameSettings.cs:46-51`, `:117-123` |
| The menu theme into `[ui]` | `ApexRadiance.toml` | `MainWindow.Launcher.cs:276`, `ApexSetup.Core/Engine.cs:995-1008` |
| No S3SS preset: the recommended S3SS settings (patch list, optional Smooth Patch at 480 ticks) are written only by the install step or the Recommended settings dialog | `S3SS.toml` | `ApexSetup.Core/SetterSettings.cs:10-40`, `MainWindow.GameSettings.cs:310-320`, `:505-516` |
| No mod order or merge change, no cache clearing at Play | | `StartGameAsync`, `MainWindow.Launcher.cs:271-311` |

### The start

- The EA app copy with Apex Radiance 4.1.6 or later starts through the EA app's link
  (`origin2://game/launch?...&cmdParams=-editInGameMode:true -apexDirect`), not as a child of the launcher
  (`ApexSetup.Core/Launch.cs:20-31`, `:54`, `:67-79`). The process, its environment, job and integrity therefore come
  from the EA app and the Sims 3 launcher, as on an EA app start. Only the Steam path sets `SteamAppId`
  (`Launch.cs:55-57`).
- `-editInGameMode:true` makes the Sims 3 launcher start the game at once and pass every argument on, so its own Play
  is skipped, with whatever that Play writes or sets up (the pack selection in `LoadOption.ini` among it). The game
  receives `-apexNoEditMode:true -apexDirect -test` after Apex Radiance renamed the switch in `GetCommandLineW` and
  `GetCommandLineA` (`framework/edit_in_game_arg.h`, `apex_main.cpp` DllMain).
- What the Sims 3 launcher's own Play passes to the game and what `-test` does in the game: unverified.

### While the game runs

- Session watch: the process is refreshed and its private bytes read every 5 s; no input, window, focus, priority,
  affinity or IPC (`Launch.cs:105-128`). A search of the launcher for window messages, priority, affinity, pipes and
  shared memory finds none.
- "Hide while playing" (on by default) hides the launcher window and shows a tray icon (`MainWindow.Launcher.cs:285`,
  `ApexSetup.Core/LauncherSettings.cs:12`); the tray takes the foreground only while its menu is open (`Tray.cs:69`).
- Automatic backups stop while the game runs (`MainWindow.SaveBackups.cs:663`).
- The restart host acts only after the game quits (`features/restart_host.h:1-5`, `native/ts3wrap/ts3wrap.cpp:1-8`).
- The launcher log is `%LOCALAPPDATA%\Apex Launcher\ApexLauncher.log`, its settings `settings.json` in the same
  folder (`installer/ApexSetup/App.xaml.cs:10-11`, `ApexSetup.Core/Paths.cs:12`, `LauncherSettings.cs:29`).

### Other mods that can stop game time (public sources)

- S3SS Smooth Patch (Precise), `[patches.SmoothPatchPrecise]`: replaces the idle simulation cycle and returns the
  previous cycle time; with "tick at most once per frame" it waits for a presented frame (1 s fallback). The likeliest
  S3SS feature to hold time. Smooth Patch (Classic) rewrites sleep calls only. Lot Streaming Optimizations defers
  object building (a stuck lot, not a stopped clock). The GC patches, Adaptive Wait and Timer Optimization do not stop
  time. The memory monitor's modal may capture input. Source: Sims3SettingsSetter at `5eb2c65`.
- S3PP (Sims3Performance.ini): ShortWaitSpin shortens short waits; no tick, frame, focus or speed feature is known.
  Its full key list is unverified.

## Candidate causes, ranked

File changes persist, so they reach a later EA app start too; differences that exist only for the launcher's start rank
first.

1. **The start path** (session only). The Sims 3 launcher's Play is skipped and the game gets the renamed switch,
   `-apexDirect` and `-test`. A reader that copied the command line before DllMain renamed it (an early CRT or script
   copy) would still see edit-in-game mode, which stops time, while WorldManager reports mode 1. Inferred.
2. **The script/modal pause bit (GameClock `+0x40` bit `0x1`) held by a script.** A ScriptError from a script mod was
   written during the world load in the reported run; a script that fails while a modal or its pause is up would leave
   the bit set. Inferred; why only the launcher start, unverified.
3. **The launcher's background work during the load**: the Health check reads every package and a Play backup may still
   copy saves while the game loads. It changes load timing, not files. Inferred.
4. **Files written just before the start**: the safe fixes, `Options.ini` pending options, `ApexRadiance.toml` theme.
   Low: they persist.
5. **S3SS or S3PP time settings.** Low as a launcher-only cause: the launcher does not change them at Play.
6. **Focus and window state** from "Hide while playing". Low: the keys reached the game window.
7. **Process environment, job, integrity.** Low: the EA app starts the game in both cases.

## What the build logs per cause

| Cause | Lines |
|---|---|
| 1 | `[Diag] Command line`, `[Diag] Command-line switch copies in private memory` (copies of `-editInGameMode:` with an on value), `[Diag] LoadOption.ini` |
| 2 | `[Diag] Pause bit 0x1 set/cleared at +N s` (from the first clock sample, the load included; world mode, OS windows and dialogs, error files so far), `[Diag] New error file ... pause bits now`, key lines with the level before and after |
| 3 | `[Diag] Footprint: Mods packages written ...`, `[Diag] Apex Launcher log, last entries`, `[Diag] Saves folder` |
| 4 | `[Diag] Footprint` (last write of each file against the game's start, `[RECENT]` within 10 min before), `[Diag] Options.ini ... keys` |
| 5 | `[Diag] S3SS ... keys`, `[Diag] Apex ... keys`, `[Diag] Sims3Performance ... keys` |
| 6 | the watch lines (game window in front, minimized), bits `0x2` and `0x4`, `[Diag] Apex Launcher settings` |
| 7 | `[Diag] Process` (working folder, integrity, elevation, priority, affinity, job and its limits, start flags, power throttling), `[Diag] Environment` |
| all | `[Diag] Verdict: ...` 2 min after the watch starts and `[Diag] End Verdict: ...` at its end |

Not logged: which code sets the pause bit. Finding the setter needs a write watchpoint or a hook on it, which is neither
cheap nor free of side effects.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `tools/start_diag_test/` | masking (OneDrive included), footprint timing, config keys, launcher settings and log, pause bit tracking, switch copies, the verdict | `run.ps1` | none |

## Open checks

- A player run with `diag-1127c` on the affected setup.
- What the Sims 3 launcher's Play passes and writes; what `-test` does in the game.
