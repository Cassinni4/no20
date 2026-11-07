#include "nu20.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace no20 {

std::string hex(int64_t v) {
    char buf[32];
    if (v < 0) std::snprintf(buf, sizeof buf, "-0x%llx", (unsigned long long)-v);
    else std::snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)v);
    return buf;
}

bool plausibleTag(const uint8_t* p) {
    for (int i = 0; i < 4; i++) {
        uint8_t c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

Nu20File Nu20File::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cant open " + path);
    Nu20File f;
    f.data.assign(std::istreambuf_iterator<char>(in), {});
    f.parse();
    return f;
}

void Nu20File::save(const std::string& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw Error("cant write " + path);
    out.write((const char*)data.data(), (std::streamsize)data.size());
    if (!out) throw Error("couldnt write " + path);
}

void Nu20File::parse() {
    if (data.size() < 0x40) throw Error("file too small");
    if (tagAt(0) == "NU20") {
        base = 0;
    } else {
        int64_t b = (int64_t)s32(0) + 4;
        if (b < 4 || b + 0x40 > (int64_t)data.size() || tagAt((uint32_t)b) != "NU20")
            throw Error("no NU20 at 0 or where word 0 points");
        base = (uint32_t)b;
    }
    if (tagAt(hdr(0x10)) != "HEAD") throw Error("no head chunk at NU20+0x10");

    version = u32(hdr(0x08));
    pntr = (uint32_t)(hdr(0x18) + (int64_t)s32(hdr(0x18)) - 8);
    if ((uint64_t)pntr + 12 > data.size() || tagAt(pntr) != "PNTR")
        throw Error("header [0x18] doesnt lead to PNTR");
    package = pntr + u32(pntr + 4);
    if (package > data.size()) throw Error("PNTR size runs past end of file");
    sceneRoot = (uint32_t)(hdr(0x1C) + (int64_t)s32(hdr(0x1C)));

    uint32_t count = u32(pntr + 8);
    if ((uint64_t)pntr + 12 + (uint64_t)count * 4 > package) throw Error("PNTR count is bigger than PNTR");
    relocs.clear();
    relocs.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = pntr + 12 + i * 4;
        int64_t field = (int64_t)e + s32(e);
        if (field < base || field + 4 > pntr) throw Error("PNTR entry " + std::to_string(i) + " points outside the scene");
        relocs.push_back((uint32_t)field);
    }
    parseChunks();
    findExtras();
}

bool Nu20File::isCharacterModel() const {
    uint32_t g = gsnh();
    return tagAt(sceneRoot - 8) == "GSNH" && g + 0x194 <= pntr && s32(g + 0x18C) != 0;
}

// for locators GSNH +0x17C count, +0x180 -> 0x50 byte records       name is a 16 bit field at +0x40, field + s16 + 0x2C
static constexpr uint32_t kLocatorStride = 0x50;

void Nu20File::findExtras() {
    extras.clear();
    if (!isCharacterModel()) return;
    uint32_t g = gsnh();
    int32_t count = s32(g + 0x17C);
    int32_t rel = s32(g + 0x180);
    if (count <= 0 || count > 4096 || rel == 0) return;
    int64_t array = (int64_t)g + 0x180 + rel;
    for (int32_t i = 0; i < count; i++) {
        int64_t field = array + (int64_t)i * kLocatorStride + 0x40;
        if (field < 0 || field + 4 > pntr) { extras.clear(); return; }
        extras.push_back({(uint32_t)field, 2, 0x2C, "locator name"});
    }
}
std::vector<Texture> Nu20File::textures() const {
    std::vector<Texture> out;
    uint32_t g = gsnhPayload();
    uint32_t count = u32(g + 4);
    int32_t rel = s32(g + 8);
    if (count == 0 || rel == 0) return out;
    if (count > 100000) throw Error("texture count makes no sense, broken file? " + std::to_string(count));
    uint32_t array = (uint32_t)((int64_t)g + 8 + rel);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = array + i * 4;
        uint32_t d = (uint32_t)((int64_t)slot + s32(slot));
        // old 0x28 byte descriptors, size lives in the package
        if (d + (base ? 0x28 : 0x48) > pntr) throw Error("texture descriptor " + std::to_string(i) + " is outside the scene");
        out.push_back({slot, d, u32(d), u32(d + 4), base ? 0 : u32(d + 0x44)});
    }
    if (base != 0) { // [u16 n][n x (0x18 hdr, +0x14 size, dds)] at 4
        uint32_t p = 6;
        for (uint32_t i = 0; i < out.size() && i < u16(4); i++) {
            if ((uint64_t)p + 0x18 > base) break;
            uint32_t sz = u32(p + 0x14);
            if ((uint64_t)p + 0x18 + sz > base) break;
            out[i].size = sz;
            out[i].data = p + 0x18;
            p += 0x18 + sz;
        }
    } else {
        uint32_t p = package + 4;
        for (auto& t : out) {
            if (t.width == 0 || t.size == 0) continue;
            if ((uint64_t)p + t.size > data.size()) break;
            t.data = p;
            p += t.size;
        }
    }
    return out;
}

