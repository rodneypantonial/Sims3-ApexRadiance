# Texture compositor queue: static RE for async read-back (WP4, option G)

Static reading of the texture compositor queue loop `FUN_00608270` and the texture builder state machine, to decide
whether the queue can keep several tiles in flight while their read-backs run asynchronously in the bridge
(research doc 11, §4 option G and §5.3).

- Binary: TS3W.exe, Steam 1.67, 14,510,400 bytes, SHA-256 `10b2423e2368e161774462b0ef1960231ad0af298d564a7bb6a6e78c5e880609`.
- Method: linear disassembly of the listed addresses (capstone). No game run.
- Tags: **[verified]** = read in the disassembly at the given address; **[inferred]** = reasoned from it, not proven.
- Names follow `features/compositor_readback.cpp` and the game_addresses table where they exist; the rest are ours.

## 1. Queue layout

| Item | Finding | Tag |
|---|---|---|
| Owner | One global queue object, pointer at `0x11CEB88` (getter `0x6079D0`); created by the queue init `0x608510`, freed at `0x6086BC` | verified |
| Container | Segmented double-ended queue (deque-like): `+0x00` block map, `+0x08` first record, `+0x0C`/`+0x10` first block begin/end, `+0x14` first map slot, `+0x18` end record, `+0x1C..+0x24` last block. Block = 0x360 bytes = 4 records of 0xD8 | verified (`0x6080E0`, `0x608010`) |
| Record (0xD8) | `+0x00` builder pointer (ref-counted: vtable `+4` add-ref on insert `0x60977A`, `+8` release on pop `0x608103`); `+0x04` per-record job (0xCC bytes, ctor `0x5FC8D0`, dtor `0x5FC970`); `+0xD0` priority (int); `+0xD4` started flag (byte) | verified |
| Head | Always `[queue+8]`; the loop never looks past it | verified (`0x6082C0`) |
| Add | Builder method `0x5FD3E0` (7 stack args, `ret 0x1C`): refuses if builder `+0x98` (queued) is set, sets it, calls enqueue `0x609760`. Enqueue finds the position with a binary search `0x607E10` (upper bound) and inserts with `0x6094F0` | verified |
| Order | Comparator in `0x607E10`: a started record goes before an unstarted one; otherwise higher `+0xD0` first; equal keys keep FIFO (upper bound). So the queue is sorted: started prefix, then priority descending | verified (`0x607EA3..0x607EBB`) |
| Remove head | `0x6080E0`: advance `+8` (free the block when it empties), destroy the job, release the builder. Only ever pops the head | verified |
| Remove any | `0x609850` walks the deque and erases the record whose builder matches (`0x609600`). Callers: builder cancel `0x5FBE20` (if `+0x98` set; called from the material compositor `0x6CD66F`, `0x6CDF37`) and the synchronous build `0x5FE1A0` | verified |
| Flush head | `0x608220`: if the head is started, run it to the end synchronously (`0x5FE0D0`), then pop it (or reset it on result 3). Called only by the synchronous build `0x5FE1A0` (`0x5FE1DC`) before it renders | verified |

Builder fields used (builder object, ctor `0x5FCA20`):

| Offset | Meaning | Tag |
|---|---|---|
| `+0x0C` / `+0x10` / `+0x14` | Render-target **ids**, set to constants `0x21E9CD2/CD4/CD3` (or `0x21E9CD6/CD8/CD7`) in `0x5FDA00` (`0x5FDBCB`, `0x5FDAC5`). Resolved through the RT manager (`0x616FC0` -> `0x617500`). **Every builder uses the same named render targets** | verified |
| `+0x1C` | Output texture (per builder, released in the dtor `0x5FDE96`) | verified |
| `+0x20` | System-memory staging texture, per builder (alloc `0x5FDB68`, 256 wide per `0x5FDB96`; released in the dtor `0x5FDEA4`). Null = GPU-only path | verified |
| `+0x24..+0x28` | Op list (layers) | verified (`0x5FC130`) |
| `+0x60..+0x64` | Tile array, 0x60 bytes per tile | verified |
| `+0x70` | Current tile index | verified |
| `+0x74` | State (0..8) | verified |
| `+0x84` | Read-back format id (argument to `0x5FBCE0`) | verified (`0x5FDC81`) |
| `+0x88` | Device generation; a change resets the builder (`0x5FDF0D`) | verified |
| `+0x94` / `+0x95` | Done / succeeded | verified |
| `+0x96` | Valid (checked before start, `0x5F7490`) | verified |
| `+0x98` | Queued | verified |

