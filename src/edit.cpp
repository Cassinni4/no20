#include "edit.h"
#include <algorithm>

namespace no20 {

void insertBytes(Nu20File& f, uint32_t at, uint32_t n, const uint8_t* fill, const InsertOptions& opt) {
    if (n == 0) return;
    if (n % 4) throw Error("insert size must be a multiple of 4");
    if (at < f.sceneStart() || at > f.pntr) throw Error("insert point " + hex(at) + " is outside the scene region");
    if (at % 4) throw Error("insert point must be 4 aligned");
    if (!opt.allowInsideIndexedList && f.insideIndexedList(at))
        throw Error("insert point " + f.describe(at) + " is inside an index addressed list");

    auto movedField = [&](int64_t pos) { return pos >= at ? pos + n : pos; };
    auto movedTarget = [&](int64_t pos) {
        if (pos > at || (pos == at && opt.targetAtPointMoves)) return pos + n;
        return pos;
    };

    std::vector<Pointer> ptrs = f.pointers();
    std::vector<std::pair<ExtraPointer, int64_t>> extras;
    for (auto& e : f.extras) extras.push_back({e, f.extraTarget(e)});
    std::vector<uint32_t> sizeFields;
    if (opt.growChunks)
        for (auto& c : f.chunks)
            if (!c.tag.empty() && c.tag != "PNTR" && at >= c.offset + 8 && at <= c.end())
                sizeFields.push_back(c.offset + 4);

    if (fill) f.data.insert(f.data.begin() + at, fill, fill + n);
    else f.data.insert(f.data.begin() + at, n, 0);

    for (uint32_t sf : sizeFields) f.put32(sf, f.u32(sf) + n); // all of these are before at

    for (size_t i = 0; i < ptrs.size(); i++) {
        const Pointer& p = ptrs[i];
        uint32_t nf = (uint32_t)movedField(p.field);
        if (p.kind == PtrKind::InFile) f.put32(nf, (uint32_t)(int32_t)(movedTarget(p.target) - nf));
        f.relocs[i] = nf;
    }
    for (auto& [e, target] : extras) {
        ExtraPointer moved = e;
        moved.field = (uint32_t)movedField(e.field);
        f.setExtraTarget(moved, movedTarget(target));
    }

    f.pntr += n;
    f.package += n;
    f.rebuildPntr();
}

void removeBytes(Nu20File& f, uint32_t at, uint32_t n, const InsertOptions& opt) {
    if (n == 0) return;
    if (n % 4 || at % 4) throw Error("remove offset and size must be multiples of 4");
    if (at < f.sceneStart() || (uint64_t)at + n > f.pntr) throw Error("remove range is outside the scene region");
    uint32_t end = at + n;

    std::vector<Pointer> ptrs = f.pointers();
    for (auto& p : ptrs) {
        bool fieldInside = p.field + 4 > at && p.field < end;
        if (fieldInside) {
            if (p.field < at || p.field + 4 > end) throw Error("remove range splits pointer field " + hex(p.field));
            continue;
        }
        if (p.kind == PtrKind::InFile && p.target > at && p.target < end)
            throw Error("pointer at " + f.describe(p.field) + " targets removed range (" + f.describe(p.target) + ")");
    }
    std::vector<std::pair<ExtraPointer, int64_t>> extras;
    for (auto& e : f.extras) {
        if (e.field >= at && e.field < end) continue;
        int64_t t = f.extraTarget(e);
        if (t > at && t < end) throw Error(std::string(e.what) + " at " + f.describe(e.field) + " targets removed range");
        extras.push_back({e, t});
    }
    std::vector<uint32_t> sizeFields;
    if (opt.growChunks)
        for (auto& c : f.chunks)
            if (!c.tag.empty() && c.tag != "PNTR" && at >= c.offset + 8 && end <= c.end())
                sizeFields.push_back(c.offset + 4);

    auto moved = [&](int64_t pos) { return pos >= end ? pos - n : pos; };

    f.data.erase(f.data.begin() + at, f.data.begin() + end);
    for (uint32_t sf : sizeFields) f.put32(sf, f.u32(sf) - n);

    std::vector<uint32_t> kept;
    kept.reserve(ptrs.size());
    for (auto& p : ptrs) {
        if (p.field >= at && p.field < end) continue;
        uint32_t nf = (uint32_t)moved(p.field);
        if (p.kind == PtrKind::InFile) f.put32(nf, (uint32_t)(int32_t)(moved(p.target) - nf));
        kept.push_back(nf);
    }
    for (auto& [e, target] : extras) {
        ExtraPointer m = e;
        m.field = (uint32_t)moved(e.field);
        f.setExtraTarget(m, moved(target));
    }
    f.relocs = std::move(kept);
    f.pntr -= n;
    f.package -= n;
    f.rebuildPntr();
}

void addReloc(Nu20File& f, uint32_t field) {
    if (field % 4) throw Error("relocation field must be 4 alignedd");
    if (field + 4 > f.pntr) throw Error("relocation field must be inside the scene");
    if (std::find(f.relocs.begin(), f.relocs.end(), field) != f.relocs.end()) return;
    auto pos = f.relocs.end();
    if (!f.relocs.empty() && f.relocs.back() == f.hdr(0x1C)) --pos;
    f.relocs.insert(pos, field);
    f.rebuildPntr();
}

void writePointer(Nu20File& f, uint32_t field, uint32_t target, bool registerReloc) {
    f.put32(field, (uint32_t)(int32_t)((int64_t)target - field));
    if (registerReloc) addReloc(f, field);
}

}