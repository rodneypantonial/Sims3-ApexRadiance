// Offline checks of features/compositor_async.h: the production decisions run over a model of the game's texture
// compositor queue (docs/features/compositor-async-re.md): records sorted started-first then by priority, a state machine
// per builder, one render target shared by every builder, and a bridge with four read-back slots that answers late, out
// of order, or not at all. Read-only: prints its results, writes no files.
#include "compositor_async.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace CompositorAsync;

namespace {

int g_failed = 0;
std::string g_case;
void Check(bool ok, const char* what) {
    if (ok) return;
    if (++g_failed <= 20) std::printf("FAIL [%s] %s\n", g_case.c_str(), what);
}

struct Pixels { // what a surface holds: the tile of a builder
    int builder = -1;
    uint32_t tile = 0;
    bool operator==(const Pixels&) const = default;
};

struct Builder {
    int id = 0;
    int state = 0;
    uint32_t tile = 0, tiles = 0, deviceId = 0;
    bool valid = true, gpuOnly = false;
    bool done = false, ok = false, freed = false;
    bool locked = false, locked2 = false; // states 3 to 4, and 5 to 7
    Pixels staging;
    std::vector<Pixels> out;
    int renders = 0;
};

struct Rec {
    Builder* b = nullptr;
    int priority = 0;
    bool started = false;
};

struct Ticket {
    Pixels pixels;
    Builder* dst = nullptr;
    int framesLeft = 0;
    bool fail = false;
};

struct World {
    Queue<World> model;
    std::vector<std::unique_ptr<Builder>> all;
    std::vector<std::unique_ptr<Rec>> q; // head first
    Pixels rt;                            // the render target every builder draws its tile into
    std::map<uint32_t, Ticket> bridge;
    uint32_t nextTicket = 1, deviceGen = 1;
    int frame = 0, latency = 2, latencySpread = 0;
    bool refuse = false, failNext = false, inHook = false, orderBroken = false;
    Builder* queueBuilder = nullptr; // the builder the queue (or our rotation) is stepping
    uint32_t seed = 1;
    bool lastStepHead = true; // the last step our hook made was the head's

    uint32_t Rand(uint32_t n) {
        seed = seed * 1664525u + 1013904223u;
        return n ? (seed >> 8) % n : 0;
    }
    static Builder* B(void* p) {
        Builder* b = static_cast<Builder*>(p);
        Check(b && !b->freed, "a freed builder was touched");
        return b;
    }

    // ---- Env ----
    int Count() { return static_cast<int>(q.size()); }
    Record At(int i) { return Record{q[i]->b, q[i].get(), q[i]->started}; }
    void MarkStarted(int i) { q[i]->started = true; }
    int State(void* b) { return B(b)->state; }
    uint32_t Tile(void* b) { return B(b)->tile; }
    uintptr_t Tiles(void* b) { return B(b)->tiles; }
    uint32_t DeviceId(void* b) { return B(b)->deviceId; }
    bool Done(void* b) { return B(b)->done; }
    uint64_t NowMs() { return static_cast<uint64_t>(frame) * 16; }
    Poll PollTicket(uint32_t t) {
        auto it = bridge.find(t);
        if (it == bridge.end()) return Poll::Failed;
        if (it->second.framesLeft > 0) return Poll::Pending;
        const Ticket k = it->second;
        bridge.erase(it);
        if (k.fail) return Poll::Failed;
        if (!k.dst->freed) k.dst->staging = k.pixels;
        return Poll::Ready;
    }
    Poll WaitTicket(uint32_t t) { return PollTicket(t); }
    void Cancel(uint32_t t) { bridge.erase(t); }
    int Start(void* p) { return GameStart(B(p)); }
    int Step(void* p, void* job) {
        Builder* b = B(p);
        Builder* const prev = queueBuilder;
        queueBuilder = b;
        const int r = Dispatch(b);
        queueBuilder = prev;
        if (inHook) lastStepHead = !q.empty() && job == q[0].get();
        return r;
    }

