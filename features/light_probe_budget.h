#pragma once
// Time budget of one light capture (features/light_probe.cpp). Through the RTX Remix bridge every occlusion query wait
// and 1 x 1 read-back of the recorded frame is a round trip, so the recorded frame could be held for 15-60 s. The
// optional per-draw work stops at a budget, the slow finishing work runs over the following frames with a cap per
// frame, and a recording frame that still runs past its deadline is abandoned. Pure functions: the offline harness
// tools/light_probe_budget_test compiles this header.
#include <algorithm>

namespace LightProbeBudget {

constexpr double kExtrasMs = 250;          // recording frame: coverage waits, probed values, wall geometry, Night Lighting detail
constexpr double kCoverageWaitMs = 50;     // longest wait for one draw's coverage answer while the budget lasts
constexpr double kRecordDeadlineMs = 3000; // recording frame: past this the capture is abandoned
constexpr double kResolveMs = 1500;        // occlusion answers, polled once a frame after the recording frame
constexpr double kSaveFrameMs = 8;         // texture files written per frame (at least one a frame)
constexpr double kFinishDeadlineMs = 20000; // from the recording frame's end: textures not written by then are left out

// the optional work of a draw recorded `recordMs` into the recording frame
inline bool ExtrasAllowed(double recordMs) { return recordMs < kExtrasMs; }
inline double CoverageWaitMs(double recordMs) { return std::clamp(kExtrasMs - recordMs, 0.0, kCoverageWaitMs); }
inline bool PastRecordDeadline(double recordMs) { return recordMs >= kRecordDeadlineMs; }
// occlusion answers still missing `resolveMs` after the recording frame: keep polling next frame
inline bool KeepResolving(double resolveMs, bool pending) { return pending && resolveMs < kResolveMs; }
// one more texture this frame, after `written` of them took `frameMs`
inline bool SaveMoreThisFrame(int written, double frameMs) { return written == 0 || frameMs < kSaveFrameMs; }
inline bool PastFinishDeadline(double finishMs) { return finishMs >= kFinishDeadlineMs; }

} // namespace LightProbeBudget