No thumbnail flag lives on the builder or the record. Priority is the only ordering key.

## 2. Can the loop step a builder other than the head?

Not with the game's loop as is: it reads `[queue+8]` on every iteration and every result it handles acts on the head
(pop = head pop). A hook at `0x6082F8` can step other records itself, under these rules:

| Risk | Finding | Tag |
|---|---|---|
| Pop assumes head | Results 1 and 4 at `0x608307..0x608313` pop the **head**. A hook that stepped another builder must never return 1 or 4 to the loop; return 0 (continue) or 2 (stop) | verified |
| Finished non-head record | It stays in the deque. When it becomes the head, the loop sees done (`0x5F74B0`, builder `+0x94`) and pops it (`0x6082E9`); an unstarted done record is started, refused with 4 (`0x5FCAD5..0x5FCB03`) and popped | verified |
| Started prefix | Insert (upper bound) assumes started records are a prefix. Start non-head records only in queue order (start record i+1 only when 0..i are started), or later inserts land in the wrong place (wrong order, no crash) | verified (comparator), effect inferred |
| Iterator invalidation | Inserts and erases happen only through `0x609760` / `0x609850`; no state function of `0x5FDEF0` calls them. Inside one `0x608270` call the deque is stable; across frames records move. Re-walk the deque from `[queue+8]` every hook call; never keep record pointers across calls, key per-builder state by the builder pointer | verified (call scan), thread claim inferred (docs: main thread) |
| Builder lifetime | The record holds a reference, so a builder in the deque is alive. A builder no longer found in the deque may be gone: drop its pending entry | verified (add-ref/release), lifetime rule inferred |
| Shared render target | All builders render into the same named RT (`+0x0C` ids above) and `0x5FBCE0` copies from it. A **later re-copy** from that RT after another builder rendered reads the wrong tile. An async read-back issued at `0x5FDC8A` is queued in device-command order before the next builder's draws, so the copy itself is safe; the danger is only a fallback that re-reads the RT | verified (ids), ordering inferred (bridge keeps command order) |
| Destination | Staging texture `+0x20` and output `+0x1C` are per builder; job lock info is per record (`job+0x0C` staging lock, `job+0x34` output lock) | verified (`0x5FDCB2`, `0x5FDCDA`) |
| Locks on erase | The job dtor `0x5FC970` only frees buffers, it does **not** unlock. Erasing a record between its state-2 lock and state-4 unlock leaves `+0x1C`/`+0x20` locked. Natively only the head can be mid-tile (and `0x608220` flushes it first); with G a non-head must never be left between state 3 and state 7 when the hook returns | verified (dtor), consequence inferred |

## 3. Builder state machine (`0x5FDEF0`, jump table `0x5FE0A8`)

Entry checks every call: state 8 returns 1; device generation changed or device lost flag (`[0x11CECC8]+0x8D`) resets
via `0x5FC680` and returns 3.

| State | Handler (call site) | Work | Next | Tag |
|---|---|---|---|---|
| 0 | `0x5FC9D0` (`0x5FDF75`) | Setup: store device generation, check op resources (`0x5FC130`), tile = 0; state = 1 if no staging texture, else 2. Non-zero non-3 result marks done/failed | 1 or 2 | verified |
| 1 | `0x5FDD20` (`0x5FDF99`) | GPU-only path: render the tile (`0x5FC170`), copy RT -> output with `Device_CopyRenderTarget 0x610660` (no CPU lock), finish (`0x5FBE50`); tile++; repeats until all tiles | 1, then 8 | verified |
| 2 | `0x5FDDE0` (**`0x5FDFEC`**) | Render the tile into the shared RT (`0x5FC170`), then `0x5FDBF0`: fill job rects, **read back** RT -> staging (`0x5FBCE0` at **`0x5FDC8A`**), lock staging and output (`0x618DF0` x2) | 3 | verified |
| 3 | `0x5FD420` (**`0x5FDFFF`**) | CPU work on the locked tile (copy/convert staging -> output by format) | 4 | verified (call), meaning inferred |
| 4 | `0x5FBE50` (`0x5FE015`) | Unlock both (`0x619160` x2) and tidy | 5 | verified |
| 5 | `0x5FD210` (`0x5FE02B`) | Per-tile post step (op `0x5FD180`) | 6 | verified (call), meaning open |
| 6 | `0x5FBEF0` (`0x5FE041`) | Per-tile post step | 7 | verified (call), meaning open |
| 7 | `0x5FD820` (`0x5FE057`) | Last-tile step (mip/border work on the job), tile++ | 2, or 8 when no tiles left | verified |
| 8 | - | Done: `+0x95` = 1, `+0x94` = 1 | - | verified (`0x5FDFD0`) |

