// A rig per Sim draw from the skin fill (see gi_sim_rig_hook.h; wp/XR-10.md parts 4-5).
//
// Part of Apex Radiance. Credits: @loinyx

#include "gi_sim_rig_hook.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <windows.h>

namespace GiSimRigHook {
namespace {

// Steam 1.67.2 (static, wp/XR-10.md part 5): the fill and its two callers
constexpr uintptr_t kFill = 0x0068F3F0;
constexpr uintptr_t kSites[2] = {0x006905B4, 0x0070A7D4};
// The fill's first bytes: push ebp; mov ebp,esp; and esp,-16; sub esp,54h; push ebx; push esi; mov ebx,ecx; mov ecx,[ebx+64h]
constexpr uint8_t kFillHead[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x54, 0x53, 0x56, 0x8B, 0xD9, 0x8B, 0x4B, 0x64};

using FillFn = void(__fastcall*)(void* drawable, void* edx, void* poses); // thiscall with one argument, ret 4
const FillFn g_fill = reinterpret_cast<FillFn>(kFill);

std::mutex g_ctrl;
bool g_started = false;
uintptr_t g_patched[2] = {};
std::atomic<bool> g_on{false};
std::atomic<bool> g_loggedPlace{false};
std::atomic<unsigned long long> c_fills{0}, c_rigs{0}, c_faults{0};
GiSimCapsules::SkinRing g_ring;

uintptr_t CallTargetOf(const uint8_t call[5], uintptr_t at) {
    int32_t rel;
    std::memcpy(&rel, call + 1, 4);
    return at + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
}

// The draw's slots, copied under SEH (no C++ objects here). False: a fault or a bound miss
bool ReadSlots(const uint8_t* drawable, int& n, uint32_t hash[60], float bind[60][16], float first[12]) {
    __try {
        const uint32_t count = *reinterpret_cast<const uint32_t*>(drawable + 0x60);
        const int32_t* map58 = *reinterpret_cast<int32_t* const*>(drawable + 0x58);
        const uint8_t* skin = *reinterpret_cast<uint8_t* const*>(drawable + 0x64);
        const float* palette = *reinterpret_cast<float* const*>(drawable + 0x70);
        if (count == 0 || count > 60 || !map58 || !skin || !palette) return false;
        const uint32_t joints = *reinterpret_cast<const uint32_t*>(skin + 0x28);
        const uint32_t* hashes = *reinterpret_cast<uint32_t* const*>(skin + 0x2C);
        const float* binds = *reinterpret_cast<float* const*>(skin + 0x30);
        if (!joints || joints > 1024 || !hashes || !binds) return false;
        for (uint32_t i = 0; i < count; i++) {
            const int32_t j = map58[i];
            if (j < 0 || static_cast<uint32_t>(j) >= joints) return false;
            hash[i] = hashes[j];
            std::memcpy(bind[i], binds + 16 * j, 64);
        }
        std::memcpy(first, palette, 48);
        n = static_cast<int>(count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Note(void* drawable) {
    c_fills.fetch_add(1, std::memory_order_relaxed);
    int n = 0;
    uint32_t hash[60];
    float bind[60][16], first[12];
    if (!ReadSlots(static_cast<const uint8_t*>(drawable), n, hash, bind, first)) {
        c_faults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    GiSimCapsules::SkinRig rig;
    if (!GiSimCapsules::RigFromSlots(hash, bind, n, rig)) return;
    g_ring.Put(first, rig);
    c_rigs.fetch_add(1, std::memory_order_relaxed);
    // once: the torso's bind place (model space: about a metre up for an adult) [O: the smoke reads it]
    if (rig.cap[0].part == GiSimCapsules::kTorso && !g_loggedPlace.exchange(true)) {
        const float* p = rig.joint[rig.cap[0].a].bind;
        LOG_INFO(std::format("[GiSimRigs] First rig: {} slots, {} capsules; pelvis bind place ({:.3f}, {:.3f}, {:.3f}), {:.2f} m from the model origin", n, rig.nc, p[0],
                             p[1], p[2], std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2])));
    }
}

void __fastcall Hook_Fill(void* drawable, void* edx, void* poses) {
    g_fill(drawable, edx, poses);
    if (g_on.load(std::memory_order_acquire)) Note(drawable);
}

} // namespace

bool Start(std::string* why) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    const auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (!GameAddr::IsFixed()) return fail("not Steam 1.67.2 (the skin fill's addresses are known there only)");
    uint8_t head[sizeof kFillHead] = {};
    if (!MemPatch::ReadBytes(kFill, head, sizeof head) || std::memcmp(head, kFillHead, sizeof head) != 0)
        return fail(std::format("the skin fill at {:#010x} differs", kFill));
    uint8_t bytes[2][5];
    for (int i = 0; i < 2; i++) {
        uint8_t cur[5] = {};
        if (!MemPatch::ReadBytes(kSites[i], cur, 5) || cur[0] != 0xE8 || CallTargetOf(cur, kSites[i]) != kFill)
            return fail(std::format("the call at {:#010x} was changed by another module", kSites[i]));
        bytes[i][0] = 0xE8;
        const int32_t rel = MemPatch::CalculateRelativeOffset(kSites[i], reinterpret_cast<uintptr_t>(&Hook_Fill));
        std::memcpy(bytes[i] + 1, &rel, 4);
    }
    g_on.store(true, std::memory_order_release); // before a CALL can reach the wrapper
    for (int i = 0; i < 2; i++) {
        if (MemPatch::WriteCodeSuspended(kSites[i], bytes[i], 5)) {
            g_patched[i] = kSites[i];
            continue;
        }
        g_on.store(false);
        for (int k = 0; k < i; k++) { // undo the first one: both or neither
            uint8_t orig[5] = {0xE8};
            const int32_t rel = MemPatch::CalculateRelativeOffset(kSites[k], kFill);
            std::memcpy(orig + 1, &rel, 4);
            if (MemPatch::WriteCodeSuspended(kSites[k], orig, 5)) g_patched[k] = 0;
        }
        g_started = g_patched[0] || g_patched[1]; // a call left on the wrapper (which only passes on now): Stop retries
        return fail(std::format("could not patch the call at {:#010x}", kSites[i]));
    }
    g_started = true;
    LOG_INFO(std::format("[GiSimRigs] On: the skin fill {:#010x} calls at {:#010x} and {:#010x} go through Apex", kFill, kSites[0], kSites[1]));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release); // the wrapper only passes on from now
    for (uintptr_t& site : g_patched) {
        if (!site) continue;
        uint8_t cur[5] = {};
        if (!MemPatch::ReadBytes(site, cur, 5) || cur[0] != 0xE8 || CallTargetOf(cur, site) != reinterpret_cast<uintptr_t>(&Hook_Fill)) {
            LOG_WARNING(std::format("[GiSimRigs] The call at {:#010x} was changed by another module after Apex; left as it is", site));
            site = 0;
            continue;
        }
        uint8_t orig[5] = {0xE8};
        const int32_t rel = MemPatch::CalculateRelativeOffset(site, kFill);
        std::memcpy(orig + 1, &rel, 4);
        if (MemPatch::WriteCodeSuspended(site, orig, 5)) site = 0;
        else LOG_ERROR(std::format("[GiSimRigs] Could not restore the call at {:#010x}; it stays on Apex's wrapper, which only passes it on now", site));
    }
    g_started = g_patched[0] || g_patched[1];
    LOG_INFO(std::format("[GiSimRigs] Off ({} fills, {} rigs, {} reads refused)", c_fills.load(), c_rigs.load(), c_faults.load()));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

bool Find(int regs, const float c0[12], GiSimCapsules::SkinRig& out) { return g_on.load(std::memory_order_acquire) && g_ring.Find(regs, c0, out); }

Counters Read() { return {c_fills.load(std::memory_order_relaxed), c_rigs.load(std::memory_order_relaxed), c_faults.load(std::memory_order_relaxed)}; }

} // namespace GiSimRigHook
