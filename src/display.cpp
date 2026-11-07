#include "display.h"
#include <algorithm>
#include <functional>
#include "edit.h"
#include "content.h"

// I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE I HATE DISP!!!!!!!!!!!!!!!!!

namespace no20 {

namespace {

constexpr uint32_t kSubSize = 0x44, kBlock24 = 0x24, kNextSize = 0x10, kCmd = 16;

void put16(Nu20File& f, uint32_t pos, uint16_t v) { std::memcpy(&f.data[pos], &v, 2); }

const Chunk* dispChunk(const Nu20File& f) {
    for (auto& c : f.chunks)
        if (c.tag == "DISP") return &c;
    return nullptr;
}

std::vector<uint32_t> modelIndexFields(const Nu20File& f, uint32_t listOffset) {
    std::vector<uint32_t> out;
    const Chunk* c = dispChunk(f);
    if (!c) return out;
    uint32_t pl = c->offset + 8;
    uint32_t n = f.u32(pl + 0x10);
    uint32_t models = f.follow(pl + 0x14);
    if (!n || !models) return out;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t m = models + i * 12;
        uint32_t parts = f.u32(m);
        uint32_t list = f.follow(m + listOffset);
        if (!list) continue;
        for (uint32_t j = 0; j < parts; j++) out.push_back(list + j * 4);
    }
    return out;
}

struct Snapshot {
    std::vector<uint8_t> bytes;
    struct Field { uint32_t off; PtrKind kind; int64_t target; };
    std::vector<Field> relocs;
};

Snapshot snapshot(const Nu20File& f, uint32_t at, uint32_t size) {
    Snapshot s{std::vector<uint8_t>(f.data.begin() + at, f.data.begin() + at + size), {}};
    for (uint32_t r : f.relocs)
        if (r >= at && r < at + size) {
            Pointer p = f.pointerAt(r);
            s.relocs.push_back({r - at, p.kind, p.target});
        }
    std::sort(s.relocs.begin(), s.relocs.end(), [](auto& a, auto& b) { return a.off < b.off; });
    return s;
}

void registerRelocs(Nu20File& f, const std::vector<uint32_t>& fields) {
    auto pos = f.relocs.end();
    if (!f.relocs.empty() && f.relocs.back() == f.hdr(0x1C)) --pos;
    for (uint32_t r : fields)
        if (std::find(f.relocs.begin(), f.relocs.end(), r) == f.relocs.end()) pos = f.relocs.insert(pos, r) + 1;
    f.rebuildPntr();
}

void setPointer(Nu20File& f, uint32_t field, uint32_t target) {
    f.put32(field, (uint32_t)(int32_t)((int64_t)target - field));
}

struct Tracked {
    std::vector<uint32_t*> ptrs;
    void shift(uint32_t at, uint32_t n) {
        for (auto* p : ptrs)
            if (*p >= at) *p += n;
    }
};

uint32_t insertCopy(Nu20File& f, const Snapshot& src, uint32_t at, Tracked& tr, const std::function<bool(uint32_t off, uint32_t& target)>& override = nullptr, InsertOptions opt = {}) {
    uint32_t n = (uint32_t)src.bytes.size();
    insertBytes(f, at, n, src.bytes.data(), opt);
    tr.shift(at, n);
    std::vector<uint32_t> fields;
    for (auto& r : src.relocs) {
        uint32_t field = at + r.off;
        uint32_t target = 0;
        if (override && override(r.off, target)) {
            if (target) setPointer(f, field, target);
            else f.put32(field, 0);
        } else if (r.kind == PtrKind::InFile) {
            int64_t t = r.target;
            if (t > at || (t == at && opt.targetAtPointMoves)) t += n;
            setPointer(f, field, (uint32_t)t);
        }
        fields.push_back(field);
    }
    registerRelocs(f, fields);
    return at;
}

}

