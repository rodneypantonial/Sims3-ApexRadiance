# S0-08 follow-up: light capture time budget

- Status: done (offline); in-game check open (the recorded lighting scenes job).
- Branch: `fix/lightprobe-capture-budget` (from `gi/s0-08-shotfix`).
- Commits: see `git log fix/lightprobe-capture-budget` (`probe: cap the time a light capture holds a frame`).
- Deliverable: `features/light_probe_budget.h`, the budget in `features/light_probe.cpp`,
  `tools/light_probe_budget_test`, the "Time budget" section of `docs/features/dev-tools/light-probe.md`.

## Problem

In the recorded lighting scenes job (S0-08X, through the RTX Remix bridge) each light capture held the game's frame:
28 s, 60 s and 39 s from `Capture requested` to `Measured` (verified in the run's log). The held frame also stopped a
screenshot burst (fixed separately: `captures: a stalled frame does not stop a screenshot burst`).

## Cause

- (verified, code) The whole capture ran inside two frames: in the recorded frame, every screen-sized draw (389-484 in
  that run) waited up to 50 ms for its own occlusion answer with `D3DGETDATA_FLUSH`, and up to 48 draws read back up to
  24 probed values each (1 x 1 `GetRenderTargetData`); Night Lighting turned its lamp memos off and wrote its `mod:`
  detail for every draw while `Capturing()`. At the next Present the queries were polled with `Sleep(1)` for up to
  1.5 s and every texture was dumped in the same frame.
- (inferred) Through the bridge each wait and read-back is a cross-process round trip, so the recorded frame's per-draw
  work is what took most of the 28-60 s: `Measured` follows the end of the held frame by about 1.4 s, the length of
  the finishing work. No per-phase timing existed to prove the split; the new `Capture time` log line gives it.

## Fix

- The recorded frame has a 250 ms budget for the optional per-draw work (coverage wait, probed values, instance data,
  wall geometry, `mod:` detail). Past it the draws are still recorded with their state, constants and occlusion query,
  and `Capturing()` turns false so other modules stop their detail work.
- A recorded frame past 3 s records no more draws and is abandoned at its Present: `[LightProbe] Capture abandoned:
  ...` (warning), nothing saved, the frame let go.
- The finishing work runs over the following frames: occlusion answers polled once a frame (1.5 s), then the textures
  at about 8 ms a frame (at least one); past 20 s the remaining textures are listed as not saved. `Busy()` stays true
  until the files are written, so capture sessions and screenshot bursts still wait for them.
- New log line after `Measured`: `[LightProbe] Capture time: recording frame N ms (K draws past its 250 ms budget ...),
  then F frames over M ms (...)`.

## Tests

- `tools/light_probe_budget_test/run.ps1`: 15 checks, 0 failures (simulated 433-draw frame of a slow device: 381 ms
  instead of about 42 s; stuck frame dropped at the deadline; textures spread over frames).
- Developer and Public builds of the branch.

## What the scenes rerun must check

1. Each `[LightProbe] Capture requested` is followed by `Measured` within a few seconds, and the `Capture time` line
   shows a recording frame of at most about 300-400 ms.
2. No `Capture abandoned` line (if one appears, the plain recording itself is too slow through the bridge: lower
   `kMaxDraws` or record fewer constants next).
3. No `Screenshot burst stopped: it took too long`; the filtered screenshot PNG is in the job's `captures` folder.
4. The capture folders (`Light capture mouse`, `Light capture cube`) hold `Light capture.txt` and their textures; the
   draws listed past the budget say `not described: past the capture's time budget`.

## Open issues

- Coverage of the pixel's own draw: with most draws past the budget, the probed values and `mod:` detail cover only
  the first draws of the frame. If the cube pixel needs them, a later change can skip the extras for draws that do not
  touch the pixel instead (needs the answer without a wait, for example from the previous frame's queries).

## Unblocks

The gi-scenes rerun (S0-08X recorded scenes) without held frames.
