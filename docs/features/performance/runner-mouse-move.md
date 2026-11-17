# Runner: one mouse-move step

The in-game runner had two steps that post a mouse move: `mouse` (a list of `points`, on `feature/async-readback`) and
`mouse_move` (one point, on `feature/pick-skip-when-still`). Only `mouse_move` stays.

This branch drops `mouse` and carries the `mouse_move` code of the pick-skip branch as it is: the step, `Resolve-StepPoint`,
`Get-ClickLParam`, `Get-ScreenPoint`, the `GetClientRect` import and the two selftest checks. `target` (screenpos) in
`mouse_move` needs the agent's `screenpos` command from the pick-skip branch; `x`/`y` and `xf`/`yf` work without it.

A job that moved the mouse over 12 points with `{ "do": "mouse", "points": [...], "delay_ms": 60 }` becomes, per point,
`{ "do": "mouse_move", "x": .., "y": .. }` followed by `{ "do": "wait", "minutes": 0.001 }` (about 60 ms).

## Swapping the branch in

1. Take `feature/async-readback-mousemove` in place of `feature/async-readback` wherever a run or PR uses it (it is that
   branch plus this one commit).
2. Run the jobs that used `mouse` in their `-mm` form (the async pick job is `async-pick-1117-mm`).
3. When both PRs are in, merging the second one conflicts only in `run.ps1` and `runner_lib.ps1` around the shared
   functions: keep the pick-skip side (it is a superset).