DispLayout readDisp(const Nu20File& f, int64_t materialCount, bool strict) {
    const Chunk* c = dispChunk(f);
    if (!c) throw Error("no DISP chunk");
    DispLayout L;
    L.payload = c->offset + 8;
    L.commandCount = f.u32(L.payload + 4);
    L.commands = f.follow(L.payload + 8);
    if (!L.commands) throw Error("DISP has no command list");
    uint32_t g = f.gsnhPayload();
    uint32_t m = materialCount >= 0 ? (uint32_t)materialCount : f.u32(g + 0x10);
    uint32_t arr = f.follow(g + 0x0C);
    for (uint32_t i = 0; i < m; i++) {
        uint32_t rec = f.follow(arr + i * 4);
        L.materials.push_back(rec);
        uint32_t sub = f.follow(rec + 0x3C);
        if (!sub || (strict && f.u32(sub + 4) != i))
            throw Error("material " + std::to_string(i) + " has no render bucket in DISP");
        L.subs.push_back(sub);
    }
    uint32_t ng = f.u32(L.payload + 0x60);
    uint32_t gp = f.follow(L.payload + 0x64);
    for (uint32_t i = 0; gp && i < ng; i++) {
        uint32_t r = gp + i * 0x24;
        L.groups.push_back({r, f.follow(r + 4), f.u16(r + 0x20), f.u16(r + 0x22)});
    }
    return L;
}

std::vector<uint32_t> modelCommandIndexFields(const Nu20File& f) { return modelIndexFields(f, 8); }
std::vector<uint32_t> modelMaterialIndexFields(const Nu20File& f) { return modelIndexFields(f, 4); }

bool readTexAnims(const Nu20File& f, TexAnimLayout& t) {
    const Chunk* c = f.findChunk("TAS0");
    if (!c) return false;
    auto bad = [](const std::string& s) { return Error("TAS0: " + s); };
    if (c->size < 0x14) throw bad("chunk is too small");
    t = {};
    t.payload = c->offset + 8;
    t.count = f.u32(t.payload);
    t.records = t.payload + 8;
    t.chunkEnd = c->end();
    if ((uint64_t)t.count * 0x20 + 0x0C > c->size - 8) throw bad("records run past the chunk");
    t.totalField = t.record(t.count);
    t.table = t.totalField + 4;
    t.total = f.u32(t.totalField);
    if ((uint64_t)t.table + 2ull * t.total > t.chunkEnd) throw bad("frame table runs past the chunk");
    uint32_t g = f.gsnhPayload();
    if (f.u32(g + 0x60) != t.count) throw bad("GSNH anim count doesnt match");
    if (t.count && (f.follow(g + 0x64) != t.records || f.follow(g + 0x68) != t.table))
        throw bad("GSNH doesnt point at the records + frame table");
    return true;
}

std::vector<uint32_t> animatedMaterialFields(const Nu20File& f) {
    std::vector<uint32_t> out;
    TexAnimLayout t;
    try {
        if (!readTexAnims(f, t)) return out;
    } catch (const Error&) {
        return out;
    }
    uint32_t materials = f.u32(f.gsnhPayload() + 0x10);
    for (uint32_t i = 0; i < t.count; i++) {
        uint32_t field = t.record(i) + 0x10;
        if (f.u32(field) >= materials) return {};
        out.push_back(field);
    }
    return out;
}

static Snapshot blankBlock(uint32_t size, std::initializer_list<uint32_t> relocs) {
    Snapshot s{std::vector<uint8_t>(size, 0), {}};
    for (uint32_t off : relocs) s.relocs.push_back({off, PtrKind::Zero, 0});
    return s;
}

static void put32At(Snapshot& s, uint32_t off, uint32_t v) { std::memcpy(&s.bytes[off], &v, 4); }

