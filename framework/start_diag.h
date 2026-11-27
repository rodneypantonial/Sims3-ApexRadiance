#pragma once
// Start diagnostics of the diagnostics build ([Diag] lines, read-only): how the game was started, which modules and
// mods are loaded, the save, the pack and option state and, for 10 minutes after the world settled, the world mode,
// focus, windows and the player's speed keys. Written to answer why a save opened straight from a launcher stays paused.
// On by default in this build; a file diag_off.txt in the Apex folder turns it off.
#include <windows.h>
#include "edit_in_game_arg.h"
#include <string>

namespace StartDiag {
void Start(const EditInGameArg::Scan& scan, const std::string& parent, const std::string& grandparent, DWORD grandparentPid); // init thread, once the log is open: start lines, then a background thread
void Pump();                                 // start-up pump thread (10 ms)
void NoteWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp); // game window thread, every message, no IO
} // namespace StartDiag
