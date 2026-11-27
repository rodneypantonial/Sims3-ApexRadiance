#pragma once
// Pure parts of the start diagnostics (start_diag.cpp): path masking, the DBPF index check for script resources and the
// change-only line logger. No Windows calls, so tools/start_diag_test checks them offline.
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>

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

} // namespace StartDiagLogic