Step results: 0 = more to do, 1 = done, 3 = reset (device), 4 = failed (sets `+0x94`=1, `+0x95`=0 at `0x5FE08F`).
Natively no state function returns a literal 2; a 2 can only reach the loop from setup (an op resource check value,
`0x5FC15B`), and Apex's deferral returns 2 from `0x5FDC8A` so that state 2 ends with state = 3 and the loop stops
[verified: no `mov eax/edi, 2` in the state handlers].

With the current deferral, the read-back leaves the builder in **state 3 with nothing locked** (the locks in
`0x5FDBF0` come after the read-back and are skipped); Apex's state-3 hook re-runs `0x5FDBF0` with the copy skipped to
take the locks, then lets `0x5FD420` run [verified in `features/compositor_readback.cpp` and `0x5FDC8F..0x5FDCDA`].

The state-1 path also reads back through `0x610660` when its target needs it; option G does not cover it [inferred].

## 4. Priorities and budget

| Item | Finding | Tag |
|---|---|---|
| Sim (CAS) textures | Enqueued with priority 10: `0x5E09F4` and `0x5EBD11` (the CAS sim compositor request, docs: `CasSim_StepProcessCompositorRequest 0x5EBC50`) | verified |
| World object textures | Material compositor enqueues with priority 0 (`0x6CD491`) | verified |
| Thumbnails | Not queued: the object designer service builds synchronously with `0x5FE1A0` (`0xB3AAC6`); so do `0x5E0A69`, `0x5EBD4F`, `0x6CD912` in their sync branches. A sync build first flushes the queue head (`0x608220`) | verified |
| Sorting | Insert-time only (§1): started first, then priority descending, FIFO within a priority | verified |
| Budget | `0x608270` takes one budget argument; the caller `0x608630` passes the queue's `+0x18`, or -1 (no limit) when byte `+0x15` is set. A stopwatch (`0x4F35B0`/`0x4F34F0`, QPC via `[0xF953B8]`) sets the deadline; it is checked after every handled step (`0x608318..0x60833F`), so a frame does as many steps as fit, head only | verified |
| Budget value | Registered through Timeslicing (repo docs report 10000 at `0x608510`; unit not checked here); `+0x15` likely tracks the generate-thumbnails setting | inferred |

## 5. What returning 2 at `0x6082F8` does

- Caller of `0x608270`: only `TextureCompositorQueue_Update 0x608630` (`0x60864E`, `0x608661`), which runs after the
  material compositor `0x6CE0D0`, once per update of the CAS/TextureCompositor timeslice task on the main thread
  [verified calls; per-frame cadence from repo docs, inferred here].
- `0x608270` returns nothing useful (`ret 4`; `0x608630` ignores eax). At the end it reports busy state to
  `CasService_UpdateBusyState 0x5BDF10` (category 0, busy = queue not empty, elapsed time) [verified `0x608345..0x60837D`].
- Step result at `0x6082FD`: **2 or 3 = stop the loop for this update** (no pop, rest of the budget unused);
  1 = continue (the head is popped on the next iteration via its done flag); 4 = pop the head now; 0 = continue with the
  same head [verified]. Start result at `0x6082D5`: 0 = mark started, 3 = stop, anything else = pop [verified].

So today's deferral (2 while the head waits) ends the whole queue for the frame: one tile per frame (§9 failure in
doc 09) [verified mechanism].

## 6. Does completion order matter?

| Question | Finding | Tag |
|---|---|---|
| Completion callbacks from the queue | None: the loop only pops; no notify call on pop besides the builder release | verified (`0x6080E0`) |
| How requesters learn completion | They poll their own builder's done flag (`0x5E0A2D` after enqueue; the CAS sim and material compositor state machines) | verified (call sites), full poll paths inferred |
| Cross-builder dependencies | None found in the queue: no builder waits on another; priority decides who starts first | inferred |
| CAS order | A Sim's textures are separate builders at the same priority (10), FIFO. Finishing out of order inside a Sim changes when each texture appears, not the result | inferred |
| Busy signal | `0x5BDF10` goes idle only when the queue is empty, so in-flight non-head builders keep CAS "busy" correctly as long as their records stay in the deque | verified (busy = not empty) |

## 7. WP4 hook plan (option G)