// the two 0x87s are there only when the material has a diffuse texture
static Snapshot materialSegment(const std::vector<uint8_t>& record, uint32_t& mtlOff) {
    int32_t texture;
    std::memcpy(&texture, &record[0xB8], 4);
    std::vector<uint32_t> words = {0x185};
    if (texture != -1) words.insert(words.end(), {0x87, 0x87});
    mtlOff = (uint32_t)words.size() * kCmd + 4;
    words.insert(words.end(), {0x380, 0x18B});
    Snapshot s = blankBlock((uint32_t)words.size() * kCmd, {});
    for (uint32_t i = 0; i < words.size(); i++) {
        put32At(s, i * kCmd, words[i]);
        s.relocs.push_back({i * kCmd + 4, PtrKind::Zero, 0});
    }
    return s;
}

static Snapshot runtimeBlock(const Nu20File& f) {
    if (!f.base) return blankBlock(kBlock24, {0});
    if (f.version >= 3) return blankBlock(0x28, {0, 4});
    return blankBlock(0x34, {8, 0x10});
}

static void appendAnimation(Nu20File& f, uint32_t material, uint32_t name18, uint32_t name1C, uint8_t byteE, uint8_t byteF, const std::vector<uint8_t>& frames) {
    TexAnimLayout t;
    readTexAnims(f, t);
    uint32_t tail = t.table + ((t.total * 2) & ~3u);
    uint32_t chunkStart = t.payload - 8;
    uint32_t align = (t.chunkEnd - chunkStart) % 16 == 0 ? 16 : 4;
    std::vector<uint8_t> bytes(f.data.begin() + tail, f.data.begin() + t.table + t.total * 2);
    bytes.insert(bytes.end(), frames.begin(), frames.end());
    while ((tail - chunkStart + bytes.size()) % align) bytes.push_back(0);
    int64_t delta = (int64_t)bytes.size() - (t.chunkEnd - tail);
    if (delta > 0) insertBytes(f, t.chunkEnd, (uint32_t)delta);
    else if (delta < 0) removeBytes(f, (uint32_t)(t.chunkEnd + delta), (uint32_t)-delta);
    std::copy(bytes.begin(), bytes.end(), f.data.begin() + tail);
    uint32_t count = (uint32_t)frames.size() / 2;
    f.put32(t.totalField, t.total + count);

    Snapshot rec = blankBlock(0x20, {0, 4, 0x14});
    rec.relocs.push_back({0x18, name18 ? PtrKind::InFile : PtrKind::Zero, name18});
    rec.relocs.push_back({0x1C, name1C ? PtrKind::InFile : PtrKind::Zero, name1C});
    put32At(rec, 8, t.total);
    uint16_t c16 = (uint16_t)count;
    std::memcpy(&rec.bytes[0xC], &c16, 2);
    rec.bytes[0xE] = byteE;
    rec.bytes[0xF] = byteF;
    put32At(rec, 0x10, material);
    Tracked tr;
    insertCopy(f, rec, t.totalField, tr);
    f.put32(t.payload, t.count + 1);
    f.put32(f.gsnhPayload() + 0x60, t.count + 1);
}

uint32_t addTextureAnimations(Nu20File& f, uint32_t from, uint32_t to, std::string* report) {
    TexAnimLayout t;
    if (animatedMaterialFields(f).empty() || !readTexAnims(f, t)) return 0;
    std::vector<uint32_t> sources;
    for (uint32_t i = 0; i < t.count; i++)
        if (f.u32(t.record(i) + 0x10) == from) sources.push_back(i);
    std::string log;
    for (uint32_t src : sources) {
        readTexAnims(f, t);
        uint32_t rec = t.record(src);
        uint32_t first = f.u32(rec + 8), frames = f.u16(rec + 0x0C);
        if ((uint64_t)first + frames > t.total) throw Error("TAS0 anim frames are outside the frame table");
        std::vector<uint8_t> copy(f.data.begin() + t.table + first * 2, f.data.begin() + t.table + (first + frames) * 2);
        appendAnimation(f, to, f.follow(rec + 0x18), f.follow(rec + 0x1C), f.data[rec + 0xE], f.data[rec + 0xF], copy);
        log += "  animation     TAS0 record " + std::to_string(t.count) + " (copy of " + std::to_string(src) + "), " + std::to_string(frames) + " frames at table[" + std::to_string(t.total) + "]\n";
    }
    if (report) *report = log;
    return (uint32_t)sources.size();
}

