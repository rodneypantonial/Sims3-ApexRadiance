#pragma once
// Pure parts of the start diagnostics (start_diag.cpp): path masking, the DBPF index check for script resources and the
// change-only line logger. No Windows calls, so tools/start_diag_test checks them offline.
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace StartDiagLogic {

// Every case-insensitive occurrence of `what` (at least 2 characters) in `s` replaced by `with`.
inline std::wstring ReplaceI(std::wstring s, std::wstring what, const std::wstring& with) {
    if (what.size() < 2) return s;
    for (auto& c : what) c = static_cast<wchar_t>(towlower(c));
    for (size_t i = 0; i + what.size() <= s.size();) {
        bool hit = true;
        for (size_t k = 0; k < what.size() && hit; ++k) hit = towlower(s[i + k]) == what[k];
        if (!hit) { ++i; continue; }
        s.replace(i, what.size(), with);
        i += with.size();
    }
    return s;
}

// The profile folder becomes %USERPROFILE% (with either slash direction), then the user name anywhere becomes <user>.
inline std::wstring Mask(std::wstring s, std::wstring profile, const std::wstring& user) {
    while (!profile.empty() && (profile.back() == L'\\' || profile.back() == L'/')) profile.pop_back();
    if (profile.size() >= 3) {
        s = ReplaceI(s, profile, L"%USERPROFILE%");
        std::wstring fwd = profile;
        for (auto& c : fwd) if (c == L'\\') c = L'/';
        s = ReplaceI(s, fwd, L"%USERPROFILE%");
    }
    return ReplaceI(s, user, L"<user>");
}

// The OneDrive folders (their names can hold an organisation's name) become %OneDrive% first, then Mask
inline std::wstring Mask(std::wstring s, const std::vector<std::wstring>& oneDrive, const std::wstring& profile, const std::wstring& user) {
    for (std::wstring root : oneDrive) {
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        if (root.size() < 3) continue;
        s = ReplaceI(s, root, L"%OneDrive%");
        for (auto& c : root) if (c == L'\\') c = L'/';
        s = ReplaceI(s, root, L"%OneDrive%");
    }
    return Mask(s, profile, user);
}

inline uint32_t U32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

constexpr uint32_t kS3SA = 0x073FAA07; // script assembly resource
constexpr size_t kDbpfHeader = 96;

// DBPF 2.x header: where its index is. False for anything else.
inline bool DbpfIndex(const uint8_t* h, size_t n, uint32_t& count, uint32_t& pos, uint32_t& size) {
    if (n < kDbpfHeader || std::memcmp(h, "DBPF", 4) != 0 || U32(h + 4) != 2) return false;
    count = U32(h + 0x24);
    size = U32(h + 0x2C);
    pos = U32(h + 0x40);
    if (!pos) pos = U32(h + 0x28);
    return true;
}

enum class Has { No, Yes, Bad };

// Does the index (flags word, constants, then `count` entries) list a resource of `type`?
inline Has IndexHasType(const uint8_t* idx, size_t n, uint32_t count, uint32_t type) {
    if (n < 4) return count ? Has::Bad : Has::No;
    const uint32_t flags = U32(idx);
    size_t at = 4;
    uint32_t constType = 0;
    int constants = 0;
    for (int bit = 0; bit < 3; ++bit) {
        if (!(flags & (1u << bit))) continue;
        if (at + 4 > n) return Has::Bad;
        if (bit == 0) constType = U32(idx + at);
        at += 4;
        ++constants;
    }
    const size_t entry = 4u * (8 - constants); // type, group, instance high (unless constant), instance low, offset, sizes, flags
    if (count && at + entry * count > n) return Has::Bad;
    if (flags & 1) return count && constType == type ? Has::Yes : Has::No;
    for (uint32_t i = 0; i < count; ++i)
        if (U32(idx + at + entry * i) == type) return Has::Yes;
    return Has::No;
}

// One line per change of a state, a heartbeat when it stays the same, at most `cap` lines and then one note.
struct ChangeLog {
    enum Kind { None, Change, Heartbeat, CapReached };
    std::string last;
    unsigned long long lastAt = 0;
    int lines = 0, cap = 0;
    bool started = false;
    explicit ChangeLog(int c) : cap(c) {}
    Kind Next(const std::string& state, unsigned long long now, unsigned long long heartbeatMs) {
        Kind k = None;
        if (!started || state != last) k = Change;
        else if (now - lastAt >= heartbeatMs) k = Heartbeat;
        if (k == None || lines > cap) return None;
        started = true;
        last = state;
        lastAt = now;
        return ++lines > cap ? CapReached : k;
    }
};

// The game clock's pause bits (GameClock +0x40) with their meaning; the UI pause button sets none (speed and level 0)
inline std::string PauseBits(uint32_t bits) {
    std::string names;
    const auto add = [&](const char* n) { names += names.empty() ? n : std::string(", ") + n; };
    if (bits & 1) add("script/modal");
    if (bits & 2) add("minimized");
    if (bits & 4) add("device lost/not in front");
    if (bits & ~7u) add("other");
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%X", bits);
    return names.empty() ? hex : std::string(hex) + " (" + names + ")";
}

// Warnings of the pause watch, from one clock sample a second:
//  ClockStuck  the game time (+0x60) has not moved for 20 s while no pause bit is set and the speed level is above 0;
//  BitsHeld    pause bits stayed set for 20 s in a loaded world (once per bit set, again after they cleared).
struct ClockWatch {
    enum : int { None = 0, ClockStuck = 1, BitsHeld = 2 };
    static constexpr unsigned long long kMs = 20000;
    float lastTime = 0;
    unsigned long long since = 0, bitsSince = 0;
    bool started = false, stuckWarned = false, wasRunning = false, bitsOn = false;
    uint32_t bitsWarned = 0;
    int Update(unsigned long long now, float time, uint32_t bits, int level, bool live) {
        int out = None;
        const bool shouldRun = bits == 0 && level > 0;
        if (!started || time != lastTime || !shouldRun || !wasRunning) { since = now; stuckWarned = false; }
        started = true;
        wasRunning = shouldRun;
        lastTime = time;
        if (shouldRun && !stuckWarned && now - since >= kMs) { stuckWarned = true; out |= ClockStuck; }
        if (!bits || !live) { bitsOn = false; bitsWarned = 0; }
        else {
            if (!bitsOn) { bitsOn = true; bitsSince = now; }
            if (bits != bitsWarned && now - bitsSince >= kMs) { bitsWarned = bits; out |= BitsHeld; }
        }
        return out;
    }
};

// Copies of a command-line switch ("-name:" or "-name=", exact case) in a block of memory, as ASCII and as UTF-16; "on"
// counts those whose value starts with t or 1, as the game reads it
struct ArgCopies { int ascii = 0, asciiOn = 0, wide = 0, wideOn = 0; };
inline void CountArg(const uint8_t* p, size_t n, const char* name, ArgCopies& c) {
    const size_t len = std::strlen(name);
    const auto on = [](unsigned ch) { return ch == 't' || ch == 'T' || ch == '1'; };
    for (size_t i = 0; i + len + 2 < n; ++i) {
        if (p[i] != '-') continue;
        if (std::memcmp(p + i + 1, name, len) == 0 && (p[i + 1 + len] == ':' || p[i + 1 + len] == '=')) {
            ++c.ascii;
            if (on(p[i + 2 + len])) ++c.asciiOn;
            continue;
        }
        if (p[i + 1] != 0 || i + 2 * len + 6 > n) continue;
        bool hit = true;
        for (size_t k = 0; k < len && hit; ++k) hit = p[i + 2 + 2 * k] == static_cast<uint8_t>(name[k]) && p[i + 3 + 2 * k] == 0;
        const size_t v = i + 2 + 2 * len;
        if (!hit || !((p[v] == ':' || p[v] == '=') && p[v + 1] == 0)) continue;
        ++c.wide;
        if (on(p[v + 2]) && p[v + 3] == 0) ++c.wideOn;
    }
}

// A file's last write against the game's start (both FILETIME ticks of 100 ns); recent = in the 10 min before the start
inline std::string Footprint(long long write, long long start, bool& recent) {
    const long long s = (start - write) / 10000000;
    recent = write <= start && s <= 600;
    char out[64];
    if (write > start) std::snprintf(out, sizeof out, "written %lld s after the start", (write - start) / 10000000);
    else if (recent) std::snprintf(out, sizeof out, "written %lld s before the start (in the 10 min before it)", s);
    else if (s < 3600) std::snprintf(out, sizeof out, "written %lld min before the start", s / 60);
    else if (s < 172800) std::snprintf(out, sizeof out, "written %lld h before the start", s / 3600);
    else std::snprintf(out, sizeof out, "written %lld days before the start", s / 86400);
    return out;
}

inline std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

inline std::string LowerA(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// "section.key = value" for each key of a TOML or INI text whose section or key name holds one of `words` (any case;
// every key when `words` is empty), at most `cap`; `total` gets the number that matched
inline std::vector<std::string> ConfigKeys(const std::string& text, const std::vector<std::string>& words, size_t cap, size_t* total = nullptr) {
    std::vector<std::string> out;
    std::string section;
    size_t n = 0;
    for (size_t s = 0; s < text.size();) {
        size_t e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        const std::string l = Trim(text.substr(s, e - s));
        s = e + 1;
        if (l.empty() || l[0] == '#' || l[0] == ';') continue;
        if (l[0] == '[') {
            section = l.substr(l.find_first_not_of('['));
            section.resize(section.find(']') == std::string::npos ? section.size() : section.find(']'));
            continue;
        }
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(l.substr(0, eq)), name = LowerA(section + "." + key);
        bool hit = words.empty();
        for (const auto& w : words) hit = hit || name.find(LowerA(w)) != std::string::npos;
        if (!hit) continue;
        if (++n > cap) continue;
        std::string value = Trim(l.substr(eq + 1));
        if (value.size() > 80) value = value.substr(0, 80) + "...";
        out.push_back((section.empty() ? key : section + "." + key) + " = " + value);
    }
    if (total) *total = n;
    return out;
}

// A top-level value of a flat JSON object (the launcher's settings.json): the scalar as written, "{}" or "{...}" for an
// object, empty when the key is not there
inline std::string JsonValue(const std::string& text, const std::string& key) {
    const size_t at = text.find("\"" + key + "\"");
    if (at == std::string::npos) return {};
    size_t v = text.find(':', at + key.size() + 2);
    if (v == std::string::npos) return {};
    v = text.find_first_not_of(" \t\r\n", v + 1);
    if (v == std::string::npos) return {};
    if (text[v] == '{' || text[v] == '[') {
        const size_t next = text.find_first_not_of(" \t\r\n", v + 1);
        const char close = text[v] == '{' ? '}' : ']';
        return next != std::string::npos && text[next] == close ? std::string{text[v], close} : std::string{text[v]} + "..." + close;
    }
    const size_t end = text.find_first_of(",}\r\n", v);
    return Trim(text.substr(v, end == std::string::npos ? std::string::npos : end - v));
}

// The last `n` entries of the launcher's log (App.Log: a "yyyy-mm-ddThh:mm:ss Launcher <version> <context>" line, then
// the error's text): each header with the first line after it, cut to 240 characters
inline std::vector<std::string> LogEntries(const std::string& text, size_t n) {
    std::vector<std::string> lines, out;
    for (size_t s = 0; s < text.size();) {
        size_t e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        lines.push_back(Trim(text.substr(s, e - s)));
        s = e + 1;
    }
    const auto header = [](const std::string& l) {
        return l.size() > 20 && isdigit(static_cast<unsigned char>(l[0])) && l[4] == '-' && l[10] == 'T' && l.find(" Launcher ") == 19;
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!header(lines[i])) continue;
        std::string entry = lines[i];
        if (i + 1 < lines.size() && !lines[i + 1].empty() && !header(lines[i + 1])) entry += " | " + lines[i + 1];
        if (entry.size() > 240) entry = entry.substr(0, 240) + "...";
        out.push_back(entry);
    }
    if (out.size() > n) out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(n));
    return out;
}

