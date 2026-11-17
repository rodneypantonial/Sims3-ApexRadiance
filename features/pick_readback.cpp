// The mouse pick read back without waiting for the GPU (see pick_readback.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "pick_readback.h"
#include "apex_config.h"
#include "apex_log.h"
#include "d3d9_extra_hooks.h"
#include "d3d9_hooks.h"
#include "game_version.h"
#include "memory_patch.h"
#include "render_callbacks.h"
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstring>
#include <format>

namespace PickReadback {
namespace {

// Steam 1.67.2: the pick manager's per-frame update (Scene::BeginFrame calls it at 0x006EBC62). With
// RenderPickDelayReadback on, one update renders the pick target and sets [this+0x14]; the next reads it back
// (0x006DFF45 -> 0x00610660, GetRenderTargetData) and clears it. That copy waited for the GPU: about 2.5 ms every other
// frame under the RTX Remix bridge (1.35 s in 10 s, a far camera pan in Sunset Valley).
constexpr uintptr_t kUpdate = 0x006DFEC0;
constexpr uint8_t kUpdateBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x84, 0x07, 0x00, 0x00};
constexpr uintptr_t kReadbackDue = 0x14; // byte: the pick was rendered, its read-back is due this update
constexpr uint32_t kMaxWaitFrames = 4;   // then the game's read-back, waiting as before

using FnUpdate = void(__fastcall*)(void* self, void* edx);
FnUpdate g_original = nullptr;
std::atomic<IDirect3DDevice9*> g_dev{nullptr}; // the game's device (identity; Present)
IDirect3DQuery9* g_query = nullptr;            // render thread
IDirect3DDevice9* g_queryDev = nullptr;        // the device g_query was made on (identity)
void* g_pending = nullptr;                     // the manager whose read-back waits on g_query
uint32_t g_waits = 0;
std::atomic<uint64_t> c_deferred{0}, c_frames{0}, c_timedOut{0};

// [ui] async_pick_readback: under the RTX Remix bridge with patch 0017 the read-back itself goes asynchronous. The bridge
// copies the pick target into a shared slot, and the update waits until the bytes are in the game's surface. The game's
// GetRenderTargetData is then answered from them (ReadbackFilter) instead of a round trip to the server.
using FnReadAsync = HRESULT(__stdcall*)(IDirect3DSurface9*, IDirect3DSurface9*, uint32_t*);
using FnTicket = HRESULT(__stdcall*)(uint32_t);
bool g_asyncWanted = false;
bool g_asyncLooked = false;
FnReadAsync g_readAsync = nullptr; // nullptr: not looked up, not exported or refused (then the event query above)
FnTicket g_poll = nullptr;
FnTicket g_cancel = nullptr;
IDirect3DSurface9* g_pickSrc = nullptr; // the pick pair, learned from the game's own read-back (referenced)
IDirect3DSurface9* g_pickDst = nullptr;
uint32_t g_ticket = 0;   // the bridge request for g_pending (0: none)
bool g_inUpdate = false; // the game's update runs (render thread)
bool g_answered = false; // the pick pair's bytes are in g_pickDst: skip the game's copy once
std::atomic<uint64_t> c_asyncIssued{0}, c_asyncReady{0}, c_asyncFrames{0}, c_asyncFallback{0}, c_asyncTimedOut{0};

void CancelTicket() {
    if (g_ticket && g_cancel) g_cancel(g_ticket);
    g_ticket = 0;
}

void ReleasePair() {
    CancelTicket();
    if (g_pickSrc) g_pickSrc->Release();
    if (g_pickDst) g_pickDst->Release();
    g_pickSrc = g_pickDst = nullptr;
    g_answered = false;
}

// (render thread) Learns the pick pair from the game's read-back inside the update, and skips that copy once when the
// asynchronous read-back already put the same pixels into the same surface.
bool ReadbackFilter(IDirect3DDevice9*, IDirect3DSurface9* src, IDirect3DSurface9* dst) {
    if (!g_inUpdate || !src || !dst) return false;
    if (g_answered && src == g_pickSrc && dst == g_pickDst) {
        g_answered = false;
        return true;
    }
    g_answered = false;
    if (src != g_pickSrc || dst != g_pickDst) {
        ReleasePair();
        src->AddRef(), dst->AddRef();
        g_pickSrc = src, g_pickDst = dst;
    }
    return false;
}

void LookUpExports(IDirect3DDevice9* dev) {
    g_asyncLooked = true;
    const HMODULE bridge = GetModuleHandleW(L"d3d9.dll");
    const auto read = bridge ? reinterpret_cast<FnReadAsync>(GetProcAddress(bridge, "RemixBridge_ReadRenderTargetAsync")) : nullptr;
    const auto poll = bridge ? reinterpret_cast<FnTicket>(GetProcAddress(bridge, "RemixBridge_PollReadback")) : nullptr;
    const auto cancel = bridge ? reinterpret_cast<FnTicket>(GetProcAddress(bridge, "RemixBridge_CancelReadback")) : nullptr;
    if (!read || !poll || !cancel || !ExtraHooks::EnsureTargetHooks(dev)) {
        LOG_INFO("[PickReadback] async_pick_readback: no RTX Remix bridge with asynchronous read-backs (patch 0017), the event query stays");
        return;
    }
    g_readAsync = read, g_poll = poll, g_cancel = cancel;
    ExtraHooks::SetReadbackFilter(ReadbackFilter);
    LOG_INFO("[PickReadback] async_pick_readback: the pick is read back through the RTX Remix bridge's asynchronous read-back");
}

// The game's update. With answered set, its read-back of the pick pair is skipped (the bytes are already there).
void RunOriginal(void* self, void* edx, bool answered) {
    g_answered = answered;
    g_inUpdate = true;
    g_original(self, edx);
    g_inUpdate = false;
    g_answered = false;
}

// The asynchronous read-back of a due update. Returns false when the update falls back to the event query.
bool AsyncDue(void* self, void* edx) {
    if (g_pending != self) {
        uint32_t ticket = 0;
        const HRESULT hr = g_readAsync(g_pickSrc, g_pickDst, &ticket);
        if (hr == S_OK) {
            g_pending = self, g_ticket = ticket, g_waits = 0;
            c_asyncIssued.fetch_add(1, std::memory_order_relaxed);
            return true; // (the flag stays set: a later update reads back)
        }
        c_asyncFallback.fetch_add(1, std::memory_order_relaxed);
        if (hr == E_NOTIMPL) {
            g_readAsync = nullptr;
            LOG_INFO("[PickReadback] async_pick_readback: the bridge's client.asyncReadback is off, the event query stays");
        }
        return false;
    }
    const HRESULT hr = g_poll(g_ticket);
    if (hr == S_FALSE && ++g_waits < kMaxWaitFrames) {
        c_asyncFrames.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (hr == S_FALSE) {
        CancelTicket();
        c_asyncTimedOut.fetch_add(1, std::memory_order_relaxed);
    } else if (hr == S_OK) {
        c_asyncReady.fetch_add(1, std::memory_order_relaxed);
    } else {
        c_asyncFallback.fetch_add(1, std::memory_order_relaxed);
    }
    g_ticket = 0;
    g_pending = nullptr;
    RunOriginal(self, edx, hr == S_OK); // (otherwise the game's own read-back, waiting as before)
    return true;
}

void ReleaseQuery() {
    if (g_query) g_query->Release();
    g_query = nullptr;
    g_pending = nullptr;
    ReleasePair();
}

// (Steam 1.67.2, read from the code) With the flag set the update only reads back and returns (0x006DFF64 onwards, the
// next pick's request and render, runs only with it clear), so a deferred update skips nothing else.
// The read-back is due: the first time, an event query goes in behind the pick's draws and the update waits for the next
// frame; it reads back once the GPU has passed the query (no wait in the copy), or after kMaxWaitFrames
void __fastcall Hook_Update(void* self, void* edx) {
    IDirect3DDevice9* const dev = g_dev.load(std::memory_order_relaxed);
    if ((g_query || g_pickSrc) && g_queryDev != dev) ReleaseQuery(); // (another device: a query belongs to the one that made it)
    g_queryDev = dev;
    const bool due = *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(self) + kReadbackDue) != 0;
    if (g_pending == self && !due) g_pending = nullptr, CancelTicket(); // the game dropped it
    if (dev && due && g_asyncWanted && !g_asyncLooked) LookUpExports(dev);
    // (the asynchronous path once the pair is known, and not while an event query of the path below is pending)
    if (dev && due && g_readAsync && g_pickSrc && (g_pending != self || g_ticket) && AsyncDue(self, edx)) return;
    if (dev && due) {
        if (g_pending != self) {
            if (!g_query && FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_query))) g_query = nullptr;
            if (g_query && SUCCEEDED(g_query->Issue(D3DISSUE_END))) {
                g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH); // submit the pick's draws now
                g_pending = self, g_waits = 0;
                c_deferred.fetch_add(1, std::memory_order_relaxed);
                return; // (the flag stays set: the next update reads back)
            }
        } else if (g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE && ++g_waits < kMaxWaitFrames) {
            c_frames.fetch_add(1, std::memory_order_relaxed);
            return;
        } else if (g_waits >= kMaxWaitFrames) {
            c_timedOut.fetch_add(1, std::memory_order_relaxed);
        }
        g_pending = nullptr;
    }
    RunOriginal(self, edx, false);
}