std::vector<AnimInfo> textureAnimations(const Nu20File& f) {
    std::vector<AnimInfo> out;
    TexAnimLayout t;
    if (!readTexAnims(f, t)) return out;
    for (uint32_t i = 0; i < t.count; i++) {
        uint32_t rec = t.record(i), first = f.u32(rec + 8), frames = f.u16(rec + 0x0C);
        AnimInfo a;
        a.material = f.u32(rec + 0x10);
        if (uint32_t name = f.follow(rec + 0x1C)) a.script = (const char*)&f.data[name];
        for (uint32_t k = 0; k < frames && first + k < t.total; k++) a.frames.push_back(f.u16(t.table + (first + k) * 2));
        out.push_back(a);
    }
    return out;
}

static uint32_t ntblName(Nu20File& f, const std::string& name) {
    const Chunk* c = f.findChunk("NTBL");
    if (!c) throw Error("no NTBL chunk to put the script name in");
    uint32_t lenField = c->offset + 8, start = c->offset + 12, len = f.u32(lenField);
    for (uint32_t p = start; p < start + len;) {
        std::string s((const char*)&f.data[p]);
        if (s == name) return p;
        p += (uint32_t)s.size() + 1;
    }
    uint32_t end = start + len, need = (uint32_t)name.size() + 1, room = c->end() - end;
    if (need > room) insertBytes(f, std::min((end + 3) & ~3u, c->end()), (need - room + 3) & ~3u);
    std::memcpy(&f.data[end], name.c_str(), need);
    f.put32(lenField, len + need);
    return end;
}

static void createAnimationChunk(Nu20File& f, uint32_t material, uint32_t name, const std::vector<uint8_t>& frames) {
    uint32_t at = 0;
    if (const Chunk* cdat = f.findChunk("CDAT")) {
        at = cdat->offset;
    } else {
        for (auto& c : f.chunks)
            if (c.depth == 0 && (c.tag == "DISP" || c.tag == "IABL")) at = std::max(at, c.end());
    }
    if (!at) throw Error("no place to put a TAS0 chunk in this file");
    uint32_t size = (0x34 + (uint32_t)frames.size() + 15) & ~15u;
    std::vector<uint8_t> blob(size, 0);
    std::memcpy(&blob[0], "TAS0", 4);
    uint32_t count = 1, total = (uint32_t)frames.size() / 2;
    uint16_t c16 = (uint16_t)total;
    std::memcpy(&blob[4], &size, 4);
    std::memcpy(&blob[8], &count, 4);
    std::memcpy(&blob[0x10 + 0xC], &c16, 2);
    std::memcpy(&blob[0x10 + 0x10], &material, 4);
    std::memcpy(&blob[0x30], &total, 4);
    std::memcpy(&blob[0x34], frames.data(), frames.size());
    InsertOptions opt;
    opt.growChunks = false;
    insertBytes(f, at, size, blob.data(), opt);
    f.parse();
    uint32_t rec = at + 0x10;
    for (uint32_t off : {0u, 4u, 0x14u}) addReloc(f, rec + off);
    writePointer(f, rec + 0x18, name);
    writePointer(f, rec + 0x1C, name);
    uint32_t g = f.gsnhPayload();
    f.put32(g + 0x60, 1);
    writePointer(f, g + 0x64, rec);
    writePointer(f, g + 0x68, rec + 0x24);
}

