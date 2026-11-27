// Start diagnostics (framework/start_diag_logic.h) offline checks: path masking, the DBPF index check for script
// resources on synthetic packages, and the change-only line logger.
#include "../../framework/start_diag_logic.h"
#include <cstdio>
#include <vector>

using namespace StartDiagLogic;
static int g_fail = 0;
static void Expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    g_fail += !ok;
}

static void Put(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    if (b.size() < at + 4) b.resize(at + 4);
    std::memcpy(b.data() + at, &v, 4);
}

// A DBPF 2.0 header (96 bytes) with an index of `types` at offset 96; `constType` puts the type in the flags' constant
static std::vector<uint8_t> Package(const std::vector<uint32_t>& types, bool constType, bool truncate = false) {
    std::vector<uint8_t> idx;
    Put(idx, 0, constType ? 1u : 0u);
    size_t at = 4;
    if (constType) { Put(idx, at, types.empty() ? 0 : types[0]); at += 4; }
    for (uint32_t t : types) {
        if (!constType) { Put(idx, at, t); at += 4; }
        for (int k = 0; k < 7; ++k) { Put(idx, at, 0x11111111u * (k + 1)); at += 4; }
    }
    if (truncate) idx.resize(idx.size() - 8);
    std::vector<uint8_t> p(96, 0);
    std::memcpy(p.data(), "DBPF", 4);
    Put(p, 4, 2);
    Put(p, 0x24, static_cast<uint32_t>(types.size()));
    Put(p, 0x2C, static_cast<uint32_t>(idx.size()));
    Put(p, 0x40, 96);
    p.insert(p.end(), idx.begin(), idx.end());
    return p;
}

static Has Scan(const std::vector<uint8_t>& p) {
    uint32_t count = 0, pos = 0, size = 0;
    if (!DbpfIndex(p.data(), p.size(), count, pos, size) || pos + size > p.size()) return Has::Bad;
    return IndexHasType(p.data() + pos, size, count, kS3SA);
}

int main() {
    const std::wstring prof = LR"(E:\Profiles\Tester.Nine)", user = L"Tester.Nine";
    Expect(Mask(LR"(E:\Profiles\tester.nine\Documents\x.package)", prof, user) == LR"(%USERPROFILE%\Documents\x.package)", "profile path masked, any case");
    Expect(Mask(L"e:/profiles/Tester.Nine/Saves", prof + LR"(\)", user) == L"%USERPROFILE%/Saves", "profile path with forward slashes");
    Expect(Mask(LR"(D:\Games\Tester.Nine Mods\a)", prof, user) == LR"(D:\Games\<user> Mods\a)", "user name masked elsewhere");
    Expect(Mask(L"abc", L"", L"a") == L"abc", "a one-letter user name is not masked");

    Expect(Scan(Package({0x0166038C, kS3SA, 0x220557DA}, false)) == Has::Yes, "S3SA among mixed types");
    Expect(Scan(Package({0x0166038C, 0x220557DA}, false)) == Has::No, "no S3SA");
    Expect(Scan(Package({kS3SA, kS3SA}, true)) == Has::Yes, "S3SA as the index's constant type");
    Expect(Scan(Package({0x0166038C}, true)) == Has::No, "other constant type");
    Expect(Scan(Package({}, false)) == Has::No, "empty index");
    Expect(Scan(Package({0x0166038C, kS3SA}, false, true)) == Has::Bad, "truncated index");
    auto v1 = Package({kS3SA}, false);
    Put(v1, 4, 1);
    Expect(Scan(v1) == Has::Bad, "DBPF 1.x refused");
    auto old = Package({kS3SA}, false);
    Put(old, 0x40, 0);
    Put(old, 0x28, 96);
    Expect(Scan(old) == Has::Yes, "index position from the older header field");

    ChangeLog log(3);
    Expect(log.Next("a", 0, 30000) == ChangeLog::Change, "first state logged");
    Expect(log.Next("a", 1000, 30000) == ChangeLog::None, "same state not logged");
    Expect(log.Next("a", 31000, 30000) == ChangeLog::Heartbeat, "heartbeat after 30 s");
    Expect(log.Next("b", 32000, 30000) == ChangeLog::Change, "change logged");
    Expect(log.Next("c", 33000, 30000) == ChangeLog::CapReached, "cap note once");
    Expect(log.Next("d", 34000, 30000) == ChangeLog::None && log.Next("e", 99000, 30000) == ChangeLog::None, "silent after the cap");

    Expect(PauseBits(0) == "0x0", "no pause bit");
    Expect(PauseBits(1) == "0x1 (script/modal)", "bit 1 named");
    Expect(PauseBits(6) == "0x6 (minimized, device lost/not in front)", "bits 2 and 4 named");
    Expect(PauseBits(0x11) == "0x11 (script/modal, other)", "unknown bit marked");

    ClockWatch w;
    int warn = 0;
    for (unsigned long long t = 0; t <= 19000; t += 1000) warn |= w.Update(t, 5.0f, 0, 1, true);
    Expect(warn == 0, "no warning before 20 s");
    Expect(w.Update(20000, 5.0f, 0, 1, true) == ClockWatch::ClockStuck, "clock stuck after 20 s unpaused");
    Expect(w.Update(25000, 5.0f, 0, 1, true) == 0, "stuck warned once");
    w.Update(26000, 6.0f, 0, 1, true);
    Expect(w.Update(45000, 6.0f, 0, 1, true) == 0 && w.Update(46000, 6.0f, 0, 1, true) == ClockWatch::ClockStuck, "warns again after the clock moved");
    ClockWatch ui;
    warn = 0;
    for (unsigned long long t = 0; t <= 60000; t += 1000) warn |= ui.Update(t, 5.0f, 0, 0, true);
    Expect(warn == 0, "the UI pause (level 0) is no stuck clock");
    warn = 0;
    for (unsigned long long t = 61000; t <= 80000; t += 1000) warn |= ui.Update(t, 5.0f, 0, 2, true);
    Expect(warn == 0, "20 s counted from the unpause");
    ClockWatch bits;
    warn = 0;
    for (unsigned long long t = 0; t <= 19000; t += 1000) warn |= bits.Update(t, 5.0f, 1, 1, true);
    Expect(warn == 0 && bits.Update(20000, 5.0f, 1, 1, true) == ClockWatch::BitsHeld, "bit 1 held 20 s warned");
    Expect(bits.Update(21000, 5.0f, 1, 1, true) == 0, "held bits warned once");
    Expect(bits.Update(22000, 5.0f, 3, 1, true) == ClockWatch::BitsHeld, "another bit set warned");
    ClockWatch notLive;
    warn = 0;
    for (unsigned long long t = 0; t <= 40000; t += 1000) warn |= notLive.Update(t, 5.0f, 1, 1, false);
    Expect(warn == 0, "no bit warning outside a loaded world");

    std::printf(g_fail ? "%d checks FAILED\n" : "all start diagnostics checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
