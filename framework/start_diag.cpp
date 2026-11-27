// Start diagnostics ([Diag] lines): see start_diag.h. Everything here only reads. File and registry IO runs on its own
// background threads or on the start-up pump thread, never on the render or window thread; every list is capped.
#include "start_diag.h"
#include "start_diag_logic.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "game_addresses.h"
#include "hook_guard.h"
#include "world_session.h"
#include <bcrypt.h>
#include <tlhelp32.h>
#include <atomic>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")

namespace StartDiag {
namespace {

using StartDiagLogic::ChangeLog;

constexpr ULONGLONG kWatchMs = 10 * 60 * 1000;  // the pause watch after the world settled
constexpr ULONGLONG kHeartbeatMs = 30000;
constexpr int kStateLines = 300, kKeyLines = 80; // with the start lines about 400 lines in all
constexpr int kLateModules = 64, kErrorFiles = 40;
constexpr size_t kScanPackages = 5000, kListNames = 100, kWalkEntries = 200000;

bool g_on = false;
std::wstring g_profile, g_user;
FILETIME g_processStart{};
std::wstring g_cmdSave; // the save named by -worldFile (file name only), empty without one

std::string M(const std::wstring& s) { return ApexUtil::ToUtf8(StartDiagLogic::Mask(s, g_profile, g_user)); }

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

bool EndsWithI(const std::wstring& s, const wchar_t* tail) {
    const size_t n = wcslen(tail);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}

std::wstring BinDir() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L"\\/") + 1);
    return dir;
}

std::string Version(const std::wstring& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return "-";
    std::string buf(size, '\0');
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data()) || !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return "-";
    return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

// First 12 hex digits of the file's SHA-256 ("-" when it cannot be read)
std::string Sha12(const std::wstring& path) {
    std::string out = "-";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD got = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got) ok = BCryptHashData(hash, buf.data(), got, 0) == 0;
        unsigned char digest[32] = {};
        if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
            out.clear();
            for (int i = 0; i < 6; ++i) out += std::format("{:02x}", digest[i]);
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

unsigned long long Bytes(const WIN32_FIND_DATAW& fd) { return (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow; }
std::string Mb(unsigned long long b) { return std::format("{:.1f} MB", b / 1048576.0); }
std::string When(const FILETIME& ft) {
    FILETIME local{};
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

// ---- 1. Start ----
struct Proc { DWORD pid = 0, parent = 0; std::wstring name; };
std::vector<Proc> Processes() {
    std::vector<Proc> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{sizeof e};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) out.push_back({e.th32ProcessID, e.th32ParentProcessID, e.szExeFile});
    CloseHandle(snap);
    return out;
}
std::wstring ImagePath(DWORD pid) {
    std::wstring path;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD n = static_cast<DWORD>(std::size(buf));
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) path.assign(buf, n);
        CloseHandle(h);
    }
    return path;
}

void LogStart(const EditInGameArg::Scan& scan, const std::string& parent, const std::string& grandparent, DWORD grandparentPid) {
    std::wstring line = GetCommandLineW();
    if (line.size() > 1500) line = line.substr(0, 1500) + L"...";
    LOG_INFO("[Diag] Command line (after the edit-in-game rename): " + M(line));
    const auto all = Processes();
    // (names from the handles DllMain kept: a launcher that exited since still shows, marked "exited")
    LOG_INFO("[Diag] Parent " + M(ApexUtil::ToWide(parent)) + ", grandparent " + M(ApexUtil::ToWide(grandparent)));
    std::string siblings; // the grandparent's other children (a launcher that started the game through a helper)
    int nSiblings = 0;
    for (const auto& p : all)
        if (grandparentPid && p.parent == grandparentPid && p.pid != GetCurrentProcessId() && ++nSiblings <= 20) siblings += std::format(" {} ({})", M(p.name), p.pid);
    if (grandparentPid) LOG_INFO("[Diag] Other running children of the grandparent:" + (siblings.empty() ? std::string(" none") : siblings));
    LOG_INFO(std::format("[Diag] EditInGameArg: -apexDirect {}, editInGameMode found {}, value \"{}\", renamed {}, renamed for \"true0\" {}",
                         scan.marked ? "yes" : "no", scan.found, scan.value, scan.renamed, scan.legacy));
    std::string launchers, related;
    for (const auto& p : all) {
        const std::wstring n = Lower(p.name);
        if (n.find(L"apex") != std::wstring::npos && n.find(L"launcher") != std::wstring::npos) {
            const std::wstring path = ImagePath(p.pid);
            launchers += std::format("\n    {} pid {}, version {}, {}", M(p.name), p.pid, path.empty() ? "-" : Version(path), path.empty() ? "path not readable" : M(path));
        } else if (n.find(L"sims3") != std::wstring::npos || n.find(L"ts3") != std::wstring::npos || n.find(L"eadesktop") != std::wstring::npos ||
                   n.find(L"ealauncher") != std::wstring::npos || n.find(L"origin") != std::wstring::npos || n.find(L"steam") != std::wstring::npos ||
                   n.find(L"apex") != std::wstring::npos) {
            related += std::format(" {} ({})", M(p.name), p.pid);
        }
    }
    LOG_INFO("[Diag] Apex Launcher process:" + (launchers.empty() ? std::string(" not running") : launchers));
    LOG_INFO("[Diag] Related processes:" + (related.empty() ? std::string(" none") : related));
}