    // ---- the game ----
    void Setup(Builder* b) {
        b->deviceId = deviceGen;
        b->tile = 0;
        b->out.clear();
        b->state = b->gpuOnly ? 1 : 2;
    }
    int GameStart(Builder* b) { // 0x5FCAD0
        if (!b->valid || b->done || b->tiles == 0) {
            b->done = true, b->ok = false;
            return 4;
        }
        Setup(b);
        return 0;
    }
    void Finish(Builder* b) {
        b->state = 8;
        b->done = b->ok = true;
    }
    int Dispatch(Builder* b) { // 0x5FDEF0
        if (b->state == 8) return 1;
        if (b->deviceId != deviceGen) {
            b->deviceId = deviceGen;
            b->locked = b->locked2 = false;
            b->state = 0;
            return 3;
        }
        switch (b->state) {
        case 0: Setup(b); return 0;
        case 1:
            rt = Pixels{b->id, b->tile};
            b->out.push_back(rt);
            if (++b->tile == b->tiles) Finish(b);
            return 0;
        case 2: {
            const int r = Render(b, true);
            b->state = 3;
            return r;
        }
        case 3: {
            const Resume a = model.OnState3(*this, b); // our hook at 0x5FDFFF
            if (a == Resume::SkipCopy) b->locked = true;
            else if (a == Resume::Render) Render(b, false);
            Check(b->locked, "state 3 ran on a tile that is not locked");
            Check(b->staging == Pixels{b->id, b->tile}, "state 3 read another tile's pixels");
            b->out.push_back(b->staging);
            b->state = 4;
            return 0;
        }
        case 4: b->locked = false, b->state = 5; return 0;
        case 5: b->locked2 = true, b->state = 6; return 0;
        case 6: b->state = 7; return 0;
        default:
            b->locked2 = false;
            if (++b->tile == b->tiles) Finish(b);
            else b->state = 2;
            return 0;
        }
    }
    // 0x5FDDE0: draw the tile into the shared target, then read it back (our hook at 0x5FDC8A) and lock
    int Render(Builder* b, bool mayDefer) {
        rt = Pixels{b->id, b->tile};
        b->renders++;
        model.OnFreshReadback(*this, b);
        if (mayDefer && queueBuilder == b && model.CanIssue() && !refuse && bridge.size() < static_cast<size_t>(kSlots)) {
            const uint32_t t = nextTicket++;
            bridge[t] = Ticket{rt, b, latency + static_cast<int>(Rand(static_cast<uint32_t>(latencySpread + 1))), failNext};
            failNext = false;
            if (model.OnIssued(*this, b, t)) return 2;
        }
        b->staging = rt;
        b->locked = true;
        return 0;
    }

    Builder* Add(uint32_t tiles, int priority, bool gpuOnly = false, bool valid = true) {
        all.push_back(std::make_unique<Builder>());
        Builder* b = all.back().get();
        b->id = static_cast<int>(all.size());
        b->tiles = tiles, b->gpuOnly = gpuOnly, b->valid = valid;
        size_t at = 0; // upper bound: started first, then priority, then first come
        while (at < q.size() && (q[at]->started || q[at]->priority >= priority)) at++;
        auto r = std::make_unique<Rec>();
        r->b = b, r->priority = priority;
        q.insert(q.begin() + static_cast<std::ptrdiff_t>(at), std::move(r));
        return b;
    }
    void Pop() { q.erase(q.begin()); }
    int Find(Builder* b) {
        for (size_t i = 0; i < q.size(); i++)
            if (q[i]->b == b) return static_cast<int>(i);
        return -1;
    }
    void Cancel(Builder* b) { // 0x5FBE20: the record is erased, the builder goes
        const int i = Find(b);
        if (i < 0) return;
        Check(i == 0 || (!b->locked && !b->locked2), "a record was erased between its lock and its unlock"); // (the head: as in the game)
        q.erase(q.begin() + i);
        b->freed = true;
    }
    void SyncBuild(Builder* b) { // 0x5FE1A0: flush the started head, take the builder out of the queue, build it now
        if (!q.empty() && q[0]->started) {
            Builder* h = q[0]->b;
            int r = 0;
            while (!h->done && (r = Dispatch(h)) == 0) {}
            if (r != 3) Pop();
        }
        const int i = Find(b);
        if (i >= 0) {
            Check(i == 0 || (!b->locked && !b->locked2), "a record was erased between its lock and its unlock");
            q.erase(q.begin() + i);
        }
        if (b->done) return;
        if (b->state == 0 && GameStart(b) != 0) return;
        for (int r = 0, n = 0; !b->done && r != 4 && n < 1000; n++) r = Dispatch(b);
    }
    void Reset() {
        deviceGen++;
        bridge.clear();
        model.OnReset(*this);
    }
    void Invariants() {
        bool unstarted = false;
        for (size_t i = 0; i < q.size(); i++) {
            const Builder* b = q[i]->b;
            if (i > 0) Check(!b->locked && !b->locked2, "a builder behind the head was left holding a lock");
            if (!q[i]->started) unstarted = true;
            else Check(!unstarted || orderBroken, "a started record behind one that is not started");
        }
        Check(model.GetStats().inFlight <= static_cast<uint32_t>(kSlots) && bridge.size() <= static_cast<size_t>(kSlots), "more tiles in flight than slots");
    }
    void Update(int budget) { // 0x608270
        while (!q.empty() && budget-- > 0) {
            Rec* rec = q[0].get();
            Builder* b = rec->b;
            if (!rec->started) {
                const int r = GameStart(b);
                if (r == 0) rec->started = true;
                else if (r == 3) break;
                else Pop();
            } else if (b->done) {
                Pop();
            } else {
                inHook = true;
                const int r = model.OnQueueStep(*this, b, rec); // our hook at 0x6082F8
                inHook = false;
                Invariants();
                Check((r != 1 && r != 4) || lastStepHead, "the loop was handed the result of a builder behind the head");
                if (r == 2 || r == 3) break;
                if (r == 4) Pop();
            }
        }
    }
    void Frame(int budget = 40) {
        frame++;
        for (auto& [t, k] : bridge)
            if (k.framesLeft > 0) k.framesLeft--;
        Update(budget);
    }
    bool Drained() const { return q.empty(); }
    // Every builder that was not cancelled: all its tiles, in order, each with its own pixels
    void CheckResults() {
        Check(Drained(), "the queue did not drain");
        for (const auto& b : all) {
            if (b->freed) continue;
            if (!b->valid || b->tiles == 0) {
                Check(!b->ok, "an invalid builder succeeded");
                continue;
            }
            Check(b->done && b->ok, "a builder did not finish");
            Check(b->out.size() == b->tiles, "wrong number of tiles");
            for (size_t i = 0; i < b->out.size(); i++) Check(b->out[i] == Pixels{b->id, static_cast<uint32_t>(i)}, "a tile holds other pixels");
        }
        Check(!model.Busy(), "tiles left in flight");
        Check(bridge.empty(), "bridge tickets leaked");
    }
};

int Run(World& w, int maxFrames = 20000) {
    int f = 0;
    while (!w.Drained() && f < maxFrames) w.Frame(), f++;
    if (w.Drained() && w.model.Busy()) { // a cancelled builder's tile stays in flight until the queue is stepped again
        w.Add(1, 0);
        while (!w.Drained() && f < maxFrames) w.Frame(), f++;
    }
    w.CheckResults();
    return f;
}

void Fill(World& w, int builders, uint32_t tiles) {
    for (int i = 0; i < builders; i++) w.Add(tiles, i % 3 == 0 ? 10 : 0);
}

} // namespace