uint32_t addAnimation(Nu20File& f, uint32_t material, const std::string& script, const std::vector<uint16_t>& frames, std::string* report) {
    auto mats = materials(f);
    if (material >= mats.size()) throw Error("no material " + std::to_string(material));
    if (mats[material].texture32 == -1) throw Error("material " + std::to_string(material) + " has no texture, frames replace its texture so give it one first");
    if (frames.empty() || frames.size() > 0xFFFF) throw Error("an animation needs 1 to 65k frames");
    auto tex = f.textures();
    for (uint16_t t : frames) {
        if (t >= tex.size()) throw Error("no texture " + std::to_string(t));
        if (!tex[t].width) throw Error("texture " + std::to_string(t) + " is an empty cubemap slot");
    }
    for (auto& a : textureAnimations(f))
        if (a.material == material) throw Error("material " + std::to_string(material) + " already has an animation");
    std::vector<uint8_t> bytes(frames.size() * 2);
    std::memcpy(bytes.data(), frames.data(), bytes.size());

    uint32_t name = ntblName(f, script);
    TexAnimLayout t;
    uint32_t idx = 0;
    if (readTexAnims(f, t)) {
        idx = t.count;
        appendAnimation(f, material, name, name, 0, 0, bytes);
    } else {
        createAnimationChunk(f, material, name, bytes);
    }
    if (report) *report = "  animation     TAS0 record " + std::to_string(idx) + ", material " + std::to_string(material) + ", script " + script + ", " + std::to_string(frames.size()) + " frames" + (idx ? "" : " (new TAS0 chunk if the file had none)") + "\n";
    return idx;
}

static void growMaterialBits(Nu20File& f, uint32_t oldCount) {
    auto bytesFor = [](uint32_t n) { return (n + 8 + 127) / 128 * 16; };
    uint32_t oldSize = bytesFor(oldCount), grow = bytesFor(oldCount + 1) - oldSize;
    if (!grow) return;
    uint32_t pl = dispChunk(f)->offset + 8;
    uint32_t a = f.follow(pl + 0x58), b = f.follow(pl + 0x5C), c = f.follow(pl + 0x48);
    if (b != a + oldSize || c != b + oldSize) throw Error("material bit arrays arent where they should be, cant add this material");
    insertBytes(f, b, grow);
    pl = dispChunk(f)->offset + 8;
    insertBytes(f, f.follow(pl + 0x48), grow);
}

