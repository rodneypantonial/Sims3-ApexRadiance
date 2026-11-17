#pragma once
// Texture compositor tiles read back through the RTX Remix bridge's asynchronous read-back (bridge patch 0017): the
// decisions only, without the game or Direct3D. compositor_readback.cpp supplies both through Env;
// tools/compositor_async_test replays the same code over a model of the game's queue.
// The reading of the game's queue this follows: docs/features/compositor-async-re.md.
//
// Mode OneTile: the head builder's tile is copied by the bridge while the queue stops for the frame (one tile per frame,
// no wait for the GPU). Mode Rotate: while the head waits, the records behind it are started in queue order and stepped,
// so up to kSlots tiles are in flight. A builder that is not the head is never left between its lock and its unlock
// (states 3 to 7), and the loop never learns of its result (the loop's 1 and 4 pop the head).
// Rotate turns itself into OneTile on an order, reset or ticket error; a tile whose copy was lost is rendered again,
// never copied again from the render target the builders share.
#include <cstdint>

namespace CompositorAsync {

enum class Mode : int { Off = 0, OneTile = 1, Rotate = 2 };
enum class Poll { Pending, Ready, Failed };
enum class Resume { Game, SkipCopy, Render }; // state 3: nothing of ours; take the locks without the copy; render the tile again

constexpr int kSlots = 4;            // the bridge's read-back slots
constexpr int kMaxSteps = 16;        // states 3 to 7 are five steps
constexpr uint64_t kMaxWaitMs = 250; // then the tile is rendered again and read back the game's way

struct Record {
    void* builder = nullptr;
    void* job = nullptr;
    bool started = false;
};

struct Stats {
    uint64_t issued = 0;    // tiles handed to the bridge
    uint64_t ready = 0;     // answered by the bridge
    uint64_t resumed = 0;   // completed from the bridge's bytes
    uint64_t rerenders = 0; // rendered again (lost, late or failed copy)
    uint64_t waits = 0;     // queue steps answered with "stop for this frame" while tiles were in flight
    uint64_t timedOut = 0;  // waited longer than kMaxWaitMs, or not ready inside a synchronous build
    uint64_t dropped = 0;   // builder reset or gone while its tile was in flight
    uint64_t started = 0;   // records started behind a waiting head
    uint64_t stepped = 0;   // steps of builders behind a waiting head
    uint64_t fallbacks = 0; // Rotate turned into OneTile
    uint32_t inFlight = 0, maxInFlight = 0;
    const char* fallbackReason = "";
};

// Env: int Count(); Record At(int); void MarkStarted(int);            the queue's records, head first
//      int State(void*); uint32_t Tile(void*); uintptr_t Tiles(void*); uint32_t DeviceId(void*); bool Done(void*);
//      int Start(void* builder); int Step(void* builder, void* job);  the game's own start and state machine
//      Poll PollTicket(uint32_t); Poll WaitTicket(uint32_t); void Cancel(uint32_t); uint64_t NowMs();
template <class Env> class Queue {
  public:
    void SetMode(Mode m) { mode_ = m; }
    Mode GetMode() const { return mode_; }
    const Stats& GetStats() const { return s_; }
    bool Busy() const { return s_.inFlight != 0; }
    bool CanIssue() const { return mode_ != Mode::Off && s_.inFlight < kSlots; }

    // The bridge accepted the tile the builder just rendered. False: no slot (the caller copies the game's way, now).
    bool OnIssued(Env& env, void* b, uint32_t ticket) {
        Entry* e = Find(nullptr);
        if (!e || Find(b)) {
            env.Cancel(ticket);
            return false;
        }
        *e = Entry{b, ticket, env.Tile(b), env.DeviceId(b), env.Tiles(b), env.NowMs(), false, false};
        s_.issued++;
        if (++s_.inFlight > s_.maxInFlight) s_.maxInFlight = s_.inFlight;
        return true;
    }

    // The builder reads a tile back anew: whatever was in flight for it is stale.
    void OnFreshReadback(Env& env, void* b) {
        if (Entry* e = Find(b)) Drop(env, *e);
    }

    // State 3 of a builder (from the queue or a synchronous build).
    Resume OnState3(Env& env, void* b) {
        Entry* e = Find(b);
        if (!e) return Resume::Game;
        Entry c = *e;
        Free(*e);
        const bool same = c.tile == env.Tile(b) && c.tiles == env.Tiles(b) && c.deviceId == env.DeviceId(b);
        if (same && !c.ready && !c.failed) {
            const Poll p = env.WaitTicket(c.ticket);
            if (p == Poll::Ready) c.ready = true, s_.ready++;
            else if (p == Poll::Failed) c.failed = true, Fallback("ticket");
            else s_.timedOut++;
        }
        if (same && c.ready) {
            s_.resumed++;
            return Resume::SkipCopy;
        }
        if (!c.ready) env.Cancel(c.ticket);
        s_.rerenders++;
        return Resume::Render;
    }

    // The device is reset: the bridge drops its requests.
    void OnReset(Env& env) {
        for (Entry& e : e_) {
            if (!e.builder || e.ready) continue;
            env.Cancel(e.ticket);
            e.failed = true;
        }
        if (Busy()) Fallback("reset");
    }

    // The queue loop steps its head. Returns what the loop reads: the head's own result, 0 (go on) or 2 (stop).
    int OnQueueStep(Env& env, void* b, void* job) {
        if (!Busy()) return env.Step(b, job);
        const int n = env.Count();
        const bool listOk = n > 0 && env.At(0).builder == b && env.At(0).job == job;
        if (!listOk) Fallback("queue");
        else Prune(env, n);
        Entry* e = Find(b);
        if (e && env.State(b) != 3) Drop(env, *e), e = nullptr; // reset: state 3 comes only after a new state 2
        if (e) Refresh(env, *e);
        if (!e || e->ready || e->failed || TimedOut(env, *e)) return env.Step(b, job);
        if (mode_ == Mode::Rotate && listOk) {
            const int r = Rotate(env, n);
            if (r == 0) return 0;
        }
        s_.waits++;
        return 2;
    }

  private:
    struct Entry {
        void* builder = nullptr;
        uint32_t ticket = 0, tile = 0, deviceId = 0;
        uintptr_t tiles = 0;
        uint64_t issuedMs = 0;
        bool ready = false, failed = false;
    };

    Entry* Find(void* b) {
        for (Entry& e : e_)
            if (e.builder == b) return &e;
        return nullptr;
    }
    void Free(Entry& e) {
        e = Entry{};
        s_.inFlight--;
    }
    void Drop(Env& env, Entry& e) {
        if (!e.ready) env.Cancel(e.ticket);
        Free(e);
        s_.dropped++;
    }
    void Refresh(Env& env, Entry& e) {
        if (e.ready || e.failed) return;
        const Poll p = env.PollTicket(e.ticket);
        if (p == Poll::Ready) e.ready = true, s_.ready++;
        else if (p == Poll::Failed) e.failed = true, Fallback("ticket");
    }
    bool TimedOut(Env& env, const Entry& e) const { return env.NowMs() - e.issuedMs > kMaxWaitMs; }
    void Fallback(const char* why) {
        if (mode_ != Mode::Rotate) return;
        mode_ = Mode::OneTile;
        s_.fallbacks++;
        s_.fallbackReason = why;
    }
    // A builder that left the queue may be gone: its entry goes without touching it.
    void Prune(Env& env, int n) {
        for (Entry& e : e_) {
            if (!e.builder) continue;
            bool found = false;
            for (int i = 0; i < n && !found; i++) found = env.At(i).builder == e.builder;
            if (!found) Drop(env, e);
        }
    }

    // The head waits. One piece of work behind it: 0 = done some (the loop checks its time and calls again), 2 = none.
    int Rotate(Env& env, int n) {
        bool unstarted = false;
        for (int i = 0; i < n; i++) { // inserts rely on the started records being a prefix
            if (!env.At(i).started) unstarted = true;
            else if (unstarted) return Fallback("order"), 2;
        }
        for (int i = 1; i < n; i++) {
            const Record r = env.At(i);
            const bool full = s_.inFlight >= kSlots;
            if (!r.started) { // started in queue order only: the first one not started, the loop's own way
                if (full || env.Start(r.builder) != 0) return 2; // (refused: the loop pops or retries it as the head)
                env.MarkStarted(i);
                s_.started++;
                return 0;
            }
            if (env.Done(r.builder)) continue; // (popped by the loop once it is the head)
            Entry* e = Find(r.builder);
            if (e && env.State(r.builder) != 3) Drop(env, *e), e = nullptr;
            if (e) {
                Refresh(env, *e);
                if (!e->ready && !e->failed && !TimedOut(env, *e)) continue; // in flight
            } else if (full) {
                continue; // its next tile would find no slot
            }
            for (int k = 0; k < kMaxSteps; k++) {
                const int rc = env.Step(r.builder, r.job);
                s_.stepped++;
                if (rc == 3) return Fallback("reset"), 2; // the device was lost under it: the loop's stop
                if (rc == 2 && !Find(r.builder)) return 2; // (the game's own stop, not our deferral)
                if (rc != 0) return 0;                     // deferred, done or failed: nothing held, the head untouched
                const int st = env.State(r.builder);
                if (st < 3 || st > 7) return 0; // between tiles: nothing locked
            }
            return Fallback("steps"), 0;
        }
        return 2;
    }

    Mode mode_ = Mode::Off;
    Entry e_[kSlots];
    Stats s_;
};

} // namespace CompositorAsync