// ---- 2. Modules ----
std::mutex g_lateLock;
std::vector<std::wstring> g_late; // modules loaded after the snapshot, from the loader's notification
std::atomic<int> g_lateTotal{0};
size_t g_lateLogged = 0;

struct UnicodeStr { USHORT Length, MaximumLength; PWSTR Buffer; };
struct DllNotify { ULONG Flags; const UnicodeStr* FullDllName; const UnicodeStr* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
using LdrRegister = LONG(NTAPI*)(ULONG, void(CALLBACK*)(ULONG, const DllNotify*, PVOID), PVOID, PVOID*);

// Under the loader lock: copy the name only
void CALLBACK OnDll(ULONG reason, const DllNotify* data, PVOID) {
    if (reason != 1 || !data || !data->BaseDllName || !data->BaseDllName->Buffer) return; // 1 = loaded
    ++g_lateTotal;
    std::lock_guard<std::mutex> lock(g_lateLock);
    if (g_late.size() < kLateModules) g_late.emplace_back(data->BaseDllName->Buffer, data->BaseDllName->Length / sizeof(wchar_t));
}

void RegisterDllNotification() {
    PVOID cookie = nullptr;
    const auto reg = reinterpret_cast<LdrRegister>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
    if (!reg || reg(0, OnDll, nullptr, &cookie) != 0) LOG_INFO("[Diag] Later modules: no loader notification, not listed");
}

void LogModules() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) { LOG_INFO("[Diag] Modules: no snapshot"); return; }
    const std::wstring bin = Lower(BinDir());
    std::string text, before;
    int count = 0;
    bool apexSeen = false;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&LogModules), &self);
    MODULEENTRY32W e{sizeof e};
    for (BOOL ok = Module32FirstW(snap, &e); ok; ok = Module32NextW(snap, &e)) { // in load order
        ++count;
        const std::wstring path = e.szExePath;
        const bool asi = EndsWithI(path, L".asi");
        const bool inBin = Lower(path).rfind(bin, 0) == 0;
        const bool isSelf = e.hModule == self;
        if (asi && !apexSeen && !isSelf) before += " " + M(e.szModule);
        if (isSelf) apexSeen = true;
        if (count > 400) continue;
        text += std::format("\n    {:3} {} {} bytes, version {}{}{}", count, M(inBin ? std::wstring(e.szModule) : path), e.modBaseSize, Version(path),
                            asi || inBin ? ", sha256 " + Sha12(path) : std::string(), isSelf ? " (this file)" : "");
    }
    CloseHandle(snap);
    if (count > 400) text += std::format("\n    {} more not listed", count - 400);
    LOG_INFO(std::format("[Diag] Modules at start ({}, load order; Game\\Bin files by name):{}", count, text));
    LOG_INFO("[Diag] ASIs loaded before ApexRadiance.asi:" + (before.empty() ? std::string(" none") : before));
}