uint32_t createMaterial(Nu20File& f, uint32_t groupOf, std::string* report, bool inItsGroup, const std::vector<uint8_t>& record) {
    DispLayout L = readDisp(f);
    uint32_t M = (uint32_t)L.materials.size();
    if (groupOf >= M) throw Error("no material " + std::to_string(groupOf));
    if (f.u32(L.payload + 0x4C) != M) throw Error("DISP mat list doesnt match the material count");
    for (uint32_t i = 1; i < M; i++) {
        if (L.subs[i] != L.subs[i - 1] + kSubSize) throw Error("render buckets arent one array in material order");
        if (L.materials[i] != L.materials[1] - L.materials[0] + L.materials[i - 1])
            throw Error("material records arent one array in material order");
    }
    uint32_t recSize = M >= 2 ? L.materials[1] - L.materials[0] : 0x2C4;
    if (record.size() != recSize) throw Error("built material is a different size than this files materials");
    auto group = std::find_if(L.groups.begin(), L.groups.end(), [&](auto& g) {
        return inItsGroup ? groupOf >= g.first && groupOf < (uint32_t)g.first + g.count : (uint32_t)g.first + g.count == M;
    });
    if (group == L.groups.end()) throw Error("no render group to put the new material in");
    uint32_t groupFirst = group->first;
    uint32_t k = (uint32_t)group->first + group->count;

    bool hasBuffers = f.follow(L.subs[M - 1] + 0x28) != 0;
    bool ruled = hasBuffers && capacityRuleHolds(f);
    std::string log = "  index         " + std::to_string(k) + " (end of render group " + std::to_string(group - L.groups.begin()) + ")\n";

    int renumbered = 0;
    auto bump = [&](uint32_t field) {
        if (f.u32(field) >= k) { f.put32(field, f.u32(field) + 1); renumbered++; }
    };
    for (uint32_t field : modelMaterialIndexFields(f)) bump(field);
    for (uint32_t field : animatedMaterialFields(f)) bump(field);
    for (auto& g : L.groups)
        if (g.first >= k && g.record != group->record) put16(f, g.record + 0x22, (uint16_t)(g.first + 1));
    if (renumbered) log += "  renumbered    " + std::to_string(renumbered) + " material references\n";

    auto slotFor = [&](const DispLayout& l, uint32_t subOff, uint32_t stride) -> uint32_t {
        if (k < M) return f.follow(l.subs[k] + subOff);
        uint32_t last = f.follow(l.subs[M - 1] + subOff);
        return last ? last + stride : 0;
    };

    Tracked tr;
    uint32_t rec = k < M ? L.materials[k] : L.materials[M - 1] + recSize;
    Snapshot recSrc = blankBlock(recSize, {0x28, 0x2C, 0x30, 0x3C});
    recSrc.bytes = record;
    insertCopy(f, recSrc, rec, tr);
    tr.ptrs.push_back(&rec);
    log += "  record        " + f.describe(rec) + " (built)\n";

    L = readDisp(f, M);
    uint32_t term = f.follow(f.follow(L.subs[k - 1] + 0x0C) + 4);
    if (f.data[term] != 0x84) throw Error("groups last segment doesnt end at a TERM");
    uint32_t termIdx = L.commandIndex(term);
    uint32_t mtlOff = 0;
    Snapshot seg = materialSegment(record, mtlOff);
    uint32_t segLen = (uint32_t)seg.bytes.size(), dynOff = segLen - kCmd;
    InsertOptions segOpt;
    segOpt.targetAtPointMoves = false;
    segOpt.allowInsideIndexedList = true;
    uint32_t newSeg = term;
    insertCopy(f, seg, term, tr, [&](uint32_t off, uint32_t& target) {
        if (off == 4) { target = term + segLen; return true; }
        if (off == mtlOff) { target = rec; return true; }
        return false;
    }, segOpt);
    tr.ptrs.push_back(&newSeg);
    for (uint32_t field : modelCommandIndexFields(f))
        if (f.u32(field) >= termIdx) f.put32(field, f.u32(field) + segLen / kCmd);
    f.put32(L.payload + 4, f.u32(L.payload + 4) + segLen / kCmd);
    log += "  segment       " + std::to_string(segLen / kCmd) + " commands at #" + std::to_string(termIdx) + "\n";

    L = readDisp(f, M);
    Snapshot sub = blankBlock(kSubSize, {0, 8, 0x0C, 0x10, 0x14, 0x18, 0x24, 0x28, 0x2C, 0x30});
    put32At(sub, 0x20, 1);
    uint32_t newSub = k < M ? L.subs[k] : L.subs[M - 1] + kSubSize;
    uint32_t dispAt = L.payload;
    insertCopy(f, sub, newSub, tr, [&](uint32_t off, uint32_t& target) {
        if (off == 0) { target = dispAt; return true; }
        if (off == 0x0C) { target = newSeg; return true; }
        if (off == 0x18) { target = newSeg + dynOff; return true; }
        return false;
    });
    tr.ptrs.push_back(&newSub);
    setPointer(f, rec + 0x3C, newSub);
    log += "  render bucket " + f.describe(newSub) + "\n";

    auto addChild = [&](uint32_t subOff, const Snapshot& s, std::initializer_list<uint32_t> fields, const char* what) {
        DispLayout l = readDisp(f, M);
        uint32_t at = slotFor(l, subOff, (uint32_t)s.bytes.size());
        if (!at) return;
        insertCopy(f, s, at, tr);
        std::vector<uint32_t> regs;
        for (uint32_t o : fields) {
            setPointer(f, newSub + o, at);
            regs.push_back(newSub + o);
        }
        registerRelocs(f, regs);
        log += std::string("  ") + what + f.describe(at) + "\n";
    };
    Snapshot next = blankBlock(kNextSize, {4});
    put32At(next, 0, 0x18D);
    addChild(0x08, runtimeBlock(f), {0x08}, "0x24 block    ");
    addChild(0x10, next, {0x10, 0x14}, "NEXT command  ");

    {
        DispLayout l = readDisp(f, M);
        uint32_t cap = ruled ? 1 : f.u32(l.subs[groupOf] + 0x38);
        f.put32(newSub + 0x38, cap);
        uint32_t lastBuf = f.follow(l.subs[M - 1] + 0x2C);
        if (hasBuffers && lastBuf) {
            uint32_t at = k < M ? f.follow(l.subs[k] + 0x28) : lastBuf + f.u32(l.subs[M - 1] + 0x38) * 0x10;
            insertBytes(f, at, 2 * cap * 0x10);
            tr.shift(at, 2 * cap * 0x10);
            setPointer(f, newSub + 0x28, at);
            setPointer(f, newSub + 0x2C, at + cap * 0x10);
            registerRelocs(f, {newSub + 0x28, newSub + 0x2C});
            log += "  buffers       2 x " + hex(cap * 0x10) + " at " + f.describe(at) + " (capacity " + std::to_string(cap) + ")\n";
        }
    }

    InsertOptions slotOpt;
    slotOpt.targetAtPointMoves = k > 0;
    uint32_t g = f.gsnhPayload();
    uint32_t arr = f.follow(g + 0x0C);
    insertBytes(f, arr + k * 4, 4, nullptr, slotOpt);
    tr.shift(arr + k * 4, 4);
    writePointer(f, arr + k * 4, rec);
    f.put32(f.gsnhPayload() + 0x10, M + 1);
    if (const Chunk* ms = f.findChunk("MS00"); ms && f.u32(ms->offset + 8) == M) f.put32(ms->offset + 8, M + 1);

    uint32_t pl = readDisp(f, 0).payload;
    uint32_t list = f.follow(pl + 0x54);
    insertBytes(f, list + k * 4, 4, nullptr, slotOpt);
    tr.shift(list + k * 4, 4);
    writePointer(f, list + k * 4, newSub);
    insertBytes(f, list + (M + 1) * 4, 12); //pad 4 alignment
    tr.shift(list + (M + 1) * 4, 12);
    f.put32(pl + 0x4C, M + 1);

    DispLayout fin = readDisp(f, M + 1, false);
    for (uint32_t i = 0; i <= M; i++) {
        f.put32(fin.materials[i] + 0x38, i);
        f.put32(fin.subs[i] + 4, i);
    }
    fin = readDisp(f);
    auto grp = std::find_if(fin.groups.begin(), fin.groups.end(), [&](auto& gr) { return gr.first == groupFirst; });
    if (grp == fin.groups.end()) throw Error("bug: lost the render group");
    put16(f, grp->record + 0x20, (uint16_t)(grp->count + 1));
    growMaterialBits(f, M);

    if (report) *report = log;
    return k;
}

