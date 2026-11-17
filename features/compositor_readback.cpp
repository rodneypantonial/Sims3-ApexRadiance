// Compositor tile readback without the GPU wait (see compositor_readback.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "compositor_readback.h"
#include "compositor_async.h"
#include "call_chain.h"
#include "d3d9_extra_hooks.h"
#include "game_version.h"
#include "game_addresses.h"
#include "render_callbacks.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "imgui.h"
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>

namespace CompositorReadback {
namespace {

using CallChain::Layer;
using CallChain::Site;
using FnJob = int(__fastcall*)(void* builder, void* edx, void* job); // thiscall(builder; job), ret 4
using FnFmt = int(__fastcall*)(void* builder, void* edx, int format); // thiscall(builder; format), ret 4

// Texture builder fields (FUN_005fdef0 / FUN_005fdbf0 / FUN_005fc680)
constexpr uintptr_t kStaging = 0x20;  // the system-memory texture object GetRenderTargetData writes and FUN_00618df0 locks
constexpr uintptr_t kTiles = 0x60;    // tile array (0x60 bytes each)
constexpr uintptr_t kTile = 0x70;     // current tile
constexpr uintptr_t kState = 0x74;    // state machine (2 = render + read back, 3 = use the tile)
constexpr uintptr_t kDeviceId = 0x88; // device generation; a change resets the builder
// The game's texture object (FUN_00618df0: locks [t+0x30] when [t+0x2C] == 0)
constexpr uintptr_t kTexLocked = 0x2C, kTexD3D = 0x30;

constexpr uintptr_t kDone = 0x94;     // byte: the build ended
// The queue record's job (FUN_005fdbf0 fills it; FUN_005fd420 reads the tile from it)
constexpr uintptr_t kJobBits = 0x0C, kJobPitch = 0x10; // the locked staging texture
constexpr uintptr_t kJobRect = 0x5C;                   // the tile in it: left, top, right, bottom
// Steam 1.67.2, the compositor queue (docs/features/compositor-async-re.md): a deque of 0xD8-byte records in blocks of four
constexpr uintptr_t kQueueGetter = 0x006079D0; // mov eax, [0x011CEB88]; ret
constexpr uint8_t kQueueGetterBytes[] = {0xA1, 0x88, 0xEB, 0x1C, 0x01, 0xC3};
constexpr uintptr_t kQueuePtr = 0x011CEB88;
constexpr uintptr_t kQueueFirst = 0x08, kQueueFirstBlockEnd = 0x10, kQueueFirstSlot = 0x14, kQueueEnd = 0x18;
constexpr uintptr_t kRecSize = 0xD8, kRecJob = 0x04, kRecStarted = 0xD4, kBlockSize = 0x360;
constexpr uintptr_t kStart = 0x005FCAD0; // thiscall(builder): the queue loop's start of a record (0 = started)
constexpr uint8_t kStartBytes[] = {0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x1C, 0x00, 0x74, 0x1C, 0xE8};
constexpr int kMaxRecords = 256;    // of the queue's records, head first (those in flight are among the first)
constexpr double kSyncWaitMs = 4.0; // a synchronous build waits this long for the bridge, then renders the tile again

constexpr uint32_t kMaxWaitFrames = 8; // then the game's path (its lock waits, as before)
constexpr double kMaxWaitMs = 250.0;

template <class T> T Field(const void* p, uintptr_t off) { return *reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(p) + off); }

struct Pending {
    bool active = false;
    void* builder = nullptr;
    void* job = nullptr;
    uint32_t tile = 0, deviceId = 0;
    uintptr_t tiles = 0;
    uint32_t waits = 0;
    uint64_t issued = 0; // QPC
};

std::mutex g_ctrl;
bool g_started = false;
bool g_resetHooked = false; // g_ctrl
std::atomic<bool> g_on{false};
std::atomic<bool> g_defer{true}; // developer switch: off = every call passes through
std::mutex g_lock;               // g_p, g_query, g_queryDev (render thread in practice)
Pending g_p;
std::atomic<bool> g_hasPending{false};
IDirect3DQuery9* g_query = nullptr;
IDirect3DDevice9* g_queryDev = nullptr; // identity only
double g_qpcMs = 0.0;

thread_local void* t_queueBuilder = nullptr;  // the builder the queue loop is stepping
thread_local void* t_state2Builder = nullptr; // inside its state-2 call
thread_local void* t_state2Job = nullptr;
thread_local bool t_deferredNow = false;
thread_local void* t_resumeBuilder = nullptr; // FUN_005fdbf0 run again by Apex: skip the readback

std::atomic<uint64_t> c_deferred{0}, c_resumed{0}, c_wait{0}, c_timedOut{0}, c_dropped{0}, c_gamePath{0};
std::atomic<uint64_t> g_resumeTicks{0}, g_resumeMaxTicks{0}, g_state2Ticks{0};
uintptr_t g_fetchFn = 0;
uintptr_t g_renderFn = 0;

uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

// ---- through the RTX Remix bridge's asynchronous read-back (compositor_async.h) ----
using FnReadAsync = HRESULT(__stdcall*)(IDirect3DSurface9*, IDirect3DSurface9*, uint32_t*);
using FnTicket = HRESULT(__stdcall*)(uint32_t);
using FnStart = int(__fastcall*)(void* builder, void* edx);
bool g_queueOk = false;     // this build's queue is the one read above
bool g_asyncLooked = false; // g_asyncLock
FnReadAsync g_readAsync = nullptr;
FnTicket g_poll = nullptr, g_cancel = nullptr;
std::atomic<bool> g_resetSeen{false};
std::atomic<bool> g_hashLog{false};
std::atomic<uint64_t> c_refused{0}, c_hashed{0};
thread_local void* t_filterBuilder = nullptr; // the builder whose read-back the filter below answers
thread_local uint32_t t_filterTicket = 0;
std::atomic<ExtraHooks::ReadbackFilter> g_prevFilter{nullptr};

struct GameEnv {
    CompositorAsync::Record recs[kMaxRecords];
    int n = -1;