void PumpLateModules() {
    std::vector<std::wstring> fresh;
    {
        std::lock_guard<std::mutex> lock(g_lateLock);
        for (; g_lateLogged < g_late.size(); ++g_lateLogged) fresh.push_back(g_late[g_lateLogged]);
    }
    for (const auto& n : fresh) LOG_INFO("[Diag] Module loaded later: " + M(n));
    static bool capNoted = false;
    if (!capNoted && g_lateTotal.load() > static_cast<int>(kLateModules)) {
        capNoted = true;
        LOG_INFO(std::format("[Diag] Later modules: more than {} loaded, further ones are not listed", kLateModules));
    }
}

// ---- 3. Bin and Mods ----
void LogBin() {
    const std::wstring dir = BinDir();
    std::string text;
    int n = 0;
    for (const wchar_t* pattern : {L"*.asi", L"*.dll", L"*.ini", L"*.toml", L"*.cfg"}) {
        WIN32_FIND_DATAW fd{};
        const HANDLE h = FindFirstFileW((dir + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (++n <= 200) text += std::format(" {} ({})", M(fd.cFileName), Bytes(fd));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (n > 200) text += std::format(" ... {} more not listed", n - 200);
    LOG_INFO(std::format("[Diag] Game\\Bin asi/dll/ini/toml/cfg ({}):{}", n, text));
}

std::string ReadText(const std::wstring& path, size_t cap) {
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    out.resize(cap);
    DWORD got = 0;
    if (!ReadFile(f, out.data(), static_cast<DWORD>(cap), &got, nullptr)) got = 0;
    out.resize(got);
    CloseHandle(f);
    return out;
}

StartDiagLogic::Has ScanPackage(const std::wstring& path) {
    using StartDiagLogic::Has;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return Has::Bad;
    Has result = Has::Bad;
    uint8_t head[StartDiagLogic::kDbpfHeader];
    DWORD got = 0;
    uint32_t count = 0, pos = 0, size = 0;
    if (ReadFile(f, head, sizeof head, &got, nullptr) && StartDiagLogic::DbpfIndex(head, got, count, pos, size) && size <= (16u << 20)) {
        std::vector<uint8_t> idx(size);
        LARGE_INTEGER at{};
        at.QuadPart = pos;
        if (SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && (!size || (ReadFile(f, idx.data(), size, &got, nullptr) && got == size)))
            result = StartDiagLogic::IndexHasType(idx.data(), idx.size(), count, StartDiagLogic::kS3SA);
    }
    CloseHandle(f);
    return result;
}

void LogMods() {
    const std::wstring docs = ApexPaths::GameDocumentsDirectory();
    if (docs.empty()) { LOG_INFO("[Diag] Mods: Documents folder not found"); return; }
    const std::wstring mods = docs + L"Mods\\";
    std::string cfg = ReadText(mods + L"Resource.cfg", 4000);
    for (auto& c : cfg) if (c == '\r') c = ' ';
    std::string cfgLines;
    for (size_t s = 0; s < cfg.size();) {
        const size_t e = cfg.find('\n', s);
        const std::string l = cfg.substr(s, e == std::string::npos ? std::string::npos : e - s);
        if (l.find_first_not_of(' ') != std::string::npos) cfgLines += "\n    " + M(ApexUtil::ToWide(l));
        if (e == std::string::npos) break;
        s = e + 1;
    }
    LOG_INFO("[Diag] Mods\\Resource.cfg:" + (cfgLines.empty() ? std::string(" not found or empty") : cfgLines));

    static const wchar_t* kFlags[] = {L"monopatcher", L"nraas", L"overwatch", L"storyprogression", L"mastercontroller", L"errortrap", L"simler90", L"core", L"pause"};
    struct Dir { std::wstring rel; int depth; };
    std::vector<Dir> stack{{L"", 0}};
    std::vector<std::wstring> packages;
    unsigned long long bytes = 0;
    size_t entries = 0, other = 0;
    int maxDepth = 0;
    std::string overrides, flagged;
    size_t overrideCount = 0, flaggedCount = 0;
    while (!stack.empty() && entries < kWalkEntries) {
        const Dir d = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd{};
        const HANDLE h = FindFirstFileW((mods + d.rel + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            ++entries;
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring rel = d.rel + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (d.depth + 1 > maxDepth) maxDepth = d.depth + 1;
                stack.push_back({rel + L"\\", d.depth + 1});
                continue;
            }
            if (!EndsWithI(name, L".package")) { ++other; continue; }
            packages.push_back(rel);
            bytes += Bytes(fd);
            const std::wstring lower = Lower(rel);
            if (lower.rfind(L"overrides\\", 0) == 0 && ++overrideCount <= kListNames) overrides += " " + M(rel);
            for (const wchar_t* f : kFlags)
                if (Lower(name).find(f) != std::wstring::npos) {
                    if (++flaggedCount <= kListNames) flagged += " " + M(rel);
                    break;
                }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    LOG_INFO(std::format("[Diag] Mods: {} packages, {}, folder depth {}, {} other files{}", packages.size(), Mb(bytes), maxDepth, other,
                         entries >= kWalkEntries ? std::format(" (walk stopped at {} entries)", kWalkEntries) : std::string()));
    LOG_INFO(std::format("[Diag] Mods\\Overrides packages ({}):{}{}", overrideCount, overrides.empty() ? std::string(" none") : overrides,
                         overrideCount > kListNames ? std::format(" ... {} more not listed", overrideCount - kListNames) : std::string()));
    LOG_INFO(std::format("[Diag] Mods with flagged names ({}):{}{}", flaggedCount, flagged.empty() ? std::string(" none") : flagged,
                         flaggedCount > kListNames ? std::format(" ... {} more not listed", flaggedCount - kListNames) : std::string()));

    std::string scripts;
    size_t scriptCount = 0, bad = 0;
    const size_t scan = packages.size() < kScanPackages ? packages.size() : kScanPackages;
    for (size_t i = 0; i < scan; ++i) {
        const auto r = ScanPackage(mods + packages[i]);
        if (r == StartDiagLogic::Has::Bad) ++bad;
        else if (r == StartDiagLogic::Has::Yes && ++scriptCount <= 200) scripts += "\n    " + M(packages[i]);
    }
    LOG_INFO(std::format("[Diag] Script mods (S3SA in the package index): {} script mods of {} packages scanned{}{}, {} unreadable or not DBPF 2:{}", scriptCount, scan,
                         packages.size() > scan ? std::format(" ({} not scanned)", packages.size() - scan) : std::string(),
                         scriptCount > 200 ? std::format(", {} not listed", scriptCount - 200) : std::string(), bad, scripts.empty() ? std::string(" none") : scripts));

    for (const auto& [folder, ext] : {std::pair{L"DCCache\\", L"*.dbc"}, std::pair{L"Downloads\\", L"*.sims3pack"}}) {
        WIN32_FIND_DATAW fd{};
        size_t n = 0;
        unsigned long long b = 0;
        const HANDLE h = FindFirstFileW((docs + folder + ext).c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { ++n; b += Bytes(fd); } } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        LOG_INFO(std::format("[Diag] {}{}: {} files, {}", ApexUtil::ToUtf8(folder), ApexUtil::ToUtf8(ext), n, Mb(b)));
    }
}

// ---- 4. Save ----
struct SaveInfo { std::wstring name; unsigned long long bytes = 0; size_t files = 0; FILETIME write{}, access{}; };
SaveInfo ReadSave(const std::wstring& saves, const std::wstring& name) {
    SaveInfo s;
    s.name = name;
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((saves + name + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return s;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (++s.files > 5000) break;
        s.bytes += Bytes(fd);
        if (CompareFileTime(&fd.ftLastWriteTime, &s.write) > 0) s.write = fd.ftLastWriteTime;
        if (CompareFileTime(&fd.ftLastAccessTime, &s.access) > 0) s.access = fd.ftLastAccessTime;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return s;
}

std::wstring CommandLineSave() {
    const std::wstring line = GetCommandLineW();
    const size_t at = Lower(line).find(L"-worldfile:");
    if (at == std::wstring::npos) return {};
    std::wstring v = line.substr(at + 11);
    const size_t end = v.find_first_of(L"\"");
    if (end != std::wstring::npos) v.resize(end);
    const size_t dash = v.find(L" -");
    if (dash != std::wstring::npos) v.resize(dash);
    while (!v.empty() && v.back() == L' ') v.pop_back();
    const size_t slash = v.find_last_of(L"\\/");
    return slash == std::wstring::npos ? v : v.substr(slash + 1);
}

// After the world settled: the save folder read most recently (last-access times; Windows may not update them)
DWORD WINAPI SaveCheckThread(LPVOID) {
    HookGuard::Try("Diag: save check", [] {
        const std::wstring saves = ApexPaths::GameDocumentsDirectory() + L"Saves\\";
        std::vector<SaveInfo> all;
        WIN32_FIND_DATAW fd{};
        const HANDLE h = FindFirstFileW((saves + L"*.sims3").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && all.size() < 200) all.push_back(ReadSave(saves, fd.cFileName)); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        const SaveInfo* recent = nullptr;
        for (const auto& s : all) if (!recent || CompareFileTime(&s.access, &recent->access) > 0) recent = &s;
        const auto describe = [&](const SaveInfo& s) {
            const DWORD backup = GetFileAttributesW((saves + s.name + L".backup").c_str());
            return std::format("{}: {} files, {}, last write {}, last read {}, backup {}", M(s.name), s.files, Mb(s.bytes), When(s.write), When(s.access),
                               backup != INVALID_FILE_ATTRIBUTES ? "yes" : "no");
        };
        LOG_INFO(std::format("[Diag] Saves folder {}: {} saves", M(saves), all.size()));
        const SaveInfo* named = nullptr;
        for (const auto& s : all) if (!g_cmdSave.empty() && _wcsicmp(s.name.c_str(), g_cmdSave.c_str()) == 0) named = &s;
        if (!g_cmdSave.empty()) LOG_INFO(named ? "[Diag] Save on the command line: " + describe(*named) : "[Diag] Save on the command line not in the Saves folder: " + M(g_cmdSave));
        if (recent)
            LOG_INFO(std::format("[Diag] Save read most recently: {}{}", describe(*recent),
                                 g_cmdSave.empty() ? "" : (named == recent ? " (the command line's)" : " (NOT the command line's)")));
    });
    return 0;
}

// ---- 5. Packs and options ----
void LogPacksAndOptions() {
    const std::wstring docs = ApexPaths::GameDocumentsDirectory();
    const std::wstring ini = docs + L"LoadOption.ini";
    wchar_t sel[64] = {}, played[64] = {};
    GetPrivateProfileStringW(L"productoption", L"selectedflags", L"(none)", sel, 64, ini.c_str());
    GetPrivateProfileStringW(L"productoption", L"playedflags", L"(none)", played, 64, ini.c_str());
    LOG_INFO("[Diag] LoadOption.ini selectedflags " + ApexUtil::ToUtf8(sel) + ", playedflags " + ApexUtil::ToUtf8(played));
    std::string text = ReadText(docs + L"Options.ini", 64 * 1024), keys;
    for (size_t s = 0; s < text.size();) {
        const size_t e = text.find('\n', s);
        std::string l = text.substr(s, e == std::string::npos ? std::string::npos : e - s);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        std::string lower = l;
        for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        for (const char* k : {"pause", "focus", "mute", "autosave", "speed", "time", "background", "edgescroll", "story"})
            if (lower.find(k) != std::string::npos && lower.find('=') != std::string::npos) { keys += " " + l; break; }
        if (e == std::string::npos) break;
        s = e + 1;
    }
    LOG_INFO("[Diag] Options.ini pause/focus/time keys:" + (keys.empty() ? std::string(" none found") : keys));
    std::string packs;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Sims", 0, KEY_READ | KEY_WOW64_32KEY, &key) == ERROR_SUCCESS) {
        wchar_t name[256];
        for (DWORD i = 0; i < 60; ++i) {
            DWORD n = 256;
            if (RegEnumKeyExW(key, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            packs += std::string(packs.empty() ? " " : ", ") + ApexUtil::ToUtf8(std::wstring(name, n));
        }
        RegCloseKey(key);
    }
    LOG_INFO("[Diag] Installed packs (registry SOFTWARE\\Sims):" + (packs.empty() ? std::string(" none found") : packs));
}

// ---- 7. Script errors ----
std::set<std::wstring> g_errorsSeen;
int g_errorLines = 0;
void CheckErrorFiles(bool first) {
    const std::wstring docs = ApexPaths::GameDocumentsDirectory();
    if (docs.empty()) return;
    int fresh = 0;
    for (const wchar_t* pattern : {L"ScriptError_*.xml", L"xcpt*.txt"}) {
        WIN32_FIND_DATAW fd{};
        const HANDLE h = FindFirstFileW((docs + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (CompareFileTime(&fd.ftLastWriteTime, &g_processStart) < 0 || !g_errorsSeen.insert(fd.cFileName).second) continue;
            ++fresh;
            if (++g_errorLines > kErrorFiles) continue;
            std::string text = ReadText(docs + fd.cFileName, 8192), pick;
            for (size_t s = 0; s < text.size() && pick.empty();) {
                const size_t e = text.find('\n', s);
                std::string l = text.substr(s, e == std::string::npos ? std::string::npos : e - s);
                while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
                const size_t b = l.find_first_not_of(" \t");
                if (b != std::string::npos && l.rfind("<?xml", b) != b) pick = l.substr(b);
                if (e == std::string::npos) break;
                s = e + 1;
            }
            if (pick.size() > 200) pick.resize(200);
            LOG_INFO(std::format("[Diag] New error file {} ({}): {}", M(fd.cFileName), When(fd.ftLastWriteTime), M(ApexUtil::ToWide(pick))));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (g_errorLines > kErrorFiles && fresh) LOG_INFO(std::format("[Diag] Error files: {} new in all, further ones are not listed", g_errorsSeen.size()));
    if (first && !fresh) LOG_INFO("[Diag] No ScriptError or xcpt file written since the game started");
}

std::mutex g_errorLock; // the background thread's first check and the pump's later ones

DWORD WINAPI BackgroundThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN); // low CPU and IO priority next to the game's load
    HookGuard::Try("Diag: modules", [] { LogModules(); });
    HookGuard::Try("Diag: Game\\Bin", [] { LogBin(); });
    HookGuard::Try("Diag: packs and options", [] { LogPacksAndOptions(); });
    HookGuard::Try("Diag: error files", [] { std::lock_guard<std::mutex> l(g_errorLock); CheckErrorFiles(true); });
    HookGuard::Try("Diag: mods", [] { LogMods(); });
    LOG_INFO("[Diag] Background listing done");
    return 0;
}

// ---- 6. Pause watch ----
std::atomic<HWND> g_window{nullptr};
std::atomic<int> g_mode{-1}, g_active{-1};
std::atomic<float> g_gameTime{0}; // GameClock +0x60, from the pump
std::atomic<bool> g_clockKnown{false};
struct Press { WPARAM vk; ULONGLONG at; int mode, active; float time; bool clock; };
std::mutex g_keyLock;
std::vector<Press> g_presses; // waiting for their 3 s check
std::atomic<int> g_keyCount[6];
constexpr WPARAM kKeys[6] = {'1', '2', '3', '0', 'P', VK_SPACE};
int g_keyLines = 0;

bool ReadWorld(int& mode, int& active) {
    const uintptr_t global = GameAddr::Get(GameAddr::Id::WorldManagerPtr);
    if (!global) return false;
    __try {
        const uintptr_t world = *reinterpret_cast<const uintptr_t*>(global);
        if (!world) { mode = -1; active = -1; return true; }
        mode = *reinterpret_cast<const int*>(world + 0x1B4);
        active = *reinterpret_cast<const unsigned char*>(world + 0x41);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The game clock (GameAddr::GameClock, the object itself): +0x40 pause bits, +0x5C advance count (also at the UI
// pause), +0x60 game time (moves only with no bit set and a speed above 0), +0x64 game dt, +0x68 speed, +0x6C UI level 0-3
struct Clock { uint32_t bits = 0, advances = 0; float time = 0, dt = 0, speed = 0; int level = -1; };
bool ReadClock(Clock& c) {
    const uintptr_t clock = GameAddr::Get(GameAddr::Id::GameClock);
    if (!clock) return false;
    __try {
        c.bits = *reinterpret_cast<const uint32_t*>(clock + 0x40);
        c.advances = *reinterpret_cast<const uint32_t*>(clock + 0x5C);
        c.time = *reinterpret_cast<const float*>(clock + 0x60);
        c.dt = *reinterpret_cast<const float*>(clock + 0x64);
        c.speed = *reinterpret_cast<const float*>(clock + 0x68);
        c.level = *reinterpret_cast<const int*>(clock + 0x6C);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
StartDiagLogic::ClockWatch g_clockWatch;

struct WindowCount { int others = 0, dialogs = 0; HWND window = nullptr; };
BOOL CALLBACK CountWindow(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<WindowCount*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || h == c->window || !IsWindowVisible(h)) return TRUE;
    ++c->others;
    wchar_t cls[16] = {};
    GetClassNameW(h, cls, 16);
    if (wcscmp(cls, L"#32770") == 0 || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_DLGMODALFRAME)) ++c->dialogs;
    return TRUE;
}

const char* KeyName(WPARAM vk) { return vk == VK_SPACE ? "Space" : vk == 'P' ? "P" : vk == '0' ? "0" : vk == '1' ? "1" : vk == '2' ? "2" : "3"; }

std::string KeyTotals() {
    std::string s;
    for (int i = 0; i < 6; ++i) s += std::format("{}{}={}", i ? " " : "", KeyName(kKeys[i]), g_keyCount[i].load());
    return s;
}

ULONGLONG g_watchStart = 0, g_lastSample = 0, g_lastErrorCheck = 0;
bool g_watchDone = false;
WorldSession::Settled g_settled;
ChangeLog g_state(kStateLines);

void PumpWatch(ULONGLONG now) {
    int mode = -1, active = -1;
    const bool known = ReadWorld(mode, active);
    g_mode = mode;
    g_active = active;
    Clock clock;
    const bool clockKnown = ReadClock(clock);
    g_clockKnown = clockKnown;
    g_gameTime = clock.time;
    if (!g_watchStart) {
        // live (active) for 3 s, or edit-in-game mode 2, where time is stopped and the active byte may stay 0; mode 1 with
        // active 0 is still the load
        g_settled.Update(known && (WorldSession::Active(static_cast<unsigned char>(active), mode) || mode == 2), now);
        if (!g_settled.ready) return;
        g_watchStart = now;
        LOG_INFO(std::format("[Diag] Pause watch: world live for 3 s (mode {}, active {}), watching 10 min; game clock {}", mode, active,
                             clockKnown ? std::format("at 0x{:08X}", GameAddr::Get(GameAddr::Id::GameClock)) : std::string("unavailable (signature not found once)")));
        if (HANDLE t = CreateThread(nullptr, 0, SaveCheckThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    if (now - g_watchStart > kWatchMs) {
        g_watchDone = true;
        LOG_INFO("[Diag] Pause watch ended (10 min); keys " + KeyTotals());
        return;
    }
    // speed keys pressed 3 s ago: did the world mode or active byte change since
    std::vector<Press> due;
    {
        std::lock_guard<std::mutex> lock(g_keyLock);
        for (size_t i = 0; i < g_presses.size();)
            if (now - g_presses[i].at >= 3000) { due.push_back(g_presses[i]); g_presses.erase(g_presses.begin() + i); } else ++i;
    }
    for (const auto& p : due) {
        if (++g_keyLines > kKeyLines) continue;
        const std::string moved = p.clock && clockKnown ? std::format(", game time +{:.3f} in 3 s{}", clock.time - p.time, clock.time == p.time ? " (did not move)" : "")
                                                        : std::string(", game time unavailable");
        LOG_INFO(std::format("[Diag] Key {} at mode {} active {}; 3 s later mode {} active {}{}{}", KeyName(p.vk), p.mode, p.active, mode, active, moved,
                             g_keyLines == kKeyLines ? " (last key line)" : ""));
    }
    if (now - g_lastSample < 1000) return;
    g_lastSample = now;
    const HWND window = g_window.load();
    WindowCount c;
    c.window = window;
    EnumWindows(CountWindow, reinterpret_cast<LPARAM>(&c));
    const HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    const bool stopped = !(clock.speed > 1.2e-7f || clock.speed < -1.2e-7f);
    const std::string clockState = clockKnown ? std::format(", bits {}, level {}, speed {:g}{}", StartDiagLogic::PauseBits(clock.bits), clock.level, clock.speed, stopped ? " (stopped)" : "")
                                              : std::string(", clock unavailable");
    // (the change key leaves out the times, which move every second while the game runs)
    const std::string state = std::format("mode {} active {}, game window {}{}, OS windows of the game process {} (OS dialogs {}; in-game dialogs not counted){}", known ? mode : -9, active,
                                          !window ? "not seen yet" : fg == window ? "in front" : fgPid == GetCurrentProcessId() ? "behind its own window" : "not in front",
                                          window && IsIconic(window) ? ", minimized" : "", c.others, c.dialogs, clockState);
    const std::string times = clockKnown ? std::format("; time +0x60 {:.3f}, dt {:g}, +0x5C {}", clock.time, clock.dt, clock.advances) : std::string();
    if (clockKnown) {
        const int warn = g_clockWatch.Update(now, clock.time, clock.bits, clock.level, true);
        if (warn & StartDiagLogic::ClockWatch::ClockStuck)
            LOG_WARNING(std::format("[Diag] Clock stuck while unpaused: game time +0x60 has not moved for 20 s with no pause bit, level {}, speed {:g}", clock.level, clock.speed));
        if (warn & StartDiagLogic::ClockWatch::BitsHeld)
            LOG_WARNING("[Diag] Pause bits held for 20 s in the loaded world: " + StartDiagLogic::PauseBits(clock.bits));
    }
    switch (g_state.Next(state, now, kHeartbeatMs)) {
    case ChangeLog::Change: LOG_INFO(std::format("[Diag] +{} s: {}{}", (now - g_watchStart) / 1000, state, times)); break;
    case ChangeLog::Heartbeat: LOG_INFO(std::format("[Diag] +{} s (same): {}{}; keys {}", (now - g_watchStart) / 1000, state, times, KeyTotals())); break;
    case ChangeLog::CapReached: LOG_INFO("[Diag] Pause watch: line cap reached, only the end line follows"); break;
    default: break;
    }
}

} // namespace

void Start(const EditInGameArg::Scan& scan, const std::string& parent, const std::string& grandparent, DWORD grandparentPid) {
    g_on = GetFileAttributesW((ApexPaths::ApexDirectory() + L"diag_off.txt").c_str()) == INVALID_FILE_ATTRIBUTES;
    if (!g_on) { LOG_INFO("[Diag] build diag-1127b: off (diag_off.txt)"); return; }
    LOG_INFO("[Diag] build diag-1127b: start diagnostics on (read-only; a file diag_off.txt in the Apex folder turns them off)");
    g_profile = Env(L"USERPROFILE");
    g_user = Env(L"USERNAME");
    FILETIME a, b, c;
    GetProcessTimes(GetCurrentProcess(), &g_processStart, &a, &b, &c);
    g_cmdSave = CommandLineSave();
    RegisterDllNotification();
    HookGuard::Try("Diag: start", [&] { LogStart(scan, parent, grandparent, grandparentPid); });
    if (HANDLE t = CreateThread(nullptr, 0, BackgroundThread, nullptr, 0, nullptr)) CloseHandle(t);
}

void Pump() {
    if (!g_on) return;
    const ULONGLONG now = GetTickCount64();
    PumpLateModules();
    if (!g_watchDone) PumpWatch(now);
    if (!g_watchDone && now - g_lastErrorCheck >= kHeartbeatMs) {
        g_lastErrorCheck = now;
        std::unique_lock<std::mutex> l(g_errorLock, std::try_to_lock);
        if (l.owns_lock()) CheckErrorFiles(false);
    }
}

void NoteWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_on) return;
    if (!g_window.load()) g_window = GetAncestor(hwnd, GA_ROOT);
    if (msg != WM_KEYDOWN || (lp & (1 << 30))) return; // auto-repeat not counted
    for (int i = 0; i < 6; ++i) {
        if (wp != kKeys[i]) continue;
        ++g_keyCount[i];
        std::lock_guard<std::mutex> lock(g_keyLock);
        if (g_presses.size() < 16) g_presses.push_back({wp, GetTickCount64(), g_mode.load(), g_active.load(), g_gameTime.load(), g_clockKnown.load()});
    }
}

} // namespace StartDiag
