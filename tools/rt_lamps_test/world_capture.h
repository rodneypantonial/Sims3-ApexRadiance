#pragma once
// Reader of a Full capture's World.txt (the lights, story grids and placed floors sections), shared by the offline tests
// (tools/rt_lamps_test and the real-time GI checks). Read-only. Quadrants: per tile, bit q of Story::roofed and
// Story::placed is quadrant q (features/rt_lamp_proxy.h).
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "features/rt_lamp_proxy.h"

namespace WorldCapture {

// one light line of the capture (types 3, 4, 5 and 11), world space as written
struct Lamp {
    float pos[3];
    float colour[3] = {1, 0.95f, 0.85f};
    float range = 60.0f, intensity = 1.0f;
    int type = 3;
    float axis[3] = {0, -1, 0}, off = 0, scale = 0; // spot
    float a1[3] = {0, -1, 0}, o1 = 1, o2 = 1, S[3] = {0.3f, 0.3f, 0.3f}; // shaded lamp
    std::string name;
    int room = 0;
};

inline std::vector<float> Floats(const std::string& s) {
    std::vector<float> v;
    std::string t;
    for (char c : s) t += (c == '(' || c == ')' || c == ',' || c == '/') ? ' ' : c;
    std::istringstream in(t);
    std::string w;
    while (in >> w) {
        char* end = nullptr;
        const float f = std::strtof(w.c_str(), &end);
        if (end && *end == 0 && !w.empty()) v.push_back(f);
    }
    return v;
}
inline std::string After(const std::string& line, const std::string& key) {
    const size_t p = line.find(key);
    return p == std::string::npos ? std::string() : line.substr(p + key.size());
}
struct Lot {
    uint64_t id = 0;
    RtLampProxy::Lot lot;
    std::map<int, RtLampProxy::Story> stories;
    bool haveXf = false;
};
struct World {
    std::map<uint64_t, Lot> lots;     // by lot id
    std::multimap<uint64_t, Lamp> lamps; // by lot id: room 0's lamps, or every lamp with allLamps
};

// false when the file cannot be opened
inline bool Read(const std::string& path, World& world, bool allLamps) {
    std::ifstream f(path);
    if (!f) return false;
    std::vector<std::string> lines;
    for (std::string l; std::getline(f, l);) lines.push_back(l);
    std::map<uint64_t, Lot>& lots = world.lots;
    std::multimap<uint64_t, Lamp>& lamps = world.lamps;
    enum { kNone, kLights, kGrids, kFloors } sect = kNone;
    Lot* cur = nullptr;
    RtLampProxy::Story* st = nullptr;
    int floorRows = 0;
    std::vector<float> rowFloors;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& L = lines[i];
        if (L.rfind("==== LIGHTS", 0) == 0) sect = kLights;
        else if (L.rfind("==== STORY GRIDS", 0) == 0) sect = kGrids;
        else if (L.rfind("==== PLACED FLOORS", 0) == 0) sect = kFloors;
        else if (L.rfind("==== ", 0) == 0) sect = kNone;
        if (sect == kLights && L.rfind("#", 0) == 0 && (allLamps || L.find(" room 0 ") != std::string::npos)) {
            Lamp d;
            const size_t tp = L.find(" type ");
            if (tp == std::string::npos) continue;
            d.type = std::atoi(L.c_str() + tp + 6);
            if (d.type != 3 && d.type != 4 && d.type != 5 && d.type != 11) continue;
            const std::string lotHex = After(L, " lot ").substr(0, 16);
            const uint64_t lot = std::strtoull(lotHex.c_str(), nullptr, 16);
            if (!lot) continue;
            const auto pos = Floats(After(L, " pos ").substr(0, After(L, " pos ").find(')') + 1));
            const auto rng = Floats(After(L, " range ").substr(0, After(L, " range ").find(' ')));
            const auto inten = Floats(After(L, " intensity ").substr(0, After(L, " intensity ").find(')') + 1));
            const auto f0 = Floats(After(L, " F0 ").substr(0, After(L, " F0 ").find(')') + 1));
            if (pos.size() < 3 || rng.empty()) continue;
            for (int k = 0; k < 3; k++) d.pos[k] = pos[k];
            d.range = std::clamp(rng[0], 1.0f, 200.0f);
            d.intensity = !inten.empty() && inten[0] > 0.0f ? inten[0] : 1.0f;
            if (f0.size() >= 3)
                for (int k = 0; k < 3; k++) d.colour[k] = std::max(f0[k], 0.0f);
            if (d.type == 4) {
                const auto ax = Floats(After(L, "axis[+0x170] ").substr(0, After(L, "axis[+0x170] ").find(')') + 1));
                const auto off = Floats(After(L, "offset[+0x158] ").substr(0, After(L, "offset[+0x158] ").find(' ')));
                const auto scl = Floats(After(L, "scale[+0x154] "));
                if (ax.size() >= 3 && !off.empty() && !scl.empty()) {
                    for (int k = 0; k < 3; k++) d.axis[k] = ax[k];
                    d.off = off[0], d.scale = scl[0];
                }
            } else if (d.type == 5) {
                const auto a1 = Floats(After(L, "a1[+0x1A0] ").substr(0, After(L, "a1[+0x1A0] ").find(')') + 1));
                const auto o1 = Floats(After(L, "o1[+0x174] ").substr(0, After(L, "o1[+0x174] ").find(' ')));
                const auto o2 = Floats(After(L, "o2[+0x170] ").substr(0, After(L, "o2[+0x170] ").find(' ')));
                const auto S = Floats(After(L, "S[+0x150] "));
                if (a1.size() >= 3 && !o1.empty() && !o2.empty() && S.size() >= 3) {
                    for (int k = 0; k < 3; k++) d.a1[k] = a1[k], d.S[k] = S[k];
                    d.o1 = o1[0], d.o2 = o2[0];
                }
            }
            d.name = L.substr(0, L.find(" vt "));
            d.room = std::atoi(After(L, " room ").c_str());
            lamps.emplace(lot, d);
        }
        if (sect == kGrids && L.rfind("-- grid ", 0) == 0) {
            // "-- grid mgr .. | lot <16 hex> story <s> | <w>x<h> tiles | ..."
            const size_t bar = L.find(" | lot ");
            if (bar == std::string::npos) continue;
            const std::string rest = L.substr(bar + 7);
            const uint64_t lot = std::strtoull(rest.substr(0, 16).c_str(), nullptr, 16);
            const int story = std::atoi(After(rest, " story ").c_str());
            const std::string dims = After(rest, " | ");
            const int w = std::atoi(dims.c_str()), h = std::atoi(dims.c_str() + dims.find('x') + 1);
            if (!lot || story < 0 || story > 7 || w <= 0 || h <= 0 || w > 256 || h > 256) {
                cur = nullptr, st = nullptr;
                continue;
            }
            cur = &lots[lot];
            cur->id = lot;
            st = &cur->stories[story];
            *st = RtLampProxy::Story{};
            st->story = story, st->w = w, st->h = h;
            st->floorY.assign(static_cast<size_t>(w) * h, RtLampProxy::kNoFloor);
            st->roofed.assign(static_cast<size_t>(w) * h, 0);
            st->ceil.assign(static_cast<size_t>(w) * h, RtLampProxy::kNoFloor);
            st->placed.assign(static_cast<size_t>(w) * h, 0);
            floorRows = 0;
            continue;
        }
        if (sect == kGrids && cur && st) {
            if (L.find("world -> lot (mgr+0xE0) rows:") != std::string::npos && !cur->haveXf) {
                const auto v = Floats(After(L, "rows:"));
                if (v.size() >= 16) {
                    for (int r = 0; r < 4; r++)
                        for (int c = 0; c < 4; c++) cur->lot.worldToLot[r][c] = v[r * 4 + c];
                    cur->haveXf = true;
                }
                continue;
            }
            std::istringstream in(L);
            std::string kind;
            in >> kind;
            if (kind == "Z" || kind == "X" || kind == "D0" || kind == "D1") {
                int a = 0, b = 0;
                in >> a >> b;
                RtLampProxy::Edge e;
                if (kind == "Z") e.x0 = static_cast<float>(a), e.z0 = static_cast<float>(b), e.x1 = static_cast<float>(a + 1), e.z1 = static_cast<float>(b);
                else if (kind == "X") e.x0 = static_cast<float>(a), e.z0 = static_cast<float>(b), e.x1 = static_cast<float>(a), e.z1 = static_cast<float>(b + 1);
                else if (kind == "D0") e.x0 = static_cast<float>(a), e.z0 = static_cast<float>(b), e.x1 = static_cast<float>(a + 1), e.z1 = static_cast<float>(b + 1);
                else e.x0 = static_cast<float>(a), e.z0 = static_cast<float>(b + 1), e.x1 = static_cast<float>(a + 1), e.z1 = static_cast<float>(b);
                const auto foot = Floats(After(L, " foot ").substr(0, After(L, " foot ").find(' ')));
                const auto base = Floats(After(L, " base ").substr(0, After(L, " base ").find(' ')));
                const auto top = Floats(After(L, " top ").substr(0, After(L, " top ").find(' ')));
                if (foot.empty() || base.empty() || top.size() < 3) continue;
                e.foot = foot[0], e.base = base[0], e.topL = top[0], e.topM = top[1], e.topR = top[2];
                if (L.find(" opening#") != std::string::npos) {
                    const auto sill = Floats(After(L, " sill ").substr(0, After(L, " sill ").find(' ')));
                    std::string hs = After(L, " head ");
                    hs = hs.substr(0, hs.find(' '));
                    for (char& c : hs)
                        if (c == '.' && false) c = ' ';
                    // "h0..hm..h4"
                    std::vector<float> heads;
                    for (size_t p = 0; p <= hs.size();) {
                        const size_t q = hs.find("..", p);
                        heads.push_back(std::strtof(hs.substr(p, q == std::string::npos ? std::string::npos : q - p).c_str(), nullptr));
                        if (q == std::string::npos) break;
                        p = q + 2;
                    }
                    std::string us = After(L, " u ");
                    us = us.substr(0, us.find(' '));
                    const size_t dd = us.find("..");
                    if (!sill.empty() && heads.size() >= 3 && dd != std::string::npos) {
                        e.open = true;
                        e.o.sill = sill[0];
                        e.o.u0 = std::strtof(us.substr(0, dd).c_str(), nullptr), e.o.u1 = std::strtof(us.substr(dd + 2).c_str(), nullptr);
                        e.o.head[0] = heads[0], e.o.head[2] = heads[1], e.o.head[4] = heads[2];
                        e.o.head[1] = 0.5f * (heads[0] + heads[1]), e.o.head[3] = 0.5f * (heads[1] + heads[2]);
                    }
                }
                st->edges.push_back(e);
            } else if (kind == "z") {
                int z = 0;
                std::string word;
                in >> z >> word; // "floor:"
                rowFloors.clear();
                std::string v;
                while (in >> v) rowFloors.push_back(v == "-" ? RtLampProxy::kNoFloor : std::strtof(v.c_str(), nullptr));
                floorRows = z;
                if (z >= 0 && z < st->h)
                    for (int x = 0; x < st->w && x < static_cast<int>(rowFloors.size()); x++) {
                        const size_t t = static_cast<size_t>(z) * st->w + x;
                        st->floorY[t] = rowFloors[x];
                        if (rowFloors[x] < RtLampProxy::kNoFloor * 0.5f) st->ceil[t] = rowFloors[x] + 3.0f;
                    }
            } else if (kind == "flags:") {
                std::string v;
                for (int x = 0; x < st->w && (in >> v); x++) {
                    const int fl = static_cast<int>(std::strtol(v.c_str(), nullptr, 16));
                    if (floorRows >= 0 && floorRows < st->h) st->roofed[static_cast<size_t>(floorRows) * st->w + x] = static_cast<uint8_t>((fl >> 4) & 0xF);
                }
            }
        }
        if (sect == kFloors && L.rfind("-- mgr ", 0) == 0) {
            const std::string lotHex = After(L, " lot ").substr(0, 16);
            const uint64_t lot = std::strtoull(lotHex.c_str(), nullptr, 16);
            const int story = std::atoi(After(L, " story ").c_str());
            const auto low = Floats(After(L, "lowest floor ").substr(0, After(L, "lowest floor ").find(' ')));
            const std::string dims = After(L, "(lot y) | ");
            const int w = std::atoi(dims.c_str()), d = std::atoi(dims.c_str() + dims.find('x') + 1);
            cur = nullptr, st = nullptr;
            const auto it = lots.find(lot);
            if (it == lots.end() || !it->second.stories.count(story)) continue;
            RtLampProxy::Story& s = it->second.stories[story];
            if (s.w != w || s.h != d) continue;
            if (!low.empty()) s.plane = low[0];
            for (size_t k = i + 1; k < lines.size() && k <= i + 2 + static_cast<size_t>(d); k++) {
                std::istringstream in(lines[k]);
                std::string zw, hex;
                int z = -1;
                if (!(in >> zw) || zw != "z") continue;
                in >> z >> hex;
                if (z < 0 || z >= d) continue;
                for (int x = 0; x < w && x < static_cast<int>(hex.size()); x++) {
                    const char c = hex[x];
                    const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
                    s.placed[static_cast<size_t>(z) * w + x] = static_cast<uint8_t>(v);
                }
            }
        }
    }
    return true;
}

} // namespace WorldCapture
