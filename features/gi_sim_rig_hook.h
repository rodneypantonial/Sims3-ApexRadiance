#pragma once
// A rig per Sim draw from the skin the game just used (real-time GI work package XR-10, part 4 of wp/XR-10.md).
// [patches.RealtimeGI] pixelSimRigs (developer builds, default off, with pixelShadows). The two CALLs to the skin fill
// 0x0068F3F0 (0x006905B4, 0x0070A7D4; Steam 1.67.2 only, each checked for its CALL first, else off) go through a wrapper
// that, after the game's fill, reads the slots' joint name hashes and bind poses (SEH-guarded) and keeps the rig of that
// draw in a 64-entry ring (gi_sim_capsules.h SkinRing). The per-pixel pass finds a draw's rig by the palette it uploads.
// Rendering only: shadow shapes of every Sim, no simulation state.
#include "gi_sim_capsules.h"
#include <cstdint>
#include <string>

namespace GiSimRigHook {

bool Start(std::string* why); // false (why set): left off, the fixed column stays
void Stop();
bool Running();
// The rig of the newest fill whose palette is `regs` registers from c0 and starts with c0 (rows c0..c2)
bool Find(int regs, const float c0[12], GiSimCapsules::SkinRig& out);

struct Counters {
    unsigned long long fills = 0, rigs = 0, faults = 0; // fills seen, rigs kept (a named pair), reads that faulted or failed a bound
};
Counters Read();

} // namespace GiSimRigHook