    void Load() {
        n = 0;
        const void* q = g_queueOk ? *reinterpret_cast<void**>(kQueuePtr) : nullptr;
        if (!q) return;
        uintptr_t cur = Field<uintptr_t>(q, kQueueFirst), blockEnd = Field<uintptr_t>(q, kQueueFirstBlockEnd);
        const uintptr_t end = Field<uintptr_t>(q, kQueueEnd);
        const uintptr_t* slot = Field<const uintptr_t*>(q, kQueueFirstSlot);
        while (cur && cur != end && n < kMaxRecords) {
            recs[n++] = {*reinterpret_cast<void**>(cur), reinterpret_cast<void*>(cur + kRecJob), *reinterpret_cast<uint8_t*>(cur + kRecStarted) != 0};
            cur += kRecSize;
            if (cur == end) break;
            if (cur == blockEnd) cur = *++slot, blockEnd = cur + kBlockSize;
        }
    }
    int Count() {
        if (n < 0) Load();
        return n;
    }
    CompositorAsync::Record At(int i) { return recs[i]; }
    void MarkStarted(int i) {
        recs[i].started = true;
        *(reinterpret_cast<uint8_t*>(recs[i].job) - kRecJob + kRecStarted) = 1;
    }
    int State(void* b) { return Field<int>(b, kState); }
    uint32_t Tile(void* b) { return Field<uint32_t>(b, kTile); }
    uintptr_t Tiles(void* b) { return Field<uintptr_t>(b, kTiles); }
    uint32_t DeviceId(void* b) { return Field<uint32_t>(b, kDeviceId); }
    bool Done(void* b) { return Field<uint8_t>(b, kDone) != 0; }
    int Start(void* b) { return reinterpret_cast<FnStart>(kStart)(b, nullptr); }
    int Step(void* b, void* job) {
        void* const prev = t_queueBuilder;
        t_queueBuilder = b;
        const int r = reinterpret_cast<FnJob>(CallChain::Next(Site::CompQueue, Layer::CompositorReadback))(b, nullptr, job);
        t_queueBuilder = prev;
        return r;
    }
    static CompositorAsync::Poll ToPoll(HRESULT hr) {
        return hr == S_FALSE ? CompositorAsync::Poll::Pending : hr == S_OK ? CompositorAsync::Poll::Ready : CompositorAsync::Poll::Failed;
    }
    CompositorAsync::Poll PollTicket(uint32_t t) { return ToPoll(g_poll(t)); }
    CompositorAsync::Poll WaitTicket(uint32_t t) { // inside a synchronous build, or after the head waited too long
        const uint64_t t0 = Qpc();
        for (;;) {
            const CompositorAsync::Poll p = PollTicket(t);
            if (p != CompositorAsync::Poll::Pending || static_cast<double>(Qpc() - t0) * g_qpcMs >= kSyncWaitMs) return p;
            SwitchToThread();
        }
    }
    void Cancel(uint32_t t) { g_cancel(t); }
    uint64_t NowMs() { return GetTickCount64(); }
};

std::recursive_mutex g_asyncLock; // g_async (a rotation step re-enters the hooks below on its own thread)
CompositorAsync::Queue<GameEnv> g_async;
CompositorAsync::Stats g_asyncStats;                               // the last copy taken
uint64_t g_loggedFallbacks = 0, g_loggedIssued = 0, g_loggedAt = 0; // g_asyncLock

void DropLocked() {
    g_p.active = false;
    g_hasPending.store(false, std::memory_order_release);
    c_dropped.fetch_add(1, std::memory_order_relaxed);
}

// True when the GPU has finished the copy, or when there is nothing left to wait on (no query, device lost)
bool SignalledLocked() {
    if (!g_query) return true;
    const HRESULT hr = g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    return hr != S_FALSE;
}

// After the game's readback for (builder, job) in the queue's state 2: issue the event query and remember the tile
bool TryDefer(void* b, void* job) {
    void* tex = Field<void*>(b, kStaging);
    if (!tex || Field<uint32_t>(tex, kTexLocked) != 0) return false;
    auto* d3dTex = Field<IDirect3DTexture9*>(tex, kTexD3D);
    if (!d3dTex) return false;
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_p.active) return false;
    IDirect3DDevice9* dev = nullptr;
    if (FAILED(d3dTex->GetDevice(&dev)) || !dev) return false;
    if (dev != g_queryDev && g_query) {
        g_query->Release();
        g_query = nullptr;
    }
    g_queryDev = dev;
    if (!g_query && FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_query))) g_query = nullptr;
    dev->Release();
    if (!g_query || FAILED(g_query->Issue(D3DISSUE_END))) return false;
    g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH); // submit the copy now
    g_p.active = true;
    g_p.builder = b;
    g_p.job = job;
    g_p.tile = Field<uint32_t>(b, kTile);
    g_p.tiles = Field<uintptr_t>(b, kTiles);
    g_p.deviceId = Field<uint32_t>(b, kDeviceId);
    g_p.waits = 0;
    g_p.issued = Qpc();
    g_hasPending.store(true, std::memory_order_release);
    return true;
}