void OnPreReset(IDirect3DDevice9*) { ReleaseQuery(); } // (render thread) the game reads its pick back as before until a new query

} // namespace

void Install() {
    if (g_gameVersion != GameVersion::Steam) return; // (the address is Steam 1.67.2's; other builds keep the game's wait)
    if (std::memcmp(reinterpret_cast<const void*>(kUpdate), kUpdateBytes, sizeof kUpdateBytes) != 0) {
        LOG_WARNING("[PickReadback] The pick update's code differs from Steam 1.67.2's: left to the game");
        return;
    }
    g_asyncWanted = ApexConfig::GetUi().asyncPickReadback;
    g_original = reinterpret_cast<FnUpdate>(kUpdate);
    if (!DetourBatch::InstallHooks({{reinterpret_cast<void**>(&g_original), reinterpret_cast<void*>(&Hook_Update)}})) {
        g_original = nullptr;
        LOG_ERROR("[PickReadback] Could not hook the pick update");
        return;
    }
    D3D9Hooks::RegisterPresent("PickReadback", [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        g_dev.store(ctx.device, std::memory_order_relaxed);
        return D3D9Hooks::HookAction::Continue;
    });
    RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
    LOG_INFO("[PickReadback] On: the mouse pick is read back once the GPU has drawn it, without waiting");
}

std::string StatsText() {
    return std::format("{} pick read-backs deferred, {} frames waited, {} waited out; asynchronous: {} issued, {} ready, {} frames waited, {} waited "
                       "out, {} fallbacks",
                       c_deferred.load(), c_frames.load(), c_timedOut.load(), c_asyncIssued.load(), c_asyncReady.load(),
                       c_asyncFrames.load(), c_asyncTimedOut.load(), c_asyncFallback.load());
}

} // namespace PickReadback