Patch sites (all 5-byte `call rel32`, already owned by Apex's CallChain; no overlap with ts3native, S3SS or S3PP per
the repo's hook-site check):

| Site | Target | Use in G |
|---|---|---|
| `0x5FDC8A` | `0x5FBCE0` (read-back) | In state 2 called from the queue: issue `RemixBridge_ReadRenderTargetAsync(sharedRT, staging[+0x20])`, record a ticket for this builder, return 2 (state 3, no locks). Without the export: today's event-query path |
| `0x5FDFEC` | `0x5FDDE0` (state 2) | Unchanged: marks "inside the queue's state 2" so sync builds pass through |
| `0x5FDFFF` | `0x5FD420` (state 3) | If the builder has a ticket: poll; ready -> re-run `0x5FDBF0` with the copy skipped (takes the locks), then the game's state 3. Not ready inside a sync path (`0x5FE0D0`/`0x608220`) -> wait on the ticket with a timeout. Lost/failed ticket -> **set state 2 (same tile) and return 0** so the tile is re-rendered and read back synchronously; never re-read the shared RT |
| `0x6082F8` | `0x5FDEF0` (queue step) | Rotation, below |

Rotation at `0x6082F8` (main thread, inside `0x608270`):

1. Head has no ticket, or its ticket is ready: call the original (normal path).
2. Head waits: walk the deque from `[queue+8]` (records +0xD8, next block via the map) to the first record that is
   either started with no pending ticket, or the first unstarted one. Unstarted: start it the loop's way (`0x5FCAD0`;
   0 -> set `+0xD4` = 1; anything else -> stop rotating, leave the record for the loop). Started: step it
   (`0x5FDEF0(builder, record+4)`).
3. A non-head builder whose ticket became ready: step it **repeatedly until state 2 or 8** (or a 3/4 result), so no
   non-head is left holding locks (states 3..7) when the hook returns.
4. Return 0 if any work was done (the loop re-checks the budget and calls again), 2 when every candidate waits or the
   in-flight cap is reached. Never return 1 or 4 for a non-head result.
5. Cap tiles in flight at the bridge's slot count (4 x 256 KB slots; 256x256x4 tiles fit one slot each).

Per-builder state (map keyed by builder pointer, at most 4 entries): ticket, tile index `+0x70`, tile array `+0x60`,
device generation `+0x88`, issue time, wait count. Re-validate each call: builder still in the deque, same tile, same
array, same generation; otherwise drop the entry (the game re-runs state 2 after its own reset). Drop all entries on
device reset; the bridge drops their slots.

Erase safety: records erased by `0x609850` (cancel `0x5FBE20`, sync build `0x5FE1A0`) are safe while waiting (no locks)
and, with rule 3, never mid-tile. Optional belt-and-braces: hook `0x609850` and drop the matching entry at once.

Fallback (rotation off or unsafe): option H, i.e. today's behaviour with the async read-back and a 2 at `0x6082F8` while
the head waits (one tile per frame, no GPU wait). Turn rotation off automatically on: a step of a non-head returning 3,
a started record found after an unstarted one, or a ticket error.

Checks for the test arms: tile bytes hashed in the sync and G arms must match; CAS "busy" must go idle; count tiles per
second and frames a Sim waits for its textures.

## 8. Implementation (development builds, not run in game)

Code: `features/compositor_async.h` (the decisions, no game or Direct3D types), `features/compositor_readback.cpp` (the
hooks and the bridge calls), feature `AsyncCompositor` in `patches/performance_patches.cpp` (development builds only).