// (g_asyncLock) The device was reset since the last hook: the bridge dropped its requests
void ConsumeReset(GameEnv& env) {
    if (g_resetSeen.exchange(false, std::memory_order_acq_rel)) g_async.OnReset(env);
}

// (g_asyncLock) The first tile: the bridge's exports, and the read-back detour the filter below runs in
bool AsyncAvailable(void* b) {
    if (g_asyncLooked) return g_readAsync != nullptr;
    g_asyncLooked = true;
    const HMODULE bridge = GetModuleHandleW(L"d3d9.dll");
    const auto read = bridge ? reinterpret_cast<FnReadAsync>(GetProcAddress(bridge, "RemixBridge_ReadRenderTargetAsync")) : nullptr;
    const auto poll = bridge ? reinterpret_cast<FnTicket>(GetProcAddress(bridge, "RemixBridge_PollReadback")) : nullptr;
    const auto cancel = bridge ? reinterpret_cast<FnTicket>(GetProcAddress(bridge, "RemixBridge_CancelReadback")) : nullptr;
    void* tex = Field<void*>(b, kStaging);
    auto* d3dTex = tex ? Field<IDirect3DTexture9*>(tex, kTexD3D) : nullptr;
    IDirect3DDevice9* dev = nullptr;
    const bool hooked = read && poll && cancel && d3dTex && SUCCEEDED(d3dTex->GetDevice(&dev)) && dev && ExtraHooks::EnsureTargetHooks(dev);
    if (dev) dev->Release();
    if (!hooked) {
        LOG_INFO("[AsyncCompositor] No RTX Remix bridge with asynchronous read-backs (patch 0017): the event query stays");
        return false;
    }
    g_poll = poll, g_cancel = cancel, g_readAsync = read;
    LOG_INFO("[AsyncCompositor] Texture tiles are read back through the RTX Remix bridge's asynchronous read-back");
    return true;
}