// The script/modal pause bit (GameClock +0x40 bit 0x1) over the run, sampled at the pump's rate (10 ms): how often it
// was set and cleared, since when it is set, and whether it was already set at the first sample (during the load)
struct PauseTrack {
    bool started = false, setAtFirst = false;
    uint32_t bits = 0;
    int sets = 0, clears = 0, otherChanges = 0;
    unsigned long long since = 0, lastClear = 0; // since: when bit 0x1 was set (while it is)
    enum Event { None, Set, Cleared };
    Event Update(unsigned long long now, uint32_t b) {
        if (!started) {
            started = true;
            bits = b;
            setAtFirst = b & 1;
            since = now;
            return None;
        }
        Event ev = None;
        if ((b ^ bits) & 1) {
            if (b & 1) { ++sets; since = now; ev = Set; }
            else { ++clears; lastClear = now; ev = Cleared; }
        }
        if ((b ^ bits) & ~1u) ++otherChanges;
        bits = b;
        return ev;
    }
    bool HeldSinceFirst() const { return setAtFirst && clears == 0; }
};

// The facts the verdict line combines (times in seconds from the game's start; -1 = none)
struct VerdictFacts {
    bool clockKnown = false, timeMoving = false, footprintRead = false, inFront = false;
    uint32_t bits = 0;
    int level = -1;
    float speed = 0;
    long long now = 0, bitSince = -1, lastClear = -1, lastErrorFile = -1, watchStart = 0;
    bool bitHeldSinceFirst = false;
    int sets = 0, clears = 0, otherChanges = 0, speedKeys = 0, keysMoved = 0, levelChanged = 0, errorFiles = 0, osDialogs = 0;
    std::vector<std::string> recentWrites; // "name (N s before)"
};