int main() {
    int framesOne = 0, framesRotate = 0;
    {
        g_case = "off";
        World w;
        Fill(w, 6, 3);
        Run(w);
        Check(w.model.GetStats().issued == 0, "mode off issued a read-back");
    }
    {
        g_case = "one tile";
        World w;
        w.model.SetMode(Mode::OneTile);
        Fill(w, 6, 3);
        framesOne = Run(w);
        const Stats& s = w.model.GetStats();
        Check(s.issued == 18 && s.resumed == 18 && s.rerenders == 0, "18 tiles through the bridge");
        Check(s.maxInFlight == 1 && s.started == 0 && s.stepped == 0, "one tile in flight, nothing behind the head touched");
    }
    {
        g_case = "rotate";
        World w;
        w.model.SetMode(Mode::Rotate);
        Fill(w, 6, 3);
        framesRotate = Run(w);
        const Stats& s = w.model.GetStats();
        Check(s.issued == 18 && s.resumed == 18 && s.rerenders == 0 && s.fallbacks == 0, "18 tiles through the bridge, no fallback");
        Check(s.maxInFlight == static_cast<uint32_t>(kSlots), "four tiles in flight");
        Check(framesRotate * 2 < framesOne, "rotation at least halves the frames");
        Check(w.model.GetMode() == Mode::Rotate, "still rotating");
    }
    {
        g_case = "reset";
        World w;
        w.model.SetMode(Mode::Rotate);
        Fill(w, 6, 3);
        for (int i = 0; i < 3; i++) w.Frame();
        Check(w.model.GetStats().inFlight > 1, "tiles in flight before the reset");
        w.Reset();
        Run(w);
        const Stats& s = w.model.GetStats();
        Check(w.model.GetMode() == Mode::OneTile && s.fallbacks == 1 && std::string(s.fallbackReason) == "reset", "a reset falls back to one tile");
    }
    {
        g_case = "ticket fails";
        World w;
        w.model.SetMode(Mode::Rotate);
        Fill(w, 6, 3);
        w.failNext = true;
        Run(w);
        const Stats& s = w.model.GetStats();
        Check(s.rerenders == 1 && s.fallbacks == 1 && std::string(s.fallbackReason) == "ticket", "a failed ticket renders the tile again and falls back");
    }
    {
        g_case = "ticket unknown";
        World w;
        w.model.SetMode(Mode::Rotate);
        Fill(w, 6, 3);
        w.Frame();
        w.bridge.erase(w.bridge.begin()); // the bridge no longer knows a ticket we hold
        Run(w);
        const Stats& s = w.model.GetStats();
        Check(s.rerenders == 1 && s.fallbacks == 1 && w.model.GetMode() == Mode::OneTile, "an unknown ticket renders the tile again and falls back");
    }
    {
        g_case = "order";
        World w;
        w.model.SetMode(Mode::Rotate);
        Fill(w, 6, 3);
        w.Frame();
        w.orderBroken = true;
        w.q.back()->started = true; // (as a wrong insert would leave it)
        w.q.back()->b->state = 2, w.q.back()->b->deviceId = w.deviceGen;
        w.Frame();
        Check(w.model.GetMode() == Mode::OneTile && std::string(w.model.GetStats().fallbackReason) == "order", "a started record behind an unstarted one falls back");
        Run(w);
    }
    {
        g_case = "late";
        World w;
        w.model.SetMode(Mode::Rotate);
        w.latency = 100000;
        Fill(w, 3, 2);
        Run(w);
        const Stats& s = w.model.GetStats();
        Check(s.rerenders == s.issued && s.resumed == 0 && s.timedOut == s.issued, "tiles the bridge never answers are rendered again");
    }
    {
        g_case = "refused";
        World w;
        w.model.SetMode(Mode::Rotate);
        w.refuse = true;
        Fill(w, 4, 2);
        Run(w);
        Check(w.model.GetStats().issued == 0, "a bridge that refuses leaves the game's path");
    }
    {
        g_case = "cancel and sync build";
        World w;
        w.model.SetMode(Mode::Rotate);
        Fill(w, 6, 3);
        w.Frame();
        w.Cancel(w.q[2]->b); // in flight behind the head
        w.Cancel(w.q[0]->b); // the waiting head
        w.SyncBuild(w.q[1]->b); // in flight: built now, its ticket not ready
        Builder* outside = w.all.emplace_back(std::make_unique<Builder>()).get();
        outside->id = 99, outside->tiles = 2;
        w.SyncBuild(outside); // a thumbnail: never queued, draws over the shared target
        Run(w);
        Check(outside->ok && w.model.GetStats().fallbacks == 0, "cancels and synchronous builds need no fallback");
    }
    // Everything at once, many seeds: late and failing tickets, enqueues, cancels, synchronous builds, resets, small budgets
    for (uint32_t seed = 1; seed <= 3000; seed++) {
        g_case = "random " + std::to_string(seed);
        World w;
        w.seed = seed;
        w.model.SetMode(seed % 5 == 0 ? Mode::OneTile : Mode::Rotate);
        w.latency = static_cast<int>(w.Rand(4));
        w.latencySpread = static_cast<int>(w.Rand(6));
        const bool harsh = seed % 2 == 0;
        int toAdd = 4 + static_cast<int>(w.Rand(30));
        for (int f = 0; f < 5000 && (toAdd > 0 || !w.Drained()); f++) {
            for (int k = static_cast<int>(w.Rand(4)); k > 0 && toAdd > 0; k--, toAdd--)
                w.Add(w.Rand(12) == 0 ? 0 : 1 + w.Rand(5), w.Rand(3) == 0 ? 10 : 0, w.Rand(8) == 0, w.Rand(15) != 0);
            if (harsh && w.Rand(40) == 0) w.failNext = true;
            if (harsh && w.Rand(25) == 0 && !w.q.empty()) w.Cancel(w.q[w.Rand(static_cast<uint32_t>(w.q.size()))]->b);
            if (harsh && w.Rand(25) == 0 && !w.q.empty()) w.SyncBuild(w.q[w.Rand(static_cast<uint32_t>(w.q.size()))]->b);
            if (harsh && w.Rand(120) == 0) w.Reset();
            if (harsh && w.Rand(60) == 0) w.refuse = !w.refuse;
            if (seed % 7 == 0 && w.Rand(50) == 0) w.model.SetMode(Mode::Off); // turned off with tiles in flight
            w.Frame(1 + static_cast<int>(w.Rand(12)));
        }
        Run(w);
        if (!harsh) Check(w.model.GetStats().fallbacks == 0 && w.model.GetStats().rerenders == 0, "a quiet run needs no fallback and no second render");
    }
    if (g_failed) {
        std::printf("compositor async checks: %d FAILED\n", g_failed);
        return 1;
    }
    std::printf("compositor async checks passed (one tile per frame: %d frames, rotation: %d frames for 18 tiles; 3000 random runs)\n", framesOne, framesRotate);
    return 0;
}