// In the game's GetRenderTargetData for the tile (FUN_005fbce0, same thread): the bridge takes the copy and the call
// returns at once. Refused: the game's copy runs.
bool AsyncFilter(IDirect3DDevice9* dev, IDirect3DSurface9* src, IDirect3DSurface9* dst) {
    if (!t_filterBuilder) { // (another thread's read-back while ours is in place: the filter we stand in for)
        const auto prev = g_prevFilter.load(std::memory_order_acquire);
        return prev && prev != AsyncFilter && prev(dev, src, dst);
    }
    if (t_filterTicket || !src || !dst) return false;
    uint32_t ticket = 0;
    const HRESULT hr = g_readAsync(src, dst, &ticket);
    if (hr == S_OK && ticket) {
        t_filterTicket = ticket;
        return true;
    }
    c_refused.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// Development builds, [patches.AsyncCompositor] tileHashLog: the tile's pixels as state 3 is about to read them, so runs
// with and without the asynchronous read-back can be compared
void HashTile(void* job) {
    if (kPublicBuild || !g_hashLog.load(std::memory_order_relaxed)) return;
    const auto* bits = Field<const uint8_t*>(job, kJobBits);
    const int pitch = Field<int>(job, kJobPitch);
    const int l = Field<int>(job, kJobRect), t = Field<int>(job, kJobRect + 4), w = Field<int>(job, kJobRect + 8) - l, h = Field<int>(job, kJobRect + 12) - t;
    if (!bits || l < 0 || t < 0 || w <= 0 || h <= 0 || w > 4096 || h > 4096 || pitch < (l + w) * 4) return;
    uint64_t hash = 14695981039346656037ull; // FNV-1a
    for (int y = 0; y < h; y++) {
        const uint8_t* row = bits + static_cast<size_t>(t + y) * pitch + static_cast<size_t>(l) * 4;
        for (int i = 0; i < w * 4; i++) hash = (hash ^ row[i]) * 1099511628211ull;
    }
    c_hashed.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO(std::format("[AsyncCompositor] tile {}x{} at {},{} hash {:016x}", w, h, l, t, hash));
}

// (g_asyncLock) The counters for the log, about every 10 s while tiles go through the bridge, and each fallback once
void LogAsync() {
    const CompositorAsync::Stats& s = g_async.GetStats();
    g_asyncStats = s;
    if (s.fallbacks != g_loggedFallbacks) {
        g_loggedFallbacks = s.fallbacks;
        LOG_WARNING(std::format("[AsyncCompositor] Rotation off, one tile per frame from here ({})", s.fallbackReason));
    }
    const uint64_t now = GetTickCount64();
    if (kPublicBuild || s.issued == g_loggedIssued || now - g_loggedAt < 10000) return;
    g_loggedIssued = s.issued, g_loggedAt = now;
    LOG_INFO(std::format("[AsyncCompositor] {} tiles issued, {} ready, {} completed, {} rendered again, {} waits, {} waited out, {} dropped, {} refused; "
                         "{} in flight (most {}); {} records started and {} steps behind the head; {} fallbacks",
                         s.issued, s.ready, s.resumed, s.rerenders, s.waits, s.timedOut, s.dropped, c_refused.load(), s.inFlight, s.maxInFlight, s.started, s.stepped,
                         s.fallbacks));
}

// ---- hooks (render thread) ----

// 0x006082F8: the queue loop steps the head builder. While its copy is in flight, stop for this frame (2); with the
// bridge's read-back and mode 2, work on the records behind it first (compositor_async.h).
int __fastcall Hook_Queue(void* b, void*, void* job) {
    if (g_hasPending.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(g_lock);
        if (g_p.active && g_p.builder == b) {
            if (Field<uint32_t>(b, kState) != 3) DropLocked(); // reset: state 3 comes only after a new state 2
            else if (g_on.load(std::memory_order_relaxed) && !SignalledLocked()) {
                const double waited = static_cast<double>(Qpc() - g_p.issued) * g_qpcMs;
                if (g_p.waits < kMaxWaitFrames && waited < kMaxWaitMs) {
                    g_p.waits++;
                    c_wait.fetch_add(1, std::memory_order_relaxed);
                    return 2;
                }
                c_timedOut.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    std::lock_guard<std::recursive_mutex> lock(g_asyncLock);
    GameEnv env;
    ConsumeReset(env);
    const int r = g_async.OnQueueStep(env, b, job); // the head's own step, or with tiles in flight: wait, or work behind it
    LogAsync();
    return r;
}

// 0x005FDFEC: state 2 (render the tile, read it back). Deferral is allowed only here, called by the queue loop.
int __fastcall Hook_State2(void* b, void* edx, void* job) {
    const FnJob next = reinterpret_cast<FnJob>(CallChain::Next(Site::CompState2, Layer::CompositorReadback));
    if (t_queueBuilder != b || !g_on.load(std::memory_order_relaxed) || !g_defer.load(std::memory_order_relaxed)) return next(b, edx, job);
    const uint64_t t0 = Qpc();
    t_state2Builder = b;
    t_state2Job = job;
    t_deferredNow = false;
    const int r = next(b, edx, job);
    t_state2Builder = nullptr;
    t_state2Job = nullptr;
    if (t_deferredNow) {
        t_deferredNow = false;
        g_state2Ticks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
    }
    return r;
}

// 0x005FDC8A: FUN_005fdbf0's GetRenderTargetData (FUN_005fbce0). 0 = copied; any other value ends FUN_005fdbf0.
int __fastcall Hook_Readback(void* b, void* edx, int format) {
    const FnFmt next = reinterpret_cast<FnFmt>(CallChain::Next(Site::CompReadback, Layer::CompositorReadback));
    if (t_resumeBuilder == b) return 0; // copied in an earlier frame, the query signalled: straight to the locks
    if (g_hasPending.load(std::memory_order_acquire)) { // a fresh readback of a builder with a deferred tile: forget it
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_p.active && g_p.builder == b) DropLocked();
    }
    bool rotating = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_asyncLock);
        GameEnv env;
        ConsumeReset(env);
        g_async.OnFreshReadback(env, b);
        const bool bridge = t_state2Builder == b && g_async.GetMode() != CompositorAsync::Mode::Off && AsyncAvailable(b);
        rotating = bridge && g_async.GetMode() == CompositorAsync::Mode::Rotate;
        if (bridge && g_async.CanIssue()) {
            t_filterBuilder = b;
            t_filterTicket = 0;
            const auto prev = ExtraHooks::SetReadbackFilter(AsyncFilter);
            g_prevFilter.store(prev, std::memory_order_release);
            const int r = next(b, edx, format); // the game's own surfaces; its GetRenderTargetData is answered by AsyncFilter
            ExtraHooks::SetReadbackFilter(prev);
            t_filterBuilder = nullptr;
            const uint32_t ticket = t_filterTicket;
            t_filterTicket = 0;
            if (ticket && r != 0) g_cancel(ticket);
            if (!ticket || r != 0) { // refused: the game copied (or failed) as before
                c_gamePath.fetch_add(1, std::memory_order_relaxed);
                return r;
            }
            if (g_async.OnIssued(env, b, ticket)) {
                t_deferredNow = true;
                return 2; // FUN_005fdbf0 and FUN_005fdde0 return it; the builder goes to state 3 with nothing locked
            }
            // (no slot after all: the target still holds this tile, the game's copy below reads it)
        }
    }
    const int r = next(b, edx, format);
    if (r != 0 || t_state2Builder != b) return r;
    if (!rotating && TryDefer(b, t_state2Job)) { // (while rotating, a builder behind the head must not stop at an event query)
        t_deferredNow = true;
        c_deferred.fetch_add(1, std::memory_order_relaxed);
        return 2; // FUN_005fdbf0 and FUN_005fdde0 return it; the builder goes to state 3, the queue stops for the frame
    }
    c_gamePath.fetch_add(1, std::memory_order_relaxed);
    return r;
}

// 0x005FDFFF: state 3. A deferred tile first gets the rest of its state 2 (FUN_005fdbf0 without the copy).
int __fastcall Hook_State3(void* b, void* edx, void* job) {
    const FnJob next = reinterpret_cast<FnJob>(CallChain::Next(Site::CompState3, Layer::CompositorReadback));
    if (g_hasPending.load(std::memory_order_acquire)) {
        bool resume = false, skipCopy = false;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (g_p.active && g_p.builder == b) {
                resume = true;
                // The copy in the staging texture is this tile's only if nothing moved; otherwise copy again (the game's state 2)
                skipCopy = g_p.tile == Field<uint32_t>(b, kTile) && g_p.tiles == Field<uintptr_t>(b, kTiles) && g_p.deviceId == Field<uint32_t>(b, kDeviceId);
                g_p.active = false;
                g_hasPending.store(false, std::memory_order_release);
            }
        }
        if (resume) {
            const uint64_t t0 = Qpc();
            void* const prev = t_resumeBuilder;
            t_resumeBuilder = skipCopy ? b : nullptr;
            const int r = reinterpret_cast<FnJob>(g_fetchFn)(b, nullptr, job);
            t_resumeBuilder = prev;
            const uint64_t dt = Qpc() - t0;
            g_resumeTicks.fetch_add(dt, std::memory_order_relaxed);
            uint64_t m = g_resumeMaxTicks.load(std::memory_order_relaxed);
            while (dt > m && !g_resumeMaxTicks.compare_exchange_weak(m, dt, std::memory_order_relaxed)) {}
            c_resumed.fetch_add(1, std::memory_order_relaxed);
            if (r != 0) return r; // 3: the dispatcher resets the builder; 4: the build fails, as the game's state 2 would
        }
    }
    CompositorAsync::Resume act;
    {
        std::lock_guard<std::recursive_mutex> lock(g_asyncLock);
        GameEnv env;
        ConsumeReset(env);
        act = g_async.OnState3(env, b);
    }
    if (act == CompositorAsync::Resume::SkipCopy) { // the bridge's bytes are in the staging texture: the locks only
        const uint64_t t0 = Qpc();
        void* const prev = t_resumeBuilder;
        t_resumeBuilder = b;
        const int r = reinterpret_cast<FnJob>(g_fetchFn)(b, nullptr, job);
        t_resumeBuilder = prev;
        g_resumeTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
        if (r != 0) return r;
    } else if (act == CompositorAsync::Resume::Render) {
        // The copy was lost or late. Other builders may have drawn into the shared target since: the tile is rendered
        // again and read back the game's way (FUN_005fdde0, state 2 in full), never copied again.
        const int r = reinterpret_cast<FnJob>(g_renderFn)(b, nullptr, job);
        if (r != 0) return r;
    }
    HashTile(job);
    return next(b, edx, job);
}

