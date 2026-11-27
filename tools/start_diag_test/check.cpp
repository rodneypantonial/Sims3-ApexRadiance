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

    const std::vector<std::wstring> od{LR"(E:\Profiles\Tester.Nine\OneDrive - Example Org\)"};
    Expect(Mask(LR"(e:\profiles\tester.nine\onedrive - example org\Documents\x)", od, prof, user) == LR"(%OneDrive%\Documents\x)", "OneDrive folder masked before the profile");
    Expect(Mask(L"E:/Profiles/Tester.Nine/OneDrive - Example Org/a", od, prof, user) == L"%OneDrive%/a", "OneDrive with forward slashes");
    Expect(Mask(LR"(E:\Profiles\Tester.Nine\Documents)", od, prof, user) == LR"(%USERPROFILE%\Documents)", "profile still masked with OneDrive set");

    // footprint: FILETIME ticks (100 ns)
    const long long start = 134000000000000000LL, sec = 10000000LL;
    bool recent = false;
    Expect(Footprint(start - 12 * sec, start, recent) == "written 12 s before the start (in the 10 min before it)" && recent, "12 s before is recent");
    Expect(Footprint(start - 600 * sec, start, recent) .find("600 s before") != std::string::npos && recent, "10 min before still recent");
    Expect(Footprint(start - 601 * sec, start, recent) == "written 10 min before the start" && !recent, "just over 10 min is not recent");
    Expect(Footprint(start - 7200 * sec, start, recent) == "written 2 h before the start" && !recent, "hours");
    Expect(Footprint(start - 3 * 86400 * sec, start, recent) == "written 3 days before the start", "days");
    Expect(Footprint(start + 40 * sec, start, recent) == "written 40 s after the start" && !recent, "after the start is not recent");

    const std::string toml = "[meta]\nversion = 1\n[patches.SmoothPatchPrecise]\nenabled = true\ntickRateLimit = 480.0 # ticks\n"
                             "[patches.Mimalloc]\nenabled = true\n[qol.memory_monitor]\nwarning_style = \"modal\"\n";
    size_t total = 0;
    auto k = ConfigKeys(toml, {"smooth", "memory_monitor"}, 10, &total);
    Expect(total == 3 && k.size() == 3 && k[0] == "patches.SmoothPatchPrecise.enabled = true" && k[2] == "qol.memory_monitor.warning_style = \"modal\"", "config keys by section word");
    k = ConfigKeys(toml, {"TICK"}, 10, &total);
    Expect(k.size() == 1 && k[0] == "patches.SmoothPatchPrecise.tickRateLimit = 480.0 # ticks", "config keys by key word, any case");
    k = ConfigKeys("; c\n[Patches]\nShortWaitSpin=1\nThreadStackDiet = 0\n", {}, 1, &total);
    Expect(total == 2 && k.size() == 1 && k[0] == "Patches.ShortWaitSpin = 1", "every key, capped");

    const std::string json = R"({"FixWithoutAsking":true,"HideWhilePlaying":false,"PendingOptions":{},"IgnoredFindings":["a"],"Theme":"amethyst"})";
    Expect(JsonValue(json, "FixWithoutAsking") == "true" && JsonValue(json, "HideWhilePlaying") == "false", "json booleans");
    Expect(JsonValue(json, "PendingOptions") == "{}" && JsonValue(json, "IgnoredFindings") == "[...]", "json object and list");
    Expect(JsonValue(json, "Theme") == "\"amethyst\"" && JsonValue(json, "Missing").empty(), "json string and missing key");

    const std::string launcherLog = "2026-11-27T10:00:00 Launcher 2.3.6 EA 1.69\r\nSystem.IO.IOException: in use\r\n   at X\r\n"
                            "2026-11-27T10:05:00 Launcher 2.3.6 EA 1.69\r\nboom\r\n2026-11-27T10:06:00 Launcher 2.3.6 x\r\n";
    auto e = LogEntries(launcherLog, 2);
    Expect(e.size() == 2 && e[0] == "2026-11-27T10:05:00 Launcher 2.3.6 EA 1.69 | boom" && e[1] == "2026-11-27T10:06:00 Launcher 2.3.6 x", "launcher log: last entries with their first line");
    Expect(LogEntries("not a log\n", 5).empty(), "launcher log: no header, no entry");

    PauseTrack pt;
    pt.Update(0, 1);
    pt.Update(10, 1);
    Expect(pt.HeldSinceFirst() && pt.sets == 0, "bit set at the first sample, held");
    Expect(pt.Update(500, 0) == PauseTrack::Cleared && pt.clears == 1 && pt.lastClear == 500 && !pt.HeldSinceFirst(), "cleared");
    Expect(pt.Update(700, 5) == PauseTrack::Set && pt.sets == 1 && pt.since == 700 && pt.otherChanges == 1, "set again with another bit");
    Expect(pt.Update(800, 1) == PauseTrack::None && pt.otherChanges == 2, "other bit alone is no bit 0x1 event");

    VerdictFacts f;
    f.clockKnown = true; f.bits = 1; f.level = 1; f.speed = 0; f.now = 180; f.watchStart = 60; f.bitHeldSinceFirst = true;
    f.speedKeys = 4; f.errorFiles = 1; f.lastErrorFile = 45; f.inFront = true; f.footprintRead = true; f.recentWrites = {"S3SS.toml (12 s before)"};
    std::string v = Verdict(f);
    Expect(v.rfind("Verdict: paused by bits 0x1 (script/modal), bit 0x1 held since the first sample (during the load)", 0) == 0, "verdict: held since load");
    Expect(v.find("4 speed/pause keys, game time moved after 0") != std::string::npos && v.find("1 error files since the start, last at +45 s") != std::string::npos, "verdict: keys and errors");
    Expect(v.find("written in the 10 min before the start: S3SS.toml (12 s before)") != std::string::npos && v.find("(at +180 s, watch from +60 s)") != std::string::npos, "verdict: writes and times");
    VerdictFacts g;
    g.clockKnown = true; g.timeMoving = true; g.level = 2; g.speed = 3; g.sets = 1; g.clears = 1; g.lastClear = 70; g.footprintRead = true;
    v = Verdict(g);
    Expect(v.rfind("Verdict: game time moving (level 2, speed 3)", 0) == 0 && v.find("bit 0x1 last cleared at +70 s") != std::string::npos, "verdict: running");
    Expect(v.find("no speed or pause key pressed") != std::string::npos && v.find("no checked file written") != std::string::npos, "verdict: nothing pressed or written");
    VerdictFacts h;
    h.clockKnown = true; h.level = 0;
    Expect(Verdict(h).find("paused by the speed level 0 (no pause bit)") != std::string::npos && Verdict(h).find("file footprint not read yet") != std::string::npos, "verdict: UI pause");
    Expect(Verdict(VerdictFacts{}).rfind("Verdict: game clock unavailable", 0) == 0, "verdict: no clock");

    {
        const std::string a = "x -editInGameMode:true -apexDirect editInGameMode:true -editInGameMode=0 -EDITINGAMEMODE:1";
        ArgCopies c;
        CountArg(reinterpret_cast<const uint8_t*>(a.data()), a.size(), "editInGameMode", c);
        Expect(c.ascii == 2 && c.asciiOn == 1 && c.wide == 0, "switch copies: ASCII, dash and separator needed, exact case");
        const std::wstring wl = L"\"TS3W.exe\" -apexNoEditMode:true -editInGameMode:1 -editInGameMode";
        std::vector<uint8_t> wb(wl.size() * 2);
        for (size_t i = 0; i < wl.size(); ++i) { wb[2 * i] = static_cast<uint8_t>(wl[i]); wb[2 * i + 1] = 0; }
        ArgCopies d, r;
        CountArg(wb.data(), wb.size(), "editInGameMode", d);
        CountArg(wb.data(), wb.size(), "apexNoEditMode", r);
        Expect(d.wide == 1 && d.wideOn == 1 && d.ascii == 0 && r.wide == 1 && r.wideOn == 1, "switch copies: UTF-16, the end of the buffer");
    }

    std::printf(g_fail ? "%d checks FAILED\n" : "all start diagnostics checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
