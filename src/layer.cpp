#include "layer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "display.h"
#include "edit.h"

namespace no20 {

static const uint32_t OBJECT_SIZE = 0xD0;
static const size_t MAX_LAYERS = 31;
static const uint8_t GEOMCALL = 0x82, MTXLOAD = 0x83;

static uint32_t dispPayload(const Nu20File& f) {
    const Chunk* c = f.findChunk("DISP");
    if (!c) throw Error("no DISP chunk");
    return c->offset + 8;
}

static uint32_t dispPtr(const Nu20File& f, uint32_t off) { return f.follow(dispPayload(f) + off); }

static void put16(Nu20File& f, uint32_t pos, uint16_t v) { std::memcpy(&f.data[pos], &v, 2); }

static uint32_t bitBytes(uint32_t n) { return (n + 127) / 128 * 16; }

// +8 and +0x10 hold a node, +4 and +0xC an array of nodes per bone. node +8 -> object, object +0xB0 -> model
static int modelOfNode(const Nu20File& f, uint32_t node) {
    uint32_t object = f.follow(node + 8);
    uint32_t model = object ? f.follow(object + 0xB0) : 0;
    uint32_t models = dispPtr(f, 0x14), count = f.u32(dispPayload(f) + 0x10);
    if (model < models || model >= models + count * 12 || (model - models) % 12) return -1;
    return (int)((model - models) / 12);
}

std::vector<LayerInfo> characterLayers(const Nu20File& f) {
    std::vector<LayerInfo> out;
    if (!f.isCharacterModel()) return out;
    uint32_t g = f.gsnh();
    int32_t count = f.s32(g + 0x18C), bones = f.s32(g + 0x164);
    uint32_t layers = f.follow(g + 0x190);
    if (count <= 0 || count > 4096 || !layers) return out;
    for (int32_t i = 0; i < count; i++) {
        uint32_t L = layers + i * 0x14;
        LayerInfo info;
        if (uint32_t name = f.follow(L)) info.name = (const char*)&f.data[name];
        for (uint32_t off : {8u, 0x10u}) {
            uint32_t node = f.follow(L + off);
            if (!node) continue;
            int m = modelOfNode(f, node);
            if (m >= 0) info.skinned.push_back((uint32_t)m);
        }
        for (uint32_t off : {4u, 0xCu}) {
            uint32_t arr = f.follow(L + off);
            for (int32_t b = 0; arr && b < bones; b++) {
                uint32_t node = f.follow(arr + b * 4);
                if (!node) continue;
                int m = modelOfNode(f, node);
                if (m >= 0) info.rigid.push_back({(uint32_t)m, b});
            }
        }
        out.push_back(info);
    }
    return out;
}

static uint32_t newGameModel(Nu20File& f, uint32_t src) {
    uint32_t n = f.u32(dispPayload(f) + 0x10);
    if (src >= n) throw Error("no game model " + std::to_string(src));
    uint32_t srcRec = dispPtr(f, 0x14) + src * 12, parts = f.u32(srcRec);
    if (!parts) throw Error("game model " + std::to_string(src) + " has no parts to add to");
    uint32_t bnds = dispPtr(f, 0x44);
    uint32_t nb = f.u32(dispPayload(f) + 0x84);
    if (!bnds || f.u32(bnds - 4) != n || nb != n) throw Error("this files bounds dont cover all its game models");
    if (f.u32(dispPayload(f) + 0x6C) != n) throw Error("this file doesnt have one object per game model");
    std::vector<uint8_t> cmds(parts * 4), mats(parts * 4);
    std::memcpy(cmds.data(), &f.data[f.follow(srcRec + 8)], parts * 4);
    std::memcpy(mats.data(), &f.data[f.follow(srcRec + 4)], parts * 4);
    uint32_t totalParts = (dispPtr(f, 0x24) - dispPtr(f, 0x20)) / 4;

    insertBytes(f, dispPtr(f, 0x20) + totalParts * 4, parts * 4, cmds.data());
    insertBytes(f, dispPtr(f, 0x24) + totalParts * 4, parts * 4, mats.data());

    for (uint32_t off : {0x28u, 0x2Cu}) {
        uint32_t end = dispPtr(f, off + 4);
        if (end <= dispPtr(f, off)) throw Error("per model array at DISP+" + hex(off) + " isnt followed by the next one");
        insertBytes(f, end, 4);
        uint32_t countAt = dispPayload(f) + (off == 0x28 ? 0x38 : 0x3C);
        if (f.u32(countAt)) f.put32(countAt, f.u32(countAt) + 1);
    }

    if (n % 2 == 0) insertBytes(f, dispPtr(f, 0x18) + n * 2, 4);
    put16(f, dispPtr(f, 0x18) + n * 2, (uint16_t)parts);
    if (n % 2 == 0) insertBytes(f, dispPtr(f, 0x1C) + n * 2, 4);
    put16(f, dispPtr(f, 0x1C) + n * 2, (uint16_t)totalParts);

    uint32_t grow = bitBytes(2 * (n + 9)) - bitBytes(2 * (n + 8));
    if (grow) {
        insertBytes(f, dispPtr(f, 0x34), grow);
        insertBytes(f, dispPtr(f, 0x58), grow);
    }

    uint32_t byteAt = dispPtr(f, 0x48) + n;
    uint8_t srcByte = f.data[dispPtr(f, 0x48) + src];
    if ((byteAt - f.base) % 16 == 0 || f.data[byteAt]) insertBytes(f, byteAt, 16);
    f.data[dispPtr(f, 0x48) + n] = srcByte;

    uint32_t mask = dispPtr(f, 0x88);
    bool srcBit = f.u32(mask + src / 32 * 4) >> (src % 32) & 1;
    if ((n + 1) / 32 != n / 32) insertBytes(f, mask + (n / 32 + 1) * 4, 16);
    mask = dispPtr(f, 0x88);
    uint32_t word = f.u32(mask + n / 32 * 4);
    f.put32(mask + n / 32 * 4, srcBit ? word | 1u << (n % 32) : word & ~(1u << (n % 32)));

    const float unplaced[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    insertBytes(f, dispPtr(f, 0x44) + n * 32, 32, (const uint8_t*)unplaced);
    f.put32(dispPtr(f, 0x44) - 4, n + 1);

    uint32_t at = dispPtr(f, 0x14) + n * 12;
    insertBytes(f, at, 12);
    f.put32(at, parts);
    writePointer(f, at + 4, dispPtr(f, 0x24) + totalParts * 4);
    writePointer(f, at + 8, dispPtr(f, 0x20) + totalParts * 4);

    uint32_t pl = dispPayload(f);
    f.put32(pl + 0x10, n + 1);
    f.put32(pl + 0x84, n + 1);
    uint32_t root = f.gsnhPayload();
    for (uint32_t off : {0xF0u, 0xF4u})
        if (f.u32(root + off) == n) f.put32(root + off, n + 1);
    syncFirstParts(f);
    return n;
}

static void modelBox(const Nu20File& f, uint32_t model, float lo[3], float hi[3]) {
    DispLayout d = readDisp(f, 0);
    Package pk = f.parsePackage();
    if (!pk.parsed) throw Error("couldnt read the package: " + pk.error);
    uint32_t rec = dispPtr(f, 0x14) + model * 12, parts = f.u32(rec), list = f.follow(rec + 8);
    for (int c = 0; c < 3; c++) {
        lo[c] = 1e30f;
        hi[c] = -1e30f;
    }
    for (uint32_t j = 0; j < parts; j++) {
        uint32_t idx = f.u32(list + j * 4), cmd = d.commandAt(idx);
        if (f.data[cmd] != GEOMCALL) continue;
        float M[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (idx > 0 && f.data[d.commandAt(idx - 1)] == MTXLOAD)
            if (uint32_t matrix = f.follow(d.commandAt(idx - 1) + 4)) std::memcpy(M, &f.data[matrix], 64);
        uint32_t mesh = f.follow(cmd + 4);
        uint32_t stride = f.u16(mesh + 8), base = f.u32(mesh + 0x14), count = f.u32(mesh + 0x18), vb = f.u32(mesh + 0x24);
        if (f.u32(mesh + 0x28) || vb >= pk.vertexBuffers.size() || !stride) continue;
        const GeomBuffer& b = pk.vertexBuffers[vb];
        for (uint32_t v = 0; v < count; v++) {
            uint32_t at = b.sizeField + 4 + (base + v) * stride;
            if (at + 12 > b.sizeField + 4 + b.size) break;
            float p[3];
            std::memcpy(p, &f.data[at], 12);
            for (int c = 0; c < 3; c++) {
                float q = p[0] * M[c] + p[1] * M[4 + c] + p[2] * M[8 + c] + M[12 + c];
                lo[c] = std::min(lo[c], q);
                hi[c] = std::max(hi[c], q);
            }
        }
    }
    if (lo[0] > hi[0])
        for (int c = 0; c < 3; c++) lo[c] = hi[c] = 0;
}

static uint32_t addObject(Nu20File& f, uint32_t model, uint32_t like) {
    uint32_t count = f.u32(dispPayload(f) + 0x6C), arr = dispPtr(f, 0x70);
    uint32_t t = arr + like * OBJECT_SIZE;
    std::vector<uint8_t> rec(f.data.begin() + t, f.data.begin() + t + OBJECT_SIZE);
    uint32_t name = f.follow(t + 0xB4);

    float lo[3], hi[3];
    modelBox(f, model, lo, hi);
    float half[3] = {(hi[0] - lo[0]) / 2, (hi[1] - lo[1]) / 2, (hi[2] - lo[2]) / 2};
    float mid[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    float radius = std::sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]);
    float box[12] = {lo[0], lo[1], lo[2], 1, hi[0], hi[1], hi[2], 1, mid[0], mid[1], mid[2], radius};
    std::memcpy(&rec[0x80], box, sizeof box);
    std::memcpy(&rec[0xC0], &model, 4);
    std::fill(rec.begin() + 0xC4, rec.end(), (uint8_t)0);

    float world[8] = {mid[0], mid[1], mid[2], radius, half[0], half[1], half[2], 1};
    std::memcpy(&f.data[dispPtr(f, 0x44) + model * 32], world, sizeof world);

    uint32_t slot = f.follow(arr + (count - 1) * OBJECT_SIZE + 0xBC) + 4;
    if (slot <= arr) throw Error("object runtime slots arent after the objects");
    insertBytes(f, slot, 4);
    uint32_t at = dispPtr(f, 0x70) + count * OBJECT_SIZE;
    insertBytes(f, at, OBJECT_SIZE, rec.data());
    slot += OBJECT_SIZE;
    writePointer(f, at + 0xB0, dispPtr(f, 0x14) + model * 12);
    writePointer(f, at + 0xB4, name);
    writePointer(f, at + 0xBC, slot);
    addReloc(f, at + 0xC4);
    addReloc(f, at + 0xCC);
    f.put32(dispPayload(f) + 0x6C, count + 1);
    return at;
}

static uint32_t lastNodeEnd(const Nu20File& f) {
    uint32_t g = f.gsnh(), layers = f.follow(g + 0x190), last = 0;
    int32_t count = f.s32(g + 0x18C), bones = f.s32(g + 0x164);
    for (int32_t i = 0; i < count; i++) {
        uint32_t L = layers + i * 0x14;
        for (uint32_t off : {8u, 0x10u}) last = std::max(last, f.follow(L + off));
        for (uint32_t off : {4u, 0xCu}) {
            uint32_t arr = f.follow(L + off);
            for (int32_t b = 0; arr && b < bones; b++) last = std::max(last, f.follow(arr + b * 4));
        }
    }
    if (last) return last + 12;
    const Chunk* disp = f.findChunk("DISP");
    return disp->end();
}

uint32_t addLayer(Nu20File& f, const std::string& name, const Model& m, const ImportOptions& opt, std::string* report) {
    if (!f.isCharacterModel()) throw Error("add-layer is only for characters (.GHG)");
    if (name.empty()) throw Error("layer needs a name");
    auto existing = characterLayers(f);
    if (existing.size() >= MAX_LAYERS) throw Error("this char already has " + std::to_string(existing.size()) + " layers, the game only takes " + std::to_string(MAX_LAYERS));
    for (auto& l : existing)
        if (_stricmp(l.name.c_str(), name.c_str()) == 0) throw Error("theres already a layer called " + l.name);
    auto skinned = skinnedModels(f);
    if (skinned.empty()) throw Error("no skinned game model in this file to base the layer on");
    uint32_t src = skinned[0];
    for (uint32_t s : skinned)
        if (f.u32(dispPtr(f, 0x14) + s * 12) < f.u32(dispPtr(f, 0x14) + src * 12)) src = s;

    uint32_t model = newGameModel(f, src);
    std::string log;
    importModel(f, m, {model}, opt, &log);
    keepNewestParts(f, model, (uint32_t)m.parts.size());
    uint32_t objectLike = f.follow(dispPtr(f, 0x14) + src * 12) ? src : 0;
    addObject(f, model, objectLike);

    uint32_t g = f.gsnh();
    uint32_t index = f.u32(g + 0x18C);
    uint32_t size = (0x14 + (uint32_t)name.size() + 1 + 15) & ~15u;
    std::vector<uint8_t> block(size, 0);
    std::memcpy(&block[0x14], name.c_str(), name.size());
    insertBytes(f, f.follow(g + 0x190) + index * 0x14, size, block.data());
    g = f.gsnh();
    f.put32(g + 0x18C, index + 1);
    uint32_t L = f.follow(g + 0x190) + index * 0x14;
    uint32_t object = dispPtr(f, 0x70) + (f.u32(dispPayload(f) + 0x6C) - 1) * OBJECT_SIZE;

    f.put32(g + 0x18C, index);
    uint32_t node = lastNodeEnd(f);
    f.put32(g + 0x18C, index + 1);
    insertBytes(f, node, 12);
    const Chunk* cdat = f.findChunk("CDAT");
    if (!cdat) throw Error("no CDAT chunk");
    insertBytes(f, cdat->end(), 4);

    writePointer(f, node, f.gsnhPayload());
    addReloc(f, node + 4);
    writePointer(f, node + 8, object);
    writePointer(f, L, L + 0x14);
    addReloc(f, L + 4);
    writePointer(f, L + 8, node);
    addReloc(f, L + 0xC);
    addReloc(f, L + 0x10);

    log += "  layer         " + std::to_string(index) + " '" + name + "', game model " + std::to_string(model) + ", copied the setup of model " + std::to_string(src) + "\n";
    if (report) *report = log;
    return index;
}

}