#include "verify.h"
#include "display.h"
#include "mesh.h"
#include <algorithm>
#include <map>
#include <set>
#include <unordered_set>

namespace no20 {

Report verify(const Nu20File& f) {
    Report r;
    auto err = [&](std::string s) { r.errors.push_back(std::move(s)); };
    auto warn = [&](std::string s) { r.warnings.push_back(std::move(s)); };

    int64_t sceneLen = (int64_t)f.package - f.base;
    if ((int64_t)f.s32(f.hdr(0x04)) != -sceneLen)
        err("header [0x04] = " + hex(f.s32(f.hdr(0x04))) + ", expected -(PNTR end - NU20) = " + hex(-sceneLen));
    if (sceneLen % 16) err("NU20 block length " + hex(sceneLen) + " isnt a multiple of 16");
    if (f.base && f.package != f.data.size()) warn("texture first file has data after pntr");
    if (f.u32(f.hdr(0x14)) != 0x10) warn("head size is " + hex(f.u32(f.hdr(0x14))) + " (expected 0x10)");

    uint32_t count = f.u32(f.pntr + 8);
    uint32_t entriesEnd = f.pntr + 12 + count * 4;
    uint32_t pad = f.package - entriesEnd;
    if (count != f.relocs.size()) err("PNTR count doesnt match parsed entries");
    if (pad >= 16) err("pntr has " + std::to_string(pad) + " bytes of trailing padding");
    for (uint32_t i = entriesEnd; i < f.package; i++)
        if (f.data[i] != 0) { err("PNTR padding byte at " + hex(i) + " isnt zero"); break; }
    if (f.relocs.empty() || f.relocs.back() != f.hdr(0x1C)) warn("last PNTR entry isnt the scene root [0x1C]");
    if (f.tagAt(f.sceneRoot - 8) != "GSNH") warn("scene root [0x1C] doesnt point at GSNH payload");

    std::unordered_set<uint32_t> seen;
    std::map<std::string, int> sentinelBy;
    int zero = 0, infile = 0, sentinel = 0, intoPkg = 0, misaligned = 0;
    for (uint32_t field : f.relocs) {
        if (!seen.insert(field).second) err("duplicate pntr entry for field " + f.describe(field));
        if (field % 4) misaligned++;
        Pointer p = f.pointerAt(field);
        switch (p.kind) {
            case PtrKind::Zero: zero++; break;
            case PtrKind::Sentinel: {
                sentinel++;
                const Chunk* c = f.chunkContaining(field);
                sentinelBy[c ? c->tag : "?"]++;
                break;
            }
            case PtrKind::InFile:
                infile++;
                if (p.target >= f.package) intoPkg++;
                break;
        }
    }
    if (misaligned) warn(std::to_string(misaligned) + " PNTR fields arent 4 aligned");
    if (intoPkg) warn(std::to_string(intoPkg) + " relocated pointers target the package");
    r.notes.push_back("relocs: " + std::to_string(f.relocs.size()) + " total, " + std::to_string(infile) + " in-file, " + std::to_string(zero) + " zero, " + std::to_string(sentinel) + " out of file");
    for (auto& [tag, n] : sentinelBy)
        r.notes.push_back("  out of file values in " + tag + ": " + std::to_string(n));

    const Chunk* ntbl = f.findChunk("NTBL");
    int badExtras = 0;
    for (auto& e : f.extras) {
        int64_t t = f.extraTarget(e);
        if (!ntbl || t < ntbl->offset + 12 || t >= ntbl->end()) badExtras++;
    }
    if (badExtras) err(std::to_string(badExtras) + " locator names dont resolve into NTBL");
    if (!f.extras.empty()) r.notes.push_back(std::to_string(f.extras.size()) + " locator name pointers");

    // mat count is in GSNH +0x10, MS00 +0 and DISP +0x4C
    //MATS MISSING FROM DISP ARENT DRAWN
    if (f.tagAt(f.sceneRoot - 8) == "GSNH") {
        uint32_t mats = f.u32(f.gsnhPayload() + 0x10);
        int32_t arrRel = f.s32(f.gsnhPayload() + 0x0C);
        if (mats && arrRel) {
            uint32_t slot0 = (uint32_t)((int64_t)f.gsnhPayload() + 0x0C + arrRel);
            uint32_t rec0 = f.ptr(slot0);
            uint32_t sub0 = rec0 + 0x40 <= f.pntr ? f.ptr(rec0 + 0x3C) : 0;
            for (auto& c : f.chunks) {
                if (c.tag != "DISP" || c.size < 0x60) continue;
                uint32_t pl = c.offset + 8;
                int32_t rel = f.s32(pl + 0x54);
                if (!rel) continue;
                uint32_t list = (uint32_t)((int64_t)pl + 0x54 + rel);
                if (list < c.offset || list + 4 > c.end() || f.ptr(list) != sub0) continue;
                if (f.u32(pl + 0x4C) != mats)
                    err("DISP per mat list has " + std::to_string(f.u32(pl + 0x4C)) + " entries but there are " + std::to_string(mats) + " mats");
            }
        }
    }

    //each segments mtl needs 2 point at its own nat, and each groups CLIP chain hits exactly its mats
    try {
        DispLayout L = readDisp(f);
        int wrongMtl = 0;
        for (size_t i = 0; i < L.subs.size(); i++) {
            uint32_t seg = f.ptr(L.subs[i] + 0x0C);
            uint32_t next = f.ptr(seg + 4);
            for (uint32_t c = seg; c < next && c + 16 <= f.pntr; c += 16)
                if (f.data[c] == 0x80 && (int64_t)c + 4 + f.s32(c + 4) != L.materials[i]) wrongMtl++;
        }
        if (wrongMtl) err(std::to_string(wrongMtl) + " material render segments draw a different material");
        for (size_t gi = 0; gi < L.groups.size(); gi++) {
            auto& g = L.groups[gi];
            uint32_t c = g.firstCommand, expect = g.first;
            for (uint32_t steps = 0; c && c + 16 <= f.pntr && f.data[c] == 0x85 && steps < 100000; steps++) {
                if (expect >= L.subs.size() || f.ptr(L.subs[expect] + 0x0C) != c) break;
                expect++;
                c = f.ptr(c + 4);
            }
            if (expect != (uint32_t)g.first + g.count || !c || f.data[c] != 0x84) {
                err("render group " + std::to_string(gi) + "s command chain doesnt visit exactly its mats");
                break;
            }
        }
    } catch (const Error&) {}

    //loader turns frames [0, max(first + count)) into tex and mat idx into ptrs
    try {
        TexAnimLayout t;
        if (readTexAnims(f, t)) {
            uint32_t textures = f.u32(f.gsnhPayload() + 4), mats = f.u32(f.gsnhPayload() + 0x10);
            uint32_t used = 0, badMat = 0, badTex = 0;
            for (uint32_t i = 0; i < t.count; i++) {
                used = std::max(used, f.u32(t.record(i) + 8) + f.u16(t.record(i) + 0x0C));
                if (f.u32(t.record(i) + 0x10) >= mats) badMat++;
            }
            for (uint32_t i = 0; i < t.total; i++)
                if (f.u16(t.table + i * 2) >= textures) badTex++;
            if (used != t.total)
                err("TAS0 frame total " + std::to_string(t.total) + " but its animations use " + std::to_string(used));
            if (badMat) err(std::to_string(badMat) + " TAS0 animations name a material that doesnt exist");
            if (badTex) err(std::to_string(badTex) + " TAS0 frames name a texture that doesnt exist");
            for (uint32_t i = t.table + t.total * 2; i < t.chunkEnd; i++)
                if (f.data[i]) { err("TAS0 padding isnt zero"); break; }
        }
    } catch (const Error& e) {
        err(e.what());
    }

    Nu20File copy = f;
    try {
        copy.rebuildPntr();
        if (copy.data != f.data) {
            size_t i = 0;
            while (i < f.data.size() && i < copy.data.size() && f.data[i] == copy.data[i]) i++;
            err("pntr+header regeneration isnt byte identical (first diff at " + hex((int64_t)i) + ")");
        } else {
            r.notes.push_back("pntr+header regenerate byte identically");
        }
    } catch (const Error& e) {
        err(std::string("pntr regeneration failed: ") + e.what());
    }

    checkMeshes(f, r.errors);
    checkModelLists(f, r.errors);
    return r;
}

std::vector<std::string> checkIndexedLists(const Nu20File& before, const Nu20File& after) {
    std::vector<std::string> out;
    auto a = before.indexedLists(), b = after.indexedLists();
    if (a.size() != b.size()) return {"number of index addressed lists changed"};
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].end - a[i].start != b[i].end - b[i].start) {
            out.push_back(std::string(a[i].what) + ": element count changed");
            continue;
        }
        for (uint32_t off = 0; off < a[i].end - a[i].start; off += 16)
            if (before.u32(a[i].start + off) != after.u32(b[i].start + off)) {
                out.push_back(std::string(a[i].what) + ": element " + std::to_string(off / 16) + " changed or moved");
                break;
            }
    }
    return out;
}

}