void OnPreReset(IDirect3DDevice9*) {
    g_resetSeen.store(true, std::memory_order_release); // (taken up by the next hook, on its own thread)
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_query) g_query->Release();
    g_query = nullptr; // a pending tile resumes without waiting on it (its device id changes, so the game resets it anyway)
    g_queryDev = nullptr;
}

struct Hook {
    Site site;
    void* fn;
    const char* what;
};
const Hook kHooks[] = {
    {Site::CompState3, reinterpret_cast<void*>(&Hook_State3), "state 3"}, // completing hooks first, deferring ones last
    {Site::CompQueue, reinterpret_cast<void*>(&Hook_Queue), "queue step"},
    {Site::CompReadback, reinterpret_cast<void*>(&Hook_Readback), "readback"},
    {Site::CompState2, reinterpret_cast<void*>(&Hook_State2), "state 2"},
};

void RemoveAll() {
    for (int i = static_cast<int>(std::size(kHooks)) - 1; i >= 0; i--) CallChain::Remove(kHooks[i].site, Layer::CompositorReadback);
}

} // namespace

bool Start(int mode, bool tileHashLog, std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::string missing;
    if (!GameAddr::GroupAvailable("CompositorReadback", &missing)) return fail(GameAddr::NotAvailable(missing));
    g_fetchFn = GameAddr::Get(GameAddr::Id::CompTileFetch);
    if (!g_fetchFn) return fail(GameAddr::NotAvailable("CompTileFetch"));
    g_renderFn = GameAddr::Get(GameAddr::Id::CompTileRender);
    if (!g_renderFn) return fail(GameAddr::NotAvailable("CompTileRender"));
    // The bridge path walks the queue (which builders are still in it): only where the queue is the one that was read
    g_queueOk = g_gameVersion == GameVersion::Steam && std::memcmp(reinterpret_cast<const void*>(kQueueGetter), kQueueGetterBytes, sizeof kQueueGetterBytes) == 0 &&
                std::memcmp(reinterpret_cast<const void*>(kStart), kStartBytes, sizeof kStartBytes) == 0;
    const auto want = static_cast<CompositorAsync::Mode>(mode < 0 ? 0 : mode > 2 ? 2 : mode);
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    for (const Hook& h : kHooks) {
        std::string err;
        if (!CallChain::Install(h.site, Layer::CompositorReadback, h.fn, &err)) {
            if (!g_hasPending.load()) RemoveAll();
            return fail(std::format("Could not hook the texture builder's {}: {}", h.what, err));
        }
    }
    if (!g_resetHooked) RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
    g_resetHooked = true;
    {
        std::lock_guard<std::recursive_mutex> alock(g_asyncLock);
        g_async.SetMode(g_queueOk ? want : CompositorAsync::Mode::Off);
    }
    g_hashLog.store(tileHashLog, std::memory_order_release);
    g_on.store(want != CompositorAsync::Mode::Off, std::memory_order_release);
    g_started = true;
    LOG_INFO(std::format("[AsyncCompositor] On: mode {} ({}{}{}; queue {:#010x}, readback {:#010x}, state 3 {:#010x})", static_cast<int>(want),
                         want == CompositorAsync::Mode::Off ? "the game's read-back" : want == CompositorAsync::Mode::OneTile ? "one tile per frame" : "several tiles in flight",
                         want != CompositorAsync::Mode::Off && !g_queueOk ? ", event query only: the queue is not Steam 1.67.2's" : "", tileHashLog ? ", tile hashes logged" : "",
                         CallChain::CallAddress(Site::CompQueue), CallChain::CallAddress(Site::CompReadback), CallChain::CallAddress(Site::CompState3)));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release); // no new deferral; a pending tile still completes through state 3
    g_hashLog.store(false, std::memory_order_release);
    g_started = false;
    bool inFlight = false;
    {
        std::lock_guard<std::recursive_mutex> alock(g_asyncLock);
        g_async.SetMode(CompositorAsync::Mode::Off); // (tiles in flight still complete; the queue waits for the head's)
        inFlight = g_async.Busy();
    }
    const Stats s = GetStats();
    if (inFlight || g_hasPending.load(std::memory_order_acquire)) {
        // Removing state 3 now would hand FUN_005fd420 a tile that was never locked: the hooks stay and pass calls through
        LOG_INFO(std::format("[AsyncCompositor] Off ({} tiles deferred); hooks kept until a tile in flight completes (they pass calls through)", s.deferred));
        return;
    }
    RemoveAll();
    RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
    g_resetHooked = false;
    LOG_INFO(std::format("[AsyncCompositor] Off ({} tiles deferred, {} resumed, {} frames waited, {} timed out, {} dropped; deferred locks {:.1f} ms, slowest {:.2f} ms)",
                         s.deferred, s.resumed, s.waitFrames, s.timedOut, s.dropped, s.resumeMs, s.resumeMaxMs));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

