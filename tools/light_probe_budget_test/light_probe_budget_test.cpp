// The light capture's time budget (features/light_probe_budget.h) on a simulated clock: the recording frame of a slow
// device (every coverage wait and read-back a long round trip, as through the RTX Remix bridge), the deadline that drops
// a capture, and texture saving spread over frames. The loops follow features/light_probe.cpp (OnDraw, SaveTextures).
#include "light_probe_budget.h"
#include <cstdio>

namespace B = LightProbeBudget;
int g_checks = 0, g_failures = 0;
void Check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_failures++, std::printf("FAIL: %s\n", what);
}

struct Frame {
    double ms = 0;
    int plain = 0, drawn = 0;
    bool abandoned = false;
};
// OnDraw over `draws` screen-sized draws: `base` ms each always, `waitAnswer` ms until a coverage answer (capped by
// CoverageWaitMs), `extra` ms of probed values / geometry / detail while the budget lasts
Frame Record(int draws, double base, double waitAnswer, double extra) {
    Frame f;
    bool extrasOff = false, abandon = false;
    for (int i = 0; i < draws && !abandon; i++) {
        const bool extras = !extrasOff;
        f.ms += base;
        if (extras) {
            const double wait = B::CoverageWaitMs(f.ms);
            f.ms += waitAnswer < wait ? waitAnswer : wait;
            f.ms += extra;
        } else
            f.plain++;
        f.drawn++;
        extrasOff = !B::ExtrasAllowed(f.ms);
        abandon = B::PastRecordDeadline(f.ms);
    }
    f.abandoned = abandon;
    return f;
}

int main() {
    // policy edges
    Check(B::ExtrasAllowed(0) && !B::ExtrasAllowed(B::kExtrasMs), "extras end at the budget");
    Check(B::CoverageWaitMs(0) == B::kCoverageWaitMs, "a full coverage wait at the frame's start");
    Check(B::CoverageWaitMs(B::kExtrasMs - 10) == 10, "the coverage wait shrinks to the budget left");
    Check(B::CoverageWaitMs(B::kExtrasMs + 100) == 0, "no coverage wait past the budget");
    Check(!B::PastRecordDeadline(B::kRecordDeadlineMs - 1) && B::PastRecordDeadline(B::kRecordDeadlineMs), "record deadline");
    Check(B::KeepResolving(0, true) && !B::KeepResolving(0, false) && !B::KeepResolving(B::kResolveMs, true), "resolving stops when answered or late");
    Check(B::SaveMoreThisFrame(0, 1e9) && B::SaveMoreThisFrame(3, 1) && !B::SaveMoreThisFrame(1, B::kSaveFrameMs), "at least one texture a frame, then the cap");
    Check(B::kExtrasMs < B::kRecordDeadlineMs && B::kCoverageWaitMs <= B::kExtrasMs, "budget below deadline");

    // the gi-scenes case: 433 screen-sized draws, answers never ready in the frame (50 ms waits before), 24 read-backs of
    // 2 ms per probed draw. Before: 433 x 50 ms of waits alone (21.7 s) plus the read-backs
    const Frame slow = Record(433, 0.2, 1e9, 48);
    Check(!slow.abandoned && slow.drawn == 433, "the slow frame records every draw");
    Check(slow.ms < B::kExtrasMs + B::kCoverageWaitMs + 48 + 433 * 0.2 + 1, "the slow frame ends near its budget");
    Check(slow.plain > 400, "most draws of the slow frame are recorded without the optional work");
    std::printf("slow device: %d draws, %.0f ms (before: about %.0f ms), %d plain\n", slow.drawn, slow.ms, 433 * (0.2 + 50 + 48), slow.plain);

    // a fast device keeps every draw's optional work
    const Frame fast = Record(433, 0.05, 0.1, 0.3);
    Check(fast.plain == 0 && !fast.abandoned && fast.ms < B::kExtrasMs, "a fast frame keeps all optional work");

    // even the plain recording is too slow (10 ms a draw): dropped at the deadline, not held
    const Frame stuck = Record(4000, 10, 0, 0);
    Check(stuck.abandoned && stuck.ms < B::kRecordDeadlineMs + 10 + 1, "a stuck frame is dropped at its deadline");

    // texture saving over frames: 19 textures of 5 ms -> at most 2 a frame, no frame much past the cap
    {
        int left = 19, frames = 0;
        double worst = 0, finish = 0;
        while (left > 0 && !B::PastFinishDeadline(finish)) {
            double frame = 0;
            int written = 0;
            while (left > 0 && B::SaveMoreThisFrame(written, frame)) frame += 5, written++, left--;
            worst = frame > worst ? frame : worst;
            finish += frame + 16;
            frames++;
        }
        Check(left == 0 && frames == 10 && worst <= B::kSaveFrameMs + 5, "textures spread over frames under the cap");
    }
    // one texture costs more than the deadline: the rest are left out, not waited for
    {
        int left = 19, written = 0;
        double finish = 0;
        while (left > 0) {
            if (B::PastFinishDeadline(finish)) break;
            finish += 25000, written++, left--;
        }
        Check(written == 1 && left == 18, "past the finish deadline the rest are left out");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