| Setting (`[patches.AsyncCompositor]`) | Default | Meaning |
|---|---|---|
| `enabled` | true | The feature itself; it does nothing while `mode` is 0 and `tileHashLog` is off |
| `mode` | 0 | 0 = the game's read-back; 1 = one tile per frame through the bridge (option H); 2 = several tiles in flight (option G) |
| `tileHashLog` | false | Logs `[AsyncCompositor] tile <w>x<h> at <x>,<y> hash <fnv-1a 64>` for every tile as state 3 is about to read it (the tile's rectangle of the locked staging texture, `job+0x0C` bits, `job+0x10` pitch, `job+0x5C` rectangle) |

Hook sites, read again in the disassembly before use [verified, capstone on the binary above]:

| Site | Bytes | Finding |
|---|---|---|
| `0x5FDC8A` | `E8 51 E0 FF FF` -> `0x5FBCE0` | `test eax, eax; jne 0x5FDC62` returns eax unchanged, before the two locks: a 2 leaves `job+0x00..0x08` and both rectangles set and nothing locked |
| `0x5FDFFF` | `E8 1C F4 FF FF` -> `0x5FD420` | The dispatcher stores state 4 after the call whatever it returns, so a lost tile cannot be sent back to state 2 by writing `+0x74`. The hook renders the tile again itself (`0x5FDDE0`, the whole of state 2, synchronous) and then lets state 3 run |
| `0x6082F8` | `E8 F3 5B FF FF` -> `0x5FDEF0` | ecx = builder (`[record]`), the argument = `record+4`; results as in section 5 |
| `0x5FCAD0` | `56 8B F1 83 7E 1C 00 74 1C E8` | thiscall, no arguments, plain `ret`; 4 with `+0x94` set when the builder is invalid, done or has no tiles, otherwise a tail jump to the setup `0x5FC9D0` |
| `0x5FBCE0` | | Source = `[[rt]+0x24]` of the render target object from `0x617500`, destination = level 0 of the staging texture (`0x6194B0`), device vtable `+0x80` (GetRenderTargetData). The hook does not resolve these itself: it lets the function run and answers its GetRenderTargetData in Apex's detour (the read-back filter) with the bridge request |

How the plan of section 7 was built:

- Read-back (`0x5FDC8A`): the bridge request is issued from inside the game's own call; refused (no slot, mode off in
  the bridge, tile larger than a slot) means the game's copy runs as before. In mode 2 a refused tile is not deferred
  at all, so a builder behind the head completes it in the same rotation step.
- State 3 (`0x5FDFFF`): ready -> `0x5FDBF0` again with the copy skipped, then the game's state 3. Not ready (a
  synchronous build, or the head waited more than 250 ms) -> up to 4 ms of polling, then cancel and render again.
- Queue (`0x6082F8`): as section 7. The deque is walked from `[queue+8]` on every call (at most 256 records); entries
  whose builder left the queue are cancelled without touching the builder. Work behind the head is one piece per call
  (start one record, or step one builder until it is between tiles), then 0.
- Fallback to mode 1 for the rest of the session: a started record behind an unstarted one, a head that is not the
  first record, a device reset with tiles in flight, a ticket the bridge fails or no longer knows, a step of a builder
  behind the head returning 3, or more than 16 steps without leaving states 3 to 7. Logged once as
  `[AsyncCompositor] Rotation off, one tile per frame from here (<reason>)`.
- The bridge path needs the queue layout above: Steam 1.67.2 with the bytes at `0x6079D0` and `0x5FCAD0` checked at
  start. Elsewhere, and without the bridge's exports, modes 1 and 2 are the event-query deferral of 2025-10-05.
- Counters (Developer page, "Sim textures through the bridge", and a log line about every 10 s while tiles are issued):
  tiles in flight and the most at once, issued, ready, completed, rendered again, waits, waited out, dropped, refused,
  records started and steps behind the head, fallbacks with the last reason.

Tests:

- `tools/compositor_async_test` (Smoke suite): the production header over a model of the queue (sorted records, the
  builder state machine with both lock spans, one shared render target, a four-slot bridge). Checks per run: every tile
  holds its own pixels, no builder behind the head holds a lock after a hook call, the loop never receives the result
  of a builder behind the head, at most four tiles in flight, no ticket leaks. Cases: off, one tile, rotation, reset,
  failed and unknown tickets, broken order, a bridge that never answers or refuses, cancels and synchronous builds, and
  3000 random runs. In the model 18 tiles take 37 frames with one tile per frame and 14 with rotation (bridge latency
  2 frames); not a measurement of the game.
- In-game job `tools/ingame_runner/jobs/compositor-async.json` (arms game, h, g) and
  `tools/compositor_async_test/compare_tiles.ps1` for the tile hashes of its loads. Not run. It needs a development
  build and a bridge pair with patches 0016 and 0017.

Not verified: everything at run time. In particular that a second render of a tile (state 2 run twice) gives the same
pixels, that tiles are at most 256 KB (one bridge slot), and that the arms of the job see the same Sims.

## Open questions

- States 5 to 7: `0x5FD210` locks a mip level of the builder's own output texture (`0x5FC230` -> `0x618DF0`), unlocked
  through `0x5FC740`; none of `0x5FD210`, `0x5FBEF0`, `0x5FD820` or `0x5FD420` reaches the render target manager
  (`0x616FC0`, `0x617500`) or a read-back [verified by a call scan one level deep]. What each step computes is open.
- Budget unit of `+0x18` and the exact role of byte `+0x15`.
- Whether the state-1 path (`0x610660`) does a CPU read-back for some formats; if so it is a second site for G.
- Whether the bridge executes the async read-back in command order relative to later draws to the same RT (required).