inline std::string Verdict(const VerdictFacts& f) {
    std::string out;
    const auto add = [&](const std::string& s) { out += (out.empty() ? "" : "; ") + s; };
    char b[200];
    if (!f.clockKnown) add("game clock unavailable");
    else if (f.timeMoving) { std::snprintf(b, sizeof b, "game time moving (level %d, speed %g)", f.level, f.speed); add(b); }
    else if (f.bits) {
        std::string held = "paused by bits " + PauseBits(f.bits);
        if (f.bits & 1) {
            if (f.bitHeldSinceFirst) held += ", bit 0x1 held since the first sample (during the load)";
            else if (f.bitSince >= 0) { std::snprintf(b, sizeof b, ", bit 0x1 set at +%lld s and held %lld s", f.bitSince, f.now - f.bitSince); held += b; }
        }
        add(held);
    } else if (f.level == 0) add("paused by the speed level 0 (no pause bit)");
    else { std::snprintf(b, sizeof b, "game time not moving with no pause bit (level %d, speed %g)", f.level, f.speed); add(b); }
    if (f.clockKnown) {
        std::snprintf(b, sizeof b, "bit 0x1 set %d and cleared %d times%s, other bit changes %d", f.sets, f.clears, f.bitHeldSinceFirst ? " after the first sample" : "", f.otherChanges);
        add(b);
        if (f.lastClear >= 0) { std::snprintf(b, sizeof b, "bit 0x1 last cleared at +%lld s", f.lastClear); add(b); }
    }
    if (f.speedKeys) { std::snprintf(b, sizeof b, "%d speed/pause keys, game time moved after %d, level changed after %d", f.speedKeys, f.keysMoved, f.levelChanged); add(b); }
    else add("no speed or pause key pressed");
    if (f.errorFiles) { std::snprintf(b, sizeof b, "%d error files since the start, last at +%lld s", f.errorFiles, f.lastErrorFile); add(b); }
    else add("no error file since the start");
    std::snprintf(b, sizeof b, "game window %s, OS dialogs %d", f.inFront ? "in front" : "not in front", f.osDialogs);
    add(b);
    if (!f.footprintRead) add("file footprint not read yet");
    else if (f.recentWrites.empty()) add("no checked file written in the 10 min before the start");
    else {
        std::string w = "written in the 10 min before the start:";
        for (size_t i = 0; i < f.recentWrites.size() && i < 12; ++i) w += (i ? ", " : " ") + f.recentWrites[i];
        if (f.recentWrites.size() > 12) w += ", ...";
        add(w);
    }
    std::snprintf(b, sizeof b, " (at +%lld s, watch from +%lld s)", f.now, f.watchStart);
    return "Verdict: " + out + b;
}

} // namespace StartDiagLogic
