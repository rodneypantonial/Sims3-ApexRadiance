# RTX Remix bridge patches: upstream status

Apex Radiance builds the RTX Remix bridge from source with the patch series in
[native/remix-bridge/patches/](../native/remix-bridge/patches/) (see
[native/remix-bridge/README.md](../native/remix-bridge/README.md)). This page sorts each patch into fixes that belong
upstream and changes that stay in Apex Radiance, and holds the pull request text for the upstream ones.

## Upstream

- The bridge lives in the `bridge/` folder of
  [NVIDIAGameWorks/dxvk-remix](https://github.com/NVIDIAGameWorks/dxvk-remix). The old standalone
  `NVIDIAGameWorks/bridge-remix` repository is archived. `build.cmd` pins dxvk-remix `f35b86a`, which upstream `main`
  is ahead of.
- Every patch touches `bridge/` only. None changes the dxvk-remix runtime under `src/dxvk/`.
- Upstream develops in a private GitLab and mirrors to GitHub. A GitHub pull request is cherry-picked internally and
  then closed, not merged; the commit keeps its author (for example `bridge: fix volume texture lock pitches and
  upload` from pull request 163, and `Bridge: cut per-frame IPC cost of the D3D9 bridge` from pull request 158).
- Rules from `CONTRIBUTING.md` and `AGENTS.md`: one squashed commit per pull request, changes limited to the goal of
  the pull request, the C/C++ style guide (`documentation/CONTRIBUTING-style-guide.md`: two-space indent, braces
  always, comments that describe the current code), and a credit line in `src/dxvk/imgui/dxvk_imgui_about.cpp` under
  "Github Contributors", sorted by last name. There is no CLA and no DCO sign-off. Bug reports go to
  [NVIDIAGameWorks/rtx-remix](https://github.com/NVIDIAGameWorks/rtx-remix/issues), not to dxvk-remix. The repository
  has no pull request template.

## Patches

Class: **upstream** is a general D3D9 conformance or stability fix with a pull request branch ready. **upstream
later** is generally useful but needs more work before it is offered. **apex-only** is specific to The Sims 3, to
Apex Radiance's test harness or to diagnostics.

| Patch | Summary | Class | Upstream `main` | PR branch |
|---|---|---|---|---|
| 0001 | `GetRenderTarget` and `GetDepthStencilSurface` return `D3DERR_NOTFOUND` on an empty slot instead of `S_OK` with NULL | upstream | not fixed | `fix/get-render-target-notfound` |
| 0002 | `BridgePerf` counters of client copies of locked surfaces and buffers | apex-only (diagnostics) | not present | none |
| 0003 | `client.freeIdleManagedCopiesSec`: free a MANAGED texture level's client copy after N s without a lock, fetch it back on the next lock | upstream (with 0008, 0009, 0013) | not present | `fix/free-idle-client-copies` |
| 0004 | `[BridgeDiag]` memory, frame outlier, upload repeat and creation site lines | apex-only (diagnostics) | not present | none |
| 0005 | client logger: a line of 4 KB or more no longer ends the process with 0xC0000409 | upstream | not fixed | `fix/logger-long-lines` |
| 0006 | `client.ts3TextureCacheShift`: patches the game's ResourceCache texture size in memory | apex-only (The Sims 3) | not present | none |
| 0007 | `client.ts3TextureKeepAliveSec`: hooks the game's ResourceCache trim | apex-only (The Sims 3) | not present | none |
| 0008 | `client.freeIdleBufferCopiesSec`: free a static vertex or index buffer's client copy after N s without a lock | upstream (with 0003) | not present | `fix/free-idle-client-copies` |
| 0009 | `client.freeIdleManagedCopiesMs`: 0003's threshold in milliseconds | upstream (with 0003) | not present | `fix/free-idle-client-copies` |
| 0010 | render target 0 is the implicit swapchain's back buffer 0 object, as in d3d9 | upstream (with 0016) | not fixed (`LinkBackBuffer` place-holder) | `fix/render-target-0-is-back-buffer-0` |
| 0011 | `SetRenderTarget(0)`, `Reset` and `ResetEx` reset the client's viewport and scissor rect | upstream | not fixed (sent-state invalidation only) | `fix/render-target-0-resets-viewport` |
| 0012 | `SetFVF` and `SetVertexDeclaration` replace each other in the client's state | upstream | not fixed | `fix/setfvf-declaration-replace` |
| 0013 | idle copy fetch fixes for 0003 and 0008 | upstream (with 0003) | not present | `fix/free-idle-client-copies` |
| 0014 | a new surface shadow copy starts zeroed | upstream | not fixed | `fix/surface-copy-zeroed` |
| 0015 | `rewindMode`: snapshot and rewind of the server's D3D9 state for test harnesses | apex-only (test harness) | not present | none |
| 0016 | render target 0 is not counted as a device reference (fixes 0010's reference count) | upstream (folded into 0010) | needs 0010 | `fix/render-target-0-is-back-buffer-0` |
| 0017 | asynchronous `GetRenderTargetData` exports (`RemixBridge_ReadRenderTargetAsync`) | apex-only for now (new exported API) | not present | none |

0003, 0008, 0009 and 0013 together save 32-bit client address space for any game that keeps many MANAGED textures
or static buffers. They are offered as one change, `fix/free-idle-client-copies`, without the Apex-only `BridgePerf`
counters of the patches.

## Branches

Each branch is one commit on upstream `main` (`afb0252`), pushed to a contributor fork of dxvk-remix. The
commits carry no credit line yet: the first pull request adds the author to `dxvk_imgui_about.cpp`, the others leave
it out to avoid conflicts. Build: the six branches merged together build the release x64 server and x86 client with
VS 2022 and meson 1.12, and each branch alone builds the x86 client. Five branches add a bridge app test to
`bridge/test/rtx/unit/` (an x86 D3D9 app that `test_remix_api_bridge_app.py` runs against the client and server).
Each test passes on its own branch against a server on system d3d9, and all five pass on the merged build against a
server on vanilla DXVK. The tests do not call `Reset`, because a server on system d3d9 fails `Reset` on upstream
`main` too (it still holds the back buffer references). The five branches change the same list line in
`bridge/test/rtx/unit/meson.build`, so later cherry-picks need that one line merged. No game test has run on these
branches; the same changes ship in the Apex Radiance bridge package on the pinned commit. `fix/free-idle-client-copies` alone builds the
release x64 server and x86 client.

### fix/get-render-target-notfound

Title: `bridge: return D3DERR_NOTFOUND for an empty render target or depth-stencil slot`

```text
## Summary
d3d9 returns D3DERR_NOTFOUND when no render target is bound at the requested
index, or when no depth-stencil surface is set. The bridge client returned S_OK
with the output pointer set to NULL. Games that test only the HRESULT then
dereference NULL. The Sims 3 calls GetDepthStencilSurface in its clear path and
crashed a few minutes into a world with an access violation reading 0x0. This
may be the crash in NVIDIAGameWorks/rtx-remix#1028.

Both calls now return D3DERR_NOTFOUND with the output pointer set to NULL.

## Testing
- Built the bridge (x64 server, x86 client, release).
- New bridge app test test_render_target_notfound_device: both calls return
  D3DERR_NOTFOUND and clear the output pointer on an empty slot, and
  GetDepthStencilSurface returns the surface that was set.
- The Sims 3 under the bridge no longer crashes in the clear path; the same
  change has shipped in the Apex Radiance bridge package.
```

### fix/logger-long-lines

Title: `bridge: write client log lines without a fixed 4 KB buffer`

```text
## Summary
Logger::emitLine in the client formatted each line into a static 4096-byte
buffer with sprintf_s. A line of 4 KB or more makes sprintf_s call the CRT
invalid-parameter handler, which terminates the game with 0xC0000409. Long
lines occur with verbose logging. The static buffer was also shared by every
thread that logs.

The line is now built in a local std::string and written from there, and
WriteFile gets the bytes-written argument it needs when no OVERLAPPED is passed.

## Testing
- Built the bridge (x64 server, x86 client, release).
- A client log line longer than 4 KB is written whole instead of ending the
  process.
- No bridge app test: a D3D9 app has no call that makes the client log a line
  of a chosen length. To reproduce on main, log any line of 4095 characters
  or more in the client, for example Logger::info(std::string(5000, 'x')).
```

### fix/render-target-0-is-back-buffer-0

Title: `bridge: make render target 0 the implicit back buffer 0 object`

```text
## Summary
In d3d9 and DXVK, GetRenderTarget(0) and GetBackBuffer(0, 0, ...) on a new
device return the same IDirect3DSurface9. The bridge client created a separate
place-holder surface and linked it to the server's back buffer
(LinkBackBuffer), so the two calls returned different objects. Code that tests
whether the current render target is the back buffer by pointer
(post-processing injectors, overlays) never matched and skipped its work or
drew black.

The implicit render target is now the swapchain's back buffer 0, taken with
GetBackBuffer. The swapchain already maps that surface to the server's back
buffer, so the client no longer sends LinkBackBuffer. The server keeps
handling the command.

The back buffer references the swapchain, not the device, so it is not counted
in m_implicitRefCnt. Counted, a new device reported one reference less than its
creator holds: an AddRef/Release pair right after CreateDevice, as overlay
hooks do, dropped the count to the implicit references, destroyed the implicit
swapchain and depth surface, and the next GetBackBuffer crashed. With the
count right, the final Release also destroys the device.

## Testing
- Built the bridge (x64 server, x86 client, release).
- New bridge app test test_implicit_render_target_device: GetRenderTarget(0)
  and GetBackBuffer(0, 0) return the same pointer, AddRef/Release on a new
  device return 2 and 1, and the last Release returns 0.
- A post-processing injector that compares the two pointers draws again.
```

### fix/render-target-0-resets-viewport

Title: `bridge: reset the client's viewport and scissor rect with render target 0`

```text
## Summary
In d3d9, SetRenderTarget(0, ...) sets the viewport and the scissor rectangle to
the full size of the new render target, and Reset/ResetEx do the same for the
new back buffer. The client only invalidated its sent-state cache, so the
server state was right, but GetViewport and GetScissorRect kept returning the
previous target's values. A caller that saves the viewport, switches render
target and restores it afterwards put a stale viewport back, and the rest of
the game's pass drew with it.

The client's copies of the viewport and the scissor rect now follow the new
render target 0, and the back buffer after CreateDevice, Reset or ResetEx.

## Testing
- Built the bridge (x64 server, x86 client, release).
- New bridge app test test_render_target_viewport_device: GetViewport and
  GetScissorRect return the full target on a new device and after
  SetRenderTarget(0) to a smaller target and back. The test does not call
  Reset, which fails on a server that runs on system d3d9.
- A mid-frame post-processing pass no longer restores a stale viewport.
```

### fix/setfvf-declaration-replace

Title: `bridge: make SetFVF and SetVertexDeclaration replace each other`

```text
## Summary
In d3d9, SetVertexDeclaration makes GetFVF return 0, and SetFVF replaces the
current vertex declaration. The client kept the old FVF after
SetVertexDeclaration and the old declaration after SetFVF, so
GetVertexDeclaration returned a stale declaration while an FVF was in use.
Code that saves the vertex input and restores it as "declaration if any, else
FVF" put the stale declaration back, and the game drew its next fixed-function
draws with the wrong vertex layout.

SetVertexDeclaration now sets the client's FVF to 0 and SetFVF clears the
client's declaration. After SetFVF, GetVertexDeclaration returns no
declaration, where d3d9 returns one built from the FVF; restoring the FVF gives
the same result. SetFVF(0) leaves the client's state as it is, because d3d9
ignores an FVF of 0, so a caller that restores a saved FVF of 0 after its
declaration keeps the declaration. The FVF moves into the client's State so a
state block captures and applies it with the declaration, as d3d9 does. The
sent-state cache already treated the two as replacing each other.

While a state block is recorded (BeginStateBlock to EndStateBlock), both
setters write the FVF and the declaration into the recorded block and mark its
vertex declaration dirty, and the device's own state stays as it is, as on the
server. Applying the block then sets the pair, and Capture takes the pair from
the device.

## Testing
- Built the bridge (x64 server, x86 client, release).
- New bridge app test test_vertex_format_device: GetFVF returns 0 after
  SetVertexDeclaration, the declaration is gone after SetFVF and stays after
  SetFVF(0), and a state block restores the FVF and the declaration together.
  Recording SetVertexDeclaration or SetFVF in a state block leaves GetFVF and
  GetVertexDeclaration unchanged until the block is applied, and Capture of a
  recorded block and of a D3DSBT_VERTEXSTATE block takes the pair. Passes
  against a server on vanilla DXVK 3.1.1. The recording and Capture checks fail
  on the previous version of this change.
- Save/restore of the vertex input around a mid-frame pass keeps the game's
  fixed-function UI correct.
```

The first review asked for the state block recording case: while BeginStateBlock records, the client changed
its live FVF and declaration although the server only records them. The amended commit writes both into the
recorded block and marks its vertex declaration dirty. The extended test passes against a server on vanilla
DXVK 3.1.1, and its new recording and Capture checks fail on the first version of the commit. A server on
system d3d9 cannot create a device on a hidden desktop (`D3DERR_NOTAVAILABLE`), so that run was not repeated.

### fix/surface-copy-zeroed

Title: `bridge: zero-initialize a new surface shadow copy`

```text
## Summary
DXVK clears a new texture, and d3d9 drivers hand out zeroed texture memory in
practice. The client allocated the shadow copy of a lockable surface with
new uint8_t[size], which leaves it uninitialized, so a game that locks a new
level, writes part of it and unlocks sent heap garbage to the server. The
Sims 3 does this for its lot terrain overlay maps, and some lots drew their
ground black on some loads.

The copy is now value-initialized, which costs one pass over it at the
surface's first lock. The shared heap path (useSharedHeap, off by default) is
not changed.

## Testing
- Built the bridge (x64 server, x86 client, release).
- New bridge app test test_surface_shadow_device: the first lock of 16 new
  texture levels returns zeroed memory, although each earlier texture filled
  its level before it was released. The test fails on main.
- The affected lots draw their ground correctly on every load.
```

### fix/free-idle-client-copies

Title: `bridge: free idle client copies of MANAGED textures and static buffers`

```text
## Summary
The bridge client keeps a copy of every locked MANAGED texture level and of
every vertex and index buffer in the game process for the object's whole
life. A 32-bit game that fills its textures and static geometry once carries
that data twice, once on the server and once in its own address space.

Two client options, both off by default, free such a copy after it has not
been locked for a while:
- client.freeIdleManagedCopiesSec (or client.freeIdleManagedCopiesMs) for
  MANAGED texture levels.
- client.freeIdleBufferCopiesSec for static vertex and index buffers of
  64 KB to 4 MB.

A copy is freed only when it is no larger than half of serverChannelMemSize,
because the server sends it back as one blob through that channel.

Freeable copies of 64 KB and more come from VirtualAlloc, so freeing one
returns its address space. Idle copies are freed at the start of the swap
chain's Present, which the device's Present and PresentEx call. The next
lock fetches the copy back from the server: a texture level through the
existing IDirect3DSurface9_LockRect readback, a buffer through a Lock with
the new ReturnData flag. If the fetch fails, the lock fails, so that Unlock
cannot upload stale bytes over the server's copy. A CRC32C of the freed copy
is compared with the fetched one and a mismatch is logged once. With the
options off, nothing changes. With an option on, the client needs a server of
the same version. The options are documented in bridge/bridge.conf.

## Testing
- Built the bridge (x64 server, x86 client, release).
- A small D3D9 test app under the bridge (x64 server on vanilla DXVK) fills
  96 MANAGED 512x512 A8R8G8B8 textures and 64 static 1 MiB vertex buffers
  once, presents for 6 s, then relocks each one read-only and checks every
  byte:

  | Client process        | Options off | Options on (2 s) |
  |-----------------------|-------------|------------------|
  | Private bytes         | 164.9 MiB    | 3.9 MiB           |
  | Address space in use  | 396.7 MiB    | 229.2 MiB         |

  All 160 relocked objects hold the bytes written before the copies were
  freed, and no CRC mismatch is logged.
- The same app with one D3DCREATE_MULTITHREADED device, where a second
  thread relocks the objects in turn while the main thread presents for 12 s:
  no wrong object in 128 relock rounds or in the final check.
- The same change has shipped in the Apex Radiance bridge package.
```

Measurement: 2026-11-17, release builds of the branch, values read with `GetProcessMemoryInfo` and a `VirtualQuery`
walk after the idle presents. After the relock the fetched copies are back (164.2 MiB private), as expected.