namespace {
// (never waits for a queue step in progress on another thread: the last copy then)
CompositorAsync::Stats AsyncStats() {
    if (g_asyncLock.try_lock()) {
        g_asyncStats = g_async.GetStats();
        g_asyncLock.unlock();
    }
    return g_asyncStats;
}
} // namespace

Stats GetStats() {
    Stats s;
    s.deferred = c_deferred.load();
    s.resumed = c_resumed.load();
    s.waitFrames = c_wait.load();
    s.timedOut = c_timedOut.load();
    s.dropped = c_dropped.load();
    s.gamePath = c_gamePath.load();
    s.resumeMs = static_cast<double>(g_resumeTicks.load()) * g_qpcMs;
    s.resumeMaxMs = static_cast<double>(g_resumeMaxTicks.load()) * g_qpcMs;
    s.state2Ms = static_cast<double>(g_state2Ticks.load()) * g_qpcMs;
    s.refused = c_refused.load();
    s.hashed = c_hashed.load();
    return s;
}

std::string StatusText() {
    if (!Running()) return "Off";
    const Stats s = GetStats();
    if (const CompositorAsync::Stats a = AsyncStats(); a.issued)
        return std::format("On: {} texture tiles read back through the bridge ({} in flight, most {}; {} rendered again; {} fallbacks)", a.issued, a.inFlight, a.maxInFlight,
                           a.rerenders, a.fallbacks);
    if (!s.deferred) return "On (no texture built yet)";
    return std::format("On: {} texture tiles read back without waiting ({} frames waited on the GPU, locks {:.2f} ms each on average)", s.deferred, s.waitFrames,
                       s.resumed ? s.resumeMs / static_cast<double>(s.resumed) : 0.0);
}