std::vector<Nu20File::Range> Nu20File::indexedLists() const {
    std::vector<Range> out;
    for (auto& c : chunks) {
        if (c.tag != "DISP" || c.size < 0x10) continue;
        uint32_t pl = c.offset + 8;
        uint32_t n = u32(pl + 4);
        int32_t rel = s32(pl + 8);
        if (n == 0 || n > 1000000 || rel == 0) continue;
        int64_t start = (int64_t)pl + 8 + rel;
        if (start < c.offset || start + (int64_t)n * 16 > c.end()) continue;
        out.push_back({(uint32_t)start, (uint32_t)(start + n * 16), "DISP display commands"});
    }
    return out;
}

bool Nu20File::insideIndexedList(uint32_t pos) const {
    for (auto& r : indexedLists())
        if (pos > r.start && pos < r.end) return true;
    return false;
}

Package Nu20File::parsePackage() const {
    Package pk;
    uint32_t p;
    if (base != 0) {
        //FUCK texture first duude all my homies HATE texfirst
        pk.textureFirst = true;
        pk.sizeWord = 0;
        p = 4;
        if ((uint64_t)p + 2 > base) { pk.error = "texture list runs into the NU20 block"; return pk; }
        uint16_t n = u16(p);
        p += 2;
        for (uint16_t i = 0; i < n; i++) {
            if ((uint64_t)p + 0x18 > base) { pk.error = "texture list runs into the NU20 block"; return pk; }
            int32_t sz = s32(p + 0x14);
            p += 0x18 + (sz > 0 ? (uint32_t)sz : 0);
        }
    } else {
        pk.sizeWord = package;
        p = package + 4;
        for (auto& t : textures())
            if (t.width && t.size) p += t.size;
    }
    auto readBuffers = [&](std::vector<GeomBuffer>& v) {
        if ((uint64_t)p + 2 > data.size()) return false;
        uint16_t n = u16(p);
        p += 2;
        for (uint16_t i = 0; i < n; i++) {
            if ((uint64_t)p + 4 > data.size()) return false;
            uint32_t sz = u32(p);
            if ((uint64_t)p + 4 + sz > data.size()) return false;
            v.push_back({p, sz});
            p += 4 + sz;
        }
        return true;
    };
    pk.texturesEnd = p;
    if (!readBuffers(pk.vertexBuffers) || !readBuffers(pk.indexBuffers)) {
        pk.error = "buffer list runs past end of file";
        return pk;
    }
    pk.end = p;
    if (pk.textureFirst && p > base) {
        pk.error = "buffer list runs into the nu20 block";
        return pk;
    }
    pk.parsed = true;
    return pk;
}

int64_t Nu20File::extraTarget(const ExtraPointer& e) const {
    if (e.width == 2) return base + (((int64_t)e.field - base + u16(e.field) + e.constant) & 0xFFFF);
    return (int64_t)e.field + s32(e.field) + e.constant;
}

void Nu20File::setExtraTarget(const ExtraPointer& e, int64_t target) {
    int64_t v = target - e.field - e.constant;
    if (e.width == 2) {
        if (target < base || target - base > 0xFFFF)
            throw Error(std::string(e.what) + " target " + hex(target) + " would be past the first 64K of NU20");
        uint16_t w = (uint16_t)(v & 0xFFFF);
        std::memcpy(&data[e.field], &w, 2);
    } else {
        put32(e.field, (uint32_t)(int32_t)v);
    }
}

static const char* kNestedTags[] = {"DISP"};
static const char* kContainerTags[] = {"CDAT"};