uint32_t materialUses(const Nu20File& f, uint32_t material) {
    uint32_t n = 0;
    for (uint32_t field : modelMaterialIndexFields(f))
        if (f.u32(field) == material) n++;
    return n;
}

bool capacityRuleHolds(const Nu20File& f) {
    DispLayout L = readDisp(f);
    std::vector<uint32_t> uses(L.materials.size(), 0);
    for (uint32_t field : modelMaterialIndexFields(f))
        if (f.u32(field) < uses.size()) uses[f.u32(field)]++;
    bool any = false;
    for (size_t i = 0; i < L.subs.size(); i++) {
        if (!f.follow(L.subs[i] + 0x28)) continue;
        any = true;
        if (f.u32(L.subs[i] + 0x38) != 2 * uses[i] + 1) return false;
    }
    return any;
}

void setMaterialCapacity(Nu20File& f, uint32_t material, uint32_t capacity) {
    DispLayout L = readDisp(f);
    if (material >= L.subs.size()) throw Error("no material " + std::to_string(material));
    uint32_t sub = L.subs[material];
    uint32_t a = f.follow(sub + 0x28), b = f.follow(sub + 0x2C), old = f.u32(sub + 0x38);
    if (!a || !b) throw Error("material " + std::to_string(material) + " has no instance buffers");
    if (b != a + old * 0x10) throw Error("material " + std::to_string(material) + " instance buffers arent next to each other");
    if (capacity == old) return;
    if (capacity > old) {
        uint32_t n = (capacity - old) * 0x10;
        insertBytes(f, b + old * 0x10, n);
        insertBytes(f, a + old * 0x10, n);
    } else {
        uint32_t n = (old - capacity) * 0x10;
        removeBytes(f, b + capacity * 0x10, n);
        removeBytes(f, a + capacity * 0x10, n);
    }
    f.put32(readDisp(f).subs[material] + 0x38, capacity);
}