void RenderDeveloperUI() {
    if (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    const Stats s = GetStats();
    ImGui::TextUnformatted(("Compositor readback: " + StatusText()).c_str());
    ImGui::TextDisabled("Deferred %llu, resumed %llu, waited %llu frames, timed out %llu, dropped %llu, left to the game %llu",
                        static_cast<unsigned long long>(s.deferred), static_cast<unsigned long long>(s.resumed), static_cast<unsigned long long>(s.waitFrames),
                        static_cast<unsigned long long>(s.timedOut), static_cast<unsigned long long>(s.dropped), static_cast<unsigned long long>(s.gamePath));
    ImGui::TextDisabled("Deferred locks %.1f ms (slowest %.2f ms); state 2 without its lock %.1f ms", s.resumeMs, s.resumeMaxMs, s.state2Ms);
    const CompositorAsync::Stats a = AsyncStats();
    const auto n = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    ImGui::TextDisabled("Bridge: %u tiles in flight (most %u), %llu issued, %llu ready, %llu completed, %llu rendered again, %llu refused", a.inFlight, a.maxInFlight,
                        n(a.issued), n(a.ready), n(a.resumed), n(a.rerenders), n(s.refused));
    ImGui::TextDisabled("Bridge: %llu waits, %llu waited out, %llu dropped; behind the head %llu started, %llu steps; %llu fallbacks%s%s; %llu tiles hashed", n(a.waits),
                        n(a.timedOut), n(a.dropped), n(a.started), n(a.stepped), n(a.fallbacks), a.fallbacks ? ": " : "", a.fallbacks ? a.fallbackReason : "", n(s.hashed));
    bool defer = g_defer.load();
    if (ImGui::Checkbox("Defer compositor tile locks (off = the game's own wait, for A/B)##CrDefer", &defer)) g_defer.store(defer);
}

} // namespace CompositorReadback