void Nu20File::parseChunks() {
    chunks.clear();
    uint32_t o = sceneStart();
    while (o < pntr) {
        uint32_t sz = (o + 8 <= pntr) ? u32(o + 4) : 0;
        if (plausibleTag(&data[o]) && sz >= 8 && (uint64_t)o + sz <= pntr) {
            chunks.push_back({tagAt(o), o, sz, 0});
            o += sz;
            continue;
        }
        //idk this part is weird
        uint32_t next = o + 4;
        for (; next < pntr; next += 4) {
            if (!plausibleTag(&data[next])) continue;
            uint32_t s2 = u32(next + 4);
            uint64_t e = (uint64_t)next + s2;
            if (s2 >= 8 && e <= pntr && (e == pntr || plausibleTag(&data[(size_t)e]))) break;
        }
        chunks.push_back({"", o, next - o, 0});
        o = next;
    }
    chunks.push_back({"PNTR", pntr, package - pntr, 0});

    std::vector<Chunk> nested;
    for (auto& c : chunks) {
        if (std::none_of(std::begin(kContainerTags), std::end(kContainerTags), [&](const char* t) { return c.tag == t; }))
            continue;
        for (uint32_t p = c.offset + 8; p + 8 <= c.end(); p += 4) {
            for (const char* t : kNestedTags) {
                if (std::memcmp(&data[p], t, 4) == 0) {
                    uint32_t s2 = u32(p + 4);
                    if (s2 >= 8 && p + s2 <= c.end()) nested.push_back({t, p, s2, 1});
                }
            }
        }
    }
    chunks.insert(chunks.end(), nested.begin(), nested.end());
    std::stable_sort(chunks.begin(), chunks.end(), [](const Chunk& a, const Chunk& b) {
        return a.offset != b.offset ? a.offset < b.offset : a.depth < b.depth;
    });
}

void Nu20File::rebuildPntr() {
    for (uint32_t r : relocs)
        if (r + 4 > pntr) throw Error("relocation field " + hex(r) + " isnt inside the scene");

    uint32_t count = (uint32_t)relocs.size();
    uint32_t entries = pntr + 12;
    uint32_t entriesEnd = entries + count * 4;
    uint32_t pad = (16 - ((entriesEnd - base) & 15)) & 15;
    uint32_t newSize = 12 + count * 4 + pad;

    std::vector<uint8_t> chunk(newSize, 0);
    std::memcpy(&chunk[0], "PNTR", 4);
    std::memcpy(&chunk[4], &newSize, 4);
    std::memcpy(&chunk[8], &count, 4);
    for (uint32_t i = 0; i < count; i++) {
        int32_t v = (int32_t)((int64_t)relocs[i] - (entries + i * 4));
        std::memcpy(&chunk[12 + i * 4], &v, 4);
    }
    data.erase(data.begin() + pntr, data.begin() + package);
    data.insert(data.begin() + pntr, chunk.begin(), chunk.end());
    package = pntr + newSize;

    put32(hdr(0x04), (uint32_t)-(int32_t)(package - base));
    put32(hdr(0x18), (uint32_t)((int64_t)pntr + 8 - hdr(0x18)));
    sceneRoot = (uint32_t)(hdr(0x1C) + (int64_t)s32(hdr(0x1C)));
    parseChunks();
    findExtras();
}

Pointer Nu20File::pointerAt(uint32_t field) const {
    Pointer p{field, s32(field), 0, PtrKind::Zero};
    if (p.value == 0) return p;
    p.target = (int64_t)field + p.value;
    p.kind = (p.target >= 0 && p.target < (int64_t)data.size()) ? PtrKind::InFile : PtrKind::Sentinel;
    return p;
}

std::vector<Pointer> Nu20File::pointers() const {
    std::vector<Pointer> out;
    out.reserve(relocs.size());
    for (uint32_t r : relocs) out.push_back(pointerAt(r));
    return out;
}

const Chunk* Nu20File::chunkContaining(uint32_t pos) const {
    const Chunk* best = nullptr;
    for (auto& c : chunks)
        if (pos >= c.offset && pos < c.end() && (!best || c.depth > best->depth)) best = &c;
    return best;
}

const Chunk* Nu20File::findChunk(const std::string& tag, int nth) const {
    for (auto& c : chunks)
        if (c.tag == tag && nth-- == 0) return &c;
    return nullptr;
}

std::string Nu20File::describe(int64_t pos) const {
    if (pos < 0 || pos >= (int64_t)data.size()) return "outside(" + hex(pos) + ")";
    if (pos < base) return "pre+" + hex(pos);
    if (pos < sceneStart()) return "hdr+" + hex(pos - base);
    if (pos >= package) return "pkg+" + hex(pos - package);
    const Chunk* c = chunkContaining((uint32_t)pos);
    if (!c) return "?" + hex(pos);
    return (c->tag.empty() ? std::string("gap") : c->tag) + "+" + hex(pos - c->offset);
}

}