std::vector<std::string> checkMaterialRefs(const Nu20File& before, const Nu20File& after) {
    std::vector<std::string> out;
    auto settingsEqual = [&](uint32_t ia, uint32_t ib) {
        DispLayout la = readDisp(before, -1, false), lb = readDisp(after, -1, false);
        if (ia >= la.materials.size() || ib >= lb.materials.size()) return false;
        uint32_t ra = la.materials[ia], rb = lb.materials[ib];
        uint32_t size = la.materials.size() > 1 ? la.materials[1] - la.materials[0] : 0x2C4;
        for (uint32_t o = 0; o < size; o += 4)
            if (o != 0x38 && o != 0x3C && before.u32(ra + o) != after.u32(rb + o)) return false;
        return true;
    };
    auto compare = [&](const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, const char* what, bool appendable = false) {
        if (appendable ? b.size() < a.size() : a.size() != b.size()) {
            out.push_back(std::string(what) + ": reference count changed");
            return;
        }
        for (size_t i = 0; i < a.size() && out.size() < 10; i++)
            if (!settingsEqual(before.u32(a[i]), after.u32(b[i]))) out.push_back(std::string(what) + " " + std::to_string(i) + " now reaches a different material");
    };
    compare(modelMaterialIndexFields(before), modelMaterialIndexFields(after), "game-model material reference");
    compare(animatedMaterialFields(before), animatedMaterialFields(after), "texture animation", true);
    return out;
}

std::vector<std::string> checkModelCommandRefs(const Nu20File& before, const Nu20File& after) {
    std::vector<std::string> out;
    auto a = modelCommandIndexFields(before), b = modelCommandIndexFields(after);
    if (a.size() != b.size()) return {"number of game-model command references changed"};
    DispLayout la = readDisp(before, 0), lb = readDisp(after, 0);
    for (size_t i = 0; i < a.size() && out.size() < 10; i++) {
        uint32_t ca = la.commandAt(before.u32(a[i])), cb = lb.commandAt(after.u32(b[i]));
        uint32_t ta = before.follow(ca + 4), tb = after.follow(cb + 4);
        bool same = before.u16(ca) == after.u16(cb) && (ta == 0) == (tb == 0) && (!ta || std::equal(before.data.begin() + ta, before.data.begin() + ta + 16, after.data.begin() + tb));
        if (!same) out.push_back("model cmd reference " + std::to_string(i) + " points at a different cmd now");
    }
    return out;
}

}