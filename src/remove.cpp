#include "remove.h"
#include <algorithm>
#include <cstring>
#include <vector>
#include "content.h"
#include "display.h"
#include "edit.h"
#include "layer.h"
#include "mesh.h"

namespace no20 {

static const uint32_t CMD = 16, MATRIX = 0x40, MESH_RECORD = 0x38, SUB_SIZE = 0x44, NEXT_SIZE = 0x10;
static const uint8_t GEOMCALL = 0x82, MTXLOAD = 0x83, CALLBLOCK = 0xB0;

static void put16(Nu20File& f, uint32_t at, uint16_t v) { std::memcpy(&f.data[at], &v, 2); }

static uint32_t dispPayload(const Nu20File& f) {
    const Chunk* c = f.findChunk("DISP");
    if (!c) throw Error("no DISP chunk");
    return c->offset + 8;
}

static uint32_t meshHeader(const Nu20File& f) { return f.follow(f.gsnhPayload() + 0x1D0); }

static bool anyPointerInto(const Nu20File& f, uint32_t from, uint32_t to) {
    for (auto& p : f.pointers())
        if (p.kind == PtrKind::InFile && (p.field < from || p.field >= to) && p.target >= from && p.target < to) return true;
    for (auto& e : f.extras) {
        int64_t t = f.extraTarget(e);
        if (t >= from && t < to) return true;
    }
    return false;
}

static void removeTracked(Nu20File& f, uint32_t at, uint32_t n, std::vector<uint32_t*> tracked) {
    removeBytes(f, at, n);
    for (uint32_t* p : tracked)
        if (*p >= at + n) *p -= n;
}

static void packageRemove(Nu20File& f, uint32_t at, uint32_t n) {
    Package pk = f.parsePackage();
    if (!pk.parsed) throw Error("couldnt read the package: " + pk.error);
    f.data.erase(f.data.begin() + at, f.data.begin() + at + n);
    if (!pk.textureFirst) {
        f.put32(f.package, f.u32(f.package) - n);
        return;
    }
    uint32_t end = pk.end - n, pad = f.base - n - end;
    uint32_t extra = (0x100 - n % 0x100) % 0x100;
    if (extra <= pad) {
        f.data.erase(f.data.begin() + end, f.data.begin() + end + extra);
        f.put32(0, f.u32(0) - n - extra);
    } else {
        uint32_t back = n % 0x100;
        f.data.insert(f.data.begin() + end, back, 0);
        f.put32(0, f.u32(0) - (n - back));
    }
    f.parse();
}

static void forgetName(Nu20File& f, uint32_t name) {
    const Chunk* c = f.findChunk("NTBL");
    if (!c || !name) return;
    uint32_t lenField = c->offset + 8, start = c->offset + 12, len = f.u32(lenField);
    if (name < start || name >= start + len) return;
    uint32_t size = (uint32_t)std::strlen((const char*)&f.data[name]) + 1;
    if (name + size != start + len || anyPointerInto(f, name, name + size)) return;
    std::fill(f.data.begin() + name, f.data.begin() + name + size, (uint8_t)0);
    f.put32(lenField, len - size);
    uint32_t chunkEnd = c->end(), used = ((name - f.base + 15) & ~15u) + f.base;
    if (used < chunkEnd && !anyPointerInto(f, used, chunkEnd)) removeBytes(f, used, chunkEnd - used);
}

static void removeOneAnimation(Nu20File& f, uint32_t index) {
    TexAnimLayout t;
    readTexAnims(f, t);
    uint32_t rec = t.record(index);
    uint32_t name18 = f.follow(rec + 0x18), name1C = f.follow(rec + 0x1C);
    if (t.count == 1) {
        uint32_t g = f.gsnhPayload();
        f.put32(g + 0x60, 0);
        f.put32(g + 0x64, 0);
        f.put32(g + 0x68, 0);
        const Chunk* c = f.findChunk("TAS0");
        uint32_t at = c->offset, size = c->size;
        InsertOptions opt;
        opt.growChunks = false;
        removeBytes(f, at, size, opt);
        f.parse();
        forgetName(f, name1C);
        if (name18 != name1C) forgetName(f, name18);
        return;
    }

    uint32_t first = f.u32(rec + 8), frames = f.u16(rec + 0xC);
    std::vector<uint8_t> table(f.data.begin() + t.table, f.data.begin() + t.table + t.total * 2);
    table.erase(table.begin() + first * 2, table.begin() + (first + frames) * 2);
    for (uint32_t i = 0; i < t.count; i++)
        if (i != index && f.u32(t.record(i) + 8) > first) f.put32(t.record(i) + 8, f.u32(t.record(i) + 8) - frames);
    uint32_t total = t.total - frames;
    f.put32(t.totalField, total);
    std::fill(f.data.begin() + t.table, f.data.begin() + t.chunkEnd, (uint8_t)0);
    std::copy(table.begin(), table.end(), f.data.begin() + t.table);

    for (uint32_t i = index; i + 1 < t.count; i++) {
        uint32_t to = t.record(i), from = t.record(i + 1);
        uint32_t n18 = f.follow(from + 0x18), n1C = f.follow(from + 0x1C);
        std::memmove(&f.data[to], &f.data[from], 0x18);
        f.put32(to + 0x18, n18 ? (uint32_t)(int32_t)((int64_t)n18 - (to + 0x18)) : 0);
        f.put32(to + 0x1C, n1C ? (uint32_t)(int32_t)((int64_t)n1C - (to + 0x1C)) : 0);
    }
    uint32_t payload = t.payload, remaining = t.count - 1;
    removeBytes(f, t.record(t.count - 1), 0x20);
    f.put32(payload, remaining);
    f.put32(f.gsnhPayload() + 0x60, remaining);
    readTexAnims(f, t);

    uint32_t chunkStart = t.payload - 8;
    uint32_t align = (t.chunkEnd - chunkStart) % 16 == 0 ? 16 : 4;
    uint32_t used = t.table + total * 2 - chunkStart;
    used = (used + align - 1) & ~(align - 1);
    uint32_t spare = t.chunkEnd - chunkStart - used;
    if (spare) removeBytes(f, chunkStart + used, spare);
    forgetName(f, name1C);
    if (name18 != name1C) forgetName(f, name18);
}

uint32_t removeAnimations(Nu20File& f, uint32_t material, std::string* report) {
    uint32_t removed = 0;
    for (;;) {
        TexAnimLayout t;
        if (!readTexAnims(f, t)) break;
        uint32_t found = t.count;
        for (uint32_t i = 0; i < t.count; i++)
            if (f.u32(t.record(i) + 0x10) == material) found = i;
        if (found == t.count) break;
        removeOneAnimation(f, found);
        removed++;
    }
    if (!removed) throw Error("mat " + std::to_string(material) + " has no animation");
    if (report) *report = "  animation     removed " + std::to_string(removed) + " from material " + std::to_string(material) + "\n";
    return removed;
}

void removeLayer(Nu20File& f, uint32_t layer, std::string* report) {
    auto layers = characterLayers(f);
    if (layers.empty()) throw Error("no layers, not a GHG character");
    if (layer >= layers.size()) throw Error("no layer " + std::to_string(layer) + " (this file has " + std::to_string(layers.size()) + ")");
    uint32_t L = f.follow(f.gsnh() + 0x190) + layer * 0x14;
    bool anything = false;
    for (uint32_t off = 0; off < 0x14; off += 4)
        if (f.s32(L + off)) anything = true;
    if (!anything) throw Error("layer " + std::to_string(layer) + " is already empty");
    for (uint32_t off = 0; off < 0x14; off += 4) f.put32(L + off, 0);
    if (report) {
        std::string models;
        for (uint32_t m : layers[layer].skinned) models += " " + std::to_string(m);
        for (auto& r : layers[layer].rigid) models += " " + std::to_string(r.first);
        *report = "  layer         " + std::to_string(layer) + " '" + layers[layer].name + "' is now empty, the number stays so later layers dont move\n";
        if (!models.empty()) *report += "  note: its game model(s)" + models + " are still in the file, remove-mesh gets rid of them\n";
    }
}

static void removeGeometry(Nu20File& f, const MeshRecord& gone, std::string& log) {
    auto meshes = meshRecords(f);
    Package pk = f.parsePackage();
    if (!pk.parsed) throw Error("couldnt read the package: " + pk.error);
    if (gone.vertexBuffer >= pk.vertexBuffers.size() || gone.indexBuffer >= pk.indexBuffers.size()) return;

    uint32_t indices = gone.primitives + 2;
    bool sharedVerts = false, sharedIndices = false;
    for (auto& m : meshes) {
        if (m.vertexBuffer == gone.vertexBuffer && m.baseVertex < gone.baseVertex + gone.vertexCount && gone.baseVertex < m.baseVertex + m.vertexCount) sharedVerts = true;
        if (m.indexBuffer == gone.indexBuffer && m.firstIndex < gone.firstIndex + indices && gone.firstIndex < m.firstIndex + m.primitives + 2) sharedIndices = true;
    }
    GeomBuffer ib = pk.indexBuffers[gone.indexBuffer];
    if (!sharedIndices && indices * 2 < ib.size && (gone.firstIndex + indices) * 2 <= ib.size) {
        for (auto& m : meshes)
            if (m.indexBuffer == gone.indexBuffer && m.firstIndex > gone.firstIndex) f.put32(m.record + 0x1C, m.firstIndex - indices);
        packageRemove(f, ib.sizeField + 4 + gone.firstIndex * 2, indices * 2);
        f.put32(ib.sizeField, ib.size - indices * 2);
        meshes = meshRecords(f);
        pk = f.parsePackage();
    }

    GeomBuffer vb = pk.vertexBuffers[gone.vertexBuffer];
    uint32_t bytes = gone.vertexCount * gone.stride;
    if (sharedVerts || (uint64_t)(gone.baseVertex + gone.vertexCount) * gone.stride > vb.size) {
        log += "  note: its vertices are shared or dont fit, left them in the buffer\n";
        return;
    }
    if (bytes == vb.size) {
        if (gone.vertexBuffer + 1 != pk.vertexBuffers.size()) {
            log += "  note: left its vertices in, emptying a buffer in the middle would renumber the others\n";
            return;
        }
        uint32_t id = gone.vertexBuffer;
        packageRemove(f, vb.sizeField, 4 + vb.size);
        put16(f, f.parsePackage().texturesEnd, (uint16_t)id);
        uint32_t h = meshHeader(f);
        uint16_t slots = f.u16(h);
        removeBytes(f, f.follow(h + 4) + (slots - 1) * 4, 4);
        put16(f, meshHeader(f), (uint16_t)(slots - 1));
        log += "  geometry      removed vertex buffer " + std::to_string(id) + ", nothing else used it\n";
        return;
    }
    for (auto& m : meshes)
        if (m.vertexBuffer == gone.vertexBuffer && m.baseVertex > gone.baseVertex) f.put32(m.record + 0x14, m.baseVertex - gone.vertexCount);
    packageRemove(f, vb.sizeField + 4 + gone.baseVertex * gone.stride, bytes);
    f.put32(vb.sizeField, vb.size - bytes);
    log += "  geometry      removed " + std::to_string(gone.vertexCount) + " vertices, " + std::to_string(indices) + " indices\n";
}

static void removeCommandPair(Nu20File& f, uint32_t index, std::string& log) {
    DispLayout L = readDisp(f, 0);
    if (index == 0 || index >= L.commandCount) return;
    uint32_t geom = L.commandAt(index), mtxCmd = geom - CMD;
    if (f.data[geom] != GEOMCALL || f.data[mtxCmd] != MTXLOAD) return;
    uint32_t mesh = f.follow(geom + 4), matrix = f.follow(mtxCmd + 4);

    uint32_t start = index - 1, count = 2;
    if (start > 0 && f.data[L.commandAt(start - 1)] == CALLBLOCK && index + 1 < L.commandCount && f.data[geom + CMD] != MTXLOAD) {
        start--;
        count++;
    }
    removeTracked(f, L.commandAt(start), count * CMD, {&mesh, &matrix});
    for (uint32_t field : modelCommandIndexFields(f))
        if (f.u32(field) > index) f.put32(field, f.u32(field) - count);
    uint32_t pl = dispPayload(f);
    f.put32(pl + 4, f.u32(pl + 4) - count);

    L = readDisp(f, 0);
    bool matrixUsed = false, meshUsed = false;
    for (uint32_t i = 0; i < L.commandCount; i++) {
        uint32_t c = L.commandAt(i);
        if (f.data[c] == MTXLOAD && f.follow(c + 4) == matrix) matrixUsed = true;
        if (f.data[c] == GEOMCALL && f.follow(c + 4) == mesh) meshUsed = true;
    }
    uint32_t meshAt = mesh;
    if (matrix && !matrixUsed && !anyPointerInto(f, matrix, matrix + MATRIX)) {
        removeBytes(f, matrix, MATRIX);
        if (meshAt >= matrix + MATRIX) meshAt -= MATRIX;
    }
    if (!mesh || meshUsed) return;

    auto meshes = meshRecords(f);
    auto it = std::find_if(meshes.begin(), meshes.end(), [&](const MeshRecord& m) { return m.record == meshAt; });
    if (it == meshes.end()) return;
    if (it->dynamic) {
        log += "  note: mesh is a morphing one, its data stays in the file\n";
        return;
    }
    MeshRecord gone = *it;
    uint32_t i = (uint32_t)(it - meshes.begin());
    uint32_t h = meshHeader(f);
    uint32_t n = f.u32(h + 0x14);
    uint32_t slot = f.follow(h + 0x10) + i * 4;
    removeTracked(f, slot, 4, {&meshAt});
    f.put32(meshHeader(f) + 0x14, n - 1);
    if (!anyPointerInto(f, meshAt, meshAt + MESH_RECORD)) removeBytes(f, meshAt, MESH_RECORD);
    removeGeometry(f, gone, log);
}

void removeModelParts(Nu20File& f, uint32_t model, int64_t part, std::string* report) {
    uint32_t pl = dispPayload(f);
    if (model >= f.u32(pl + 0x10)) throw Error("no game model " + std::to_string(model));
    uint32_t parts = f.u32(f.follow(pl + 0x14) + model * 12);
    if (!parts) throw Error("game model " + std::to_string(model) + " has no parts");
    if (part >= (int64_t)parts) throw Error("game model " + std::to_string(model) + " only has " + std::to_string(parts) + " part(s)");
    bool ruled = capacityRuleHolds(f);

    std::string log;
    std::vector<uint32_t> touched;
    uint32_t last = part >= 0 ? (uint32_t)part : parts - 1, firstPart = part >= 0 ? (uint32_t)part : 0;
    for (int64_t j = last; j >= firstPart; j--) {
        pl = dispPayload(f);
        uint32_t rec = f.follow(pl + 0x14) + model * 12;
        uint32_t cl = f.follow(rec + 8), command = f.u32(cl + (uint32_t)j * 4);
        uint32_t ml = f.follow(rec + 4), material = f.u32(ml + (uint32_t)j * 4);
        if (std::find(touched.begin(), touched.end(), material) == touched.end()) touched.push_back(material);
        uint32_t count = f.u32(rec);
        removeBytes(f, cl + (uint32_t)j * 4, 4);
        pl = dispPayload(f);
        rec = f.follow(pl + 0x14) + model * 12;
        removeBytes(f, f.follow(rec + 4) + (uint32_t)j * 4, 4);
        pl = dispPayload(f);
        rec = f.follow(pl + 0x14) + model * 12;
        f.put32(rec, count - 1);
        if (uint32_t sizes = f.follow(pl + 0x18)) put16(f, sizes + model * 2, (uint16_t)(count - 1));
        syncFirstParts(f);

        auto fields = modelCommandIndexFields(f);
        bool stillUsed = std::any_of(fields.begin(), fields.end(), [&](uint32_t field) { return f.u32(field) == command; });
        if (!stillUsed) removeCommandPair(f, command, log);
    }
    syncFirstParts(f);

    if (ruled) {
        for (uint32_t m : touched) {
            DispLayout l = readDisp(f);
            if (m >= l.subs.size()) continue;
            uint32_t sub = l.subs[m], a = f.follow(sub + 0x28), b = f.follow(sub + 0x2C), old = f.u32(sub + 0x38);
            uint32_t need = 2 * materialUses(f, m) + 1;
            if (!a || !b || need >= old || b != a + old * 0x10) continue;
            uint32_t drop = (old - need) * 0x10;
            removeBytes(f, b + need * 0x10, drop);
            removeBytes(f, a + need * 0x10, drop);
            f.put32(readDisp(f).subs[m] + 0x38, need);
        }
    }
    if (report) {
        uint32_t n = last - firstPart + 1;
        *report = "  parts         removed " + std::to_string(n) + " from game model " + std::to_string(model) + "\n" + log;
    }
}

static void shrinkMaterialBits(Nu20File& f, uint32_t oldCount) {
    auto bytesFor = [](uint32_t n) { return (n + 8 + 127) / 128 * 16; };
    uint32_t oldSize = bytesFor(oldCount), drop = oldSize - bytesFor(oldCount - 1);
    if (!drop) return;
    uint32_t pl = dispPayload(f);
    uint32_t a = f.follow(pl + 0x58), b = f.follow(pl + 0x5C), c = f.follow(pl + 0x48);
    if (b != a + oldSize || c != b + oldSize) return;
    removeBytes(f, c - drop, drop);
    pl = dispPayload(f);
    removeBytes(f, f.follow(pl + 0x5C) - drop, drop);
}

void removeMaterial(Nu20File& f, uint32_t material, std::string* report) {
    DispLayout L = readDisp(f);
    uint32_t M = (uint32_t)L.materials.size();
    if (material >= M) throw Error("no material " + std::to_string(material));
    if (M < 2) throw Error("cant remove the only material in a file");
    for (uint32_t i = 1; i < M; i++) {
        if (L.subs[i] != L.subs[i - 1] + SUB_SIZE) throw Error("render buckets arent one array in material order");
        if (L.materials[i] != L.materials[1] - L.materials[0] + L.materials[i - 1]) throw Error("material records arent one array in material order");
    }
    uint32_t recSize = L.materials[1] - L.materials[0];

    std::string users;
    auto modelLists = modelMaterialIndexFields(f);
    for (uint32_t field : modelLists)
        if (f.u32(field) == material) {
            users = "game models use it, remove-mesh those parts first";
            break;
        }
    if (!users.empty()) throw Error("material " + std::to_string(material) + " is in use: " + users);
    for (uint32_t field : animatedMaterialFields(f))
        if (f.u32(field) == material) throw Error("material " + std::to_string(material) + " has an animation, remove-animation first");

    auto group = std::find_if(L.groups.begin(), L.groups.end(), [&](const RenderGroup& g) { return material >= g.first && material < (uint32_t)g.first + g.count; });
    if (group == L.groups.end()) throw Error("material " + std::to_string(material) + " isnt in a render group");
    if (group->count < 2) throw Error("material " + std::to_string(material) + " is the only one in its render group");

    uint32_t rec = L.materials[material], sub = L.subs[material];
    uint32_t seg = f.follow(sub + 0x0C), segEnd = f.follow(seg + 4);
    if (seg < L.commands || segEnd <= seg || (segEnd - seg) % CMD) throw Error("material cmd segment isnt CLIP linked");
    uint32_t segIndex = L.commandIndex(seg), segCount = (segEnd - seg) / CMD;
    auto commandFields = modelCommandIndexFields(f);
    for (uint32_t field : commandFields) {
        uint32_t v = f.u32(field);
        if (v >= segIndex && v < segIndex + segCount) throw Error("a game model draws a command in this materials segment");
    }

    uint32_t block = f.follow(sub + 8), next = f.follow(sub + 0x10), bufA = f.follow(sub + 0x28), cap = f.u32(sub + 0x38);
    uint32_t blockSize = !f.base ? 0x24u : f.version >= 3 ? 0x28u : 0x34u;
    if (material + 1 < M) {
        uint32_t nextBlock = f.follow(L.subs[material + 1] + 8);
        if (nextBlock > block) blockSize = nextBlock - block;
    }

    for (uint32_t field : modelLists)
        if (f.u32(field) > material) f.put32(field, f.u32(field) - 1);
    for (uint32_t field : animatedMaterialFields(f))
        if (f.u32(field) > material) f.put32(field, f.u32(field) - 1);
    for (auto& g : L.groups)
        if (g.first > material) put16(f, g.record + 0x22, (uint16_t)(g.first - 1));
    put16(f, group->record + 0x20, (uint16_t)(group->count - 1));
    for (uint32_t field : commandFields)
        if (f.u32(field) >= segIndex + segCount) f.put32(field, f.u32(field) - segCount);
    f.put32(L.payload + 4, f.u32(L.payload + 4) - segCount);

    std::vector<uint32_t*> tracked = {&rec, &sub, &seg, &block, &next, &bufA};
    uint32_t pl = L.payload;
    uint32_t list = f.follow(pl + 0x54);
    removeTracked(f, list + material * 4, 4, tracked);
    pl = dispPayload(f);
    list = f.follow(pl + 0x54);
    uint32_t listEnd = list + (M - 1) * 4;
    bool padded = !std::any_of(f.data.begin() + listEnd, f.data.begin() + listEnd + 12, [](uint8_t b) { return b != 0; }) && !anyPointerInto(f, listEnd, listEnd + 12);
    if (padded) removeTracked(f, listEnd, 12, tracked);
    f.put32(dispPayload(f) + 0x4C, M - 1);

    uint32_t arr = f.follow(f.gsnhPayload() + 0x0C);
    removeTracked(f, arr + material * 4, 4, tracked);
    f.put32(f.gsnhPayload() + 0x10, M - 1);
    if (const Chunk* ms = f.findChunk("MS00"); ms && f.u32(ms->offset + 8) == M) f.put32(ms->offset + 8, M - 1);

    removeTracked(f, sub, SUB_SIZE, tracked);
    removeTracked(f, seg, segCount * CMD, tracked);
    if (bufA && cap) removeTracked(f, bufA, 2 * cap * 0x10, tracked);
    if (next) removeTracked(f, next, NEXT_SIZE, tracked);
    if (block) removeTracked(f, block, blockSize, tracked);
    removeTracked(f, rec, recSize, tracked);

    DispLayout fin = readDisp(f, M - 1, false);
    for (uint32_t i = 0; i < M - 1; i++) {
        f.put32(fin.materials[i] + 0x38, i);
        f.put32(fin.subs[i] + 4, i);
    }
    shrinkMaterialBits(f, M);
    readDisp(f);
    if (report) *report = "  material      " + std::to_string(material) + " removed, the ones after it moved down by 1\n";
}

uint32_t removeTexture(Nu20File& f, uint32_t texture, std::string* report) {
    auto tex = f.textures();
    if (texture >= tex.size()) throw Error("no texture " + std::to_string(texture));
    if (!tex[texture].width) throw Error("texture " + std::to_string(texture) + " is an empty slot, remove the cubemap its part of");
    bool cube = isCubemap(f, (int32_t)texture);
    uint32_t n = cube ? 6 : 1;
    if (texture + n > tex.size()) throw Error("cubemap " + std::to_string(texture) + " doesnt have its 6 slots");
    uint32_t descSize = f.base ? 0x28 : 0x80;

    auto mats = materials(f);
    uint32_t recSize = mats.size() >= 2 ? mats[1].record - mats[0].record : kMaterialRecordSize;
    if (recSize != kMaterialRecordSize) throw Error("this files materials arent the normal size");
    auto inRange = [&](int32_t v) { uint32_t i = (uint32_t)v & 0x3FFF; return v != -1 && i >= texture && i < texture + n; };
    for (uint32_t m = 0; m < mats.size(); m++)
        for (auto& fld : materialFields())
            if (fld.type == 't' && inRange(f.s32(mats[m].record + fld.off)))
                throw Error("texture " + std::to_string(texture) + " is in use by material " + std::to_string(m) + " (" + fld.name + "), change that first");
    TexAnimLayout t;
    bool animated = readTexAnims(f, t);
    if (animated)
        for (uint32_t i = 0; i < t.total; i++)
            if (inRange(f.u16(t.table + i * 2))) throw Error("texture " + std::to_string(texture) + " is a frame in a texture animation, remove-animation first");

    for (auto& m : mats) {
        for (auto& fld : materialFields()) {
            if (fld.type != 't') continue;
            int32_t v = f.s32(m.record + fld.off);
            if (v != -1 && ((uint32_t)v & 0x3FFF) >= texture + n) f.put32(m.record + fld.off, (uint32_t)v - n);
        }
        int16_t t16 = (int16_t)f.u16(m.record + 0x74);
        if (t16 > 0 && (t16 & 0x3FFF) >= (int32_t)(texture + n)) put16(f, m.record + 0x74, (uint16_t)(t16 - n));
    }
    if (animated)
        for (uint32_t i = 0; i < t.total; i++) {
            uint32_t v = f.u16(t.table + i * 2);
            if (v >= texture + n) put16(f, t.table + i * 2, (uint16_t)(v - n));
        }

    if (tex[texture].data && tex[texture].size) {
        if (f.base) {
            packageRemove(f, tex[texture].data - 0x18, 0x18 + tex[texture].size);
            put16(f, 4, (uint16_t)(f.u16(4) - 1));
        } else {
            f.data.erase(f.data.begin() + tex[texture].data, f.data.begin() + tex[texture].data + tex[texture].size);
            f.put32(f.package, f.u32(f.package) - tex[texture].size);
        }
    }

    uint32_t g = f.gsnhPayload();
    uint32_t count = f.u32(g + 4);
    uint32_t array = f.ptr(g + 8);
    uint32_t list = f.s32(g) ? f.ptr(g) : 0;
    bool together = list && list + count * 4 == array;
    uint32_t desc = f.follow(array + texture * 4);
    uint32_t arrayEnd = array + count * 4;
    bool padded = n % 2 && !std::any_of(f.data.begin() + arrayEnd, f.data.begin() + arrayEnd + 8, [](uint8_t b) { return b != 0; }) && !anyPointerInto(f, arrayEnd, arrayEnd + 8);
    if (padded && (arrayEnd + 8 - array) >= 0) removeBytes(f, arrayEnd, 8);
    removeBytes(f, array + texture * 4, n * 4);
    g = f.gsnhPayload();
    if (list) removeBytes(f, f.ptr(g) + texture * 4, n * 4);
    if (!together && !padded) {
        g = f.gsnhPayload();
        uint32_t arr2 = f.ptr(g + 8);
        insertBytes(f, arr2 + (count - n) * 4, n * 4);
    }
    f.put32(f.gsnhPayload() + 4, count - n);

    uint32_t descBytes = descSize * n;
    const Chunk* home = f.chunkContaining(desc);
    if (!home) throw Error("texture descriptor isnt in a chunk");
    std::string tag = home->tag;
    if (!anyPointerInto(f, desc, desc + descBytes)) {
        removeBytes(f, desc, descBytes);
        const Chunk* again = f.findChunk(tag);
        if (again) insertBytes(f, again->end(), descBytes);
    } else {
        std::fill(f.data.begin() + desc, f.data.begin() + desc + descBytes, (uint8_t)0);
    }
    if (!f.parsePackage().parsed) throw Error("package doesnt parse anymore");
    if (report) *report = std::string("  texture       ") + std::to_string(texture) + (cube ? " (cubemap, 6 numbers)" : "") + " removed, the ones after it moved down by " + std::to_string(n) + "\n";
    return n;
}

}
