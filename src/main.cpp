#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <cctype>
#include "content.h"
#include "display.h"
#include "edit.h"
#include "image.h"
#include "mesh.h"
#include "nu20.h"
#include "verify.h"
#include "layer.h"
#include "remove.h"

using namespace no20;

typedef std::vector<std::string> Args;

static const uint8_t GEOMCALL = 0x82;

static uint32_t num(const std::string& s) { return (uint32_t)std::stoul(s, nullptr, 0); }
static int64_t num64(const std::string& s) { return std::stoll(s, nullptr, 0); }

static bool isNumber(const std::string& s, uint32_t& out) {
    size_t used = 0;
    try {
        out = (uint32_t)std::stoul(s, &used, 0);
    } catch (...) {
        return false;
    }
    return used == s.size();
}

static void printReport(const Report& r) {
    for (auto& s : r.notes) std::printf("  note: %s\n", s.c_str());
    for (auto& s : r.warnings) std::printf("  WARN: %s\n", s.c_str());
    for (auto& s : r.errors) std::printf("  ERROR: %s\n", s.c_str());
    std::printf("%s\n", r.ok() ? "OK" : "FAILED");
}

static int writeIfOk(const Nu20File& f, const std::string& out, const Report& r) {
    if (!r.ok()) {
        printReport(r);
        std::printf("not written\n");
        return 1;
    }
    f.save(out);
    std::printf("verified, wrote %s\n", out.c_str());
    return 0;
}

static int saveEdit(const Nu20File& orig, const Nu20File& f, const std::string& out) {
    Report r = verify(f);
    for (auto& s : checkIndexedLists(orig, f)) r.errors.push_back(s);
    return writeIfOk(f, out, r);
}

static int saveNewMaterial(const Nu20File& orig, const Nu20File& f, const std::string& out) {
    Report r = verify(f);
    for (auto& s : checkModelCommandRefs(orig, f)) r.errors.push_back(s);
    for (auto& s : checkMaterialRefs(orig, f)) r.errors.push_back(s);
    return writeIfOk(f, out, r);
}

static int saveMeshEdit(const Nu20File& orig, const Nu20File& f, const std::string& out, const std::vector<uint32_t>& replaced = {}) {
    Report r = verify(f);
    for (auto& s : checkModelPartsKept(orig, f, replaced)) r.errors.push_back(s);
    return writeIfOk(f, out, r);
}

static int cmdInfo(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    std::printf("%s\n  size %s  version %u\n", a[0].c_str(), hex((int64_t)f.data.size()).c_str(), f.version);
    std::printf("  PNTR @%s (%zu relocs)  package @%s (%s bytes)  scene root -> %s\n", hex(f.pntr).c_str(), f.relocs.size(), hex(f.package).c_str(), hex((int64_t)(f.data.size() - f.package)).c_str(), f.describe(f.sceneRoot).c_str());
    std::map<const Chunk*, int> relocsIn;
    for (uint32_t r : f.relocs) relocsIn[f.chunkContaining(r)]++;
    std::printf("\n  %-6s %10s %10s %8s\n", "chunk", "offset", "size", "relocs");
    for (auto& c : f.chunks) {
        std::string name = c.tag.empty() ? "(gap)" : c.tag;
        if (c.depth) name = "  " + name;
        std::printf("  %-6s %10s %10s %8d\n", name.c_str(), hex(c.offset).c_str(), hex(c.size).c_str(), relocsIn[&c]);
    }
    return 0;
}

static int cmdVerify(const Args& a) {
    int rc = 0;
    for (auto& path : a) {
        Nu20File f = Nu20File::load(path);
        std::printf("%s\n", path.c_str());
        Report r = verify(f);
        printReport(r);
        if (!r.ok()) rc = 1;
    }
    return rc;
}

static int cmdTextures(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    auto tex = f.textures();
    std::printf("%zu textures\n", tex.size());
    for (size_t i = 0; i < tex.size(); i++) {
        std::string data = tex[i].data ? f.describe(tex[i].data) : "-";
        std::printf("  %3zu  desc %-14s %5ux%-5u size %8s  data %s%s\n", i, f.describe(tex[i].descriptor).c_str(), tex[i].width, tex[i].height, hex(tex[i].size).c_str(), data.c_str(), isCubemap(f, (int32_t)i) ? "  (cubemap)" : "");
    }
    Package pk = f.parsePackage();
    if (!pk.parsed) {
        std::printf("package: couldnt read it (%s)\n", pk.error.c_str());
        return 0;
    }
    if (pk.textureFirst)
        std::printf("package (texture-first): size word %s, NU20 block at %s\n", hex(f.u32(0)).c_str(), hex(f.base).c_str());
    else
        std::printf("package: size word %s (file says rest = %s)\n", hex(f.u32(pk.sizeWord)).c_str(), hex((int64_t)f.data.size() - pk.sizeWord - 4).c_str());
    for (auto& b : pk.vertexBuffers) std::printf("  vertex buffer @%s size %s\n", f.describe(b.sizeField).c_str(), hex(b.size).c_str());
    for (auto& b : pk.indexBuffers) std::printf("  index buffer  @%s size %s\n", f.describe(b.sizeField).c_str(), hex(b.size).c_str());
    uint32_t until = pk.textureFirst ? f.base : (uint32_t)f.data.size();
    std::printf("  buffers end %s, %s %s bytes\n", f.describe(pk.end).c_str(), pk.textureFirst ? "padding" : "trailer", hex((int64_t)until - pk.end).c_str());
    return 0;
}

static int cmdMaterials(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    auto mats = materials(f);
    std::printf("%zu materials\n", mats.size());
    for (size_t i = 0; i < mats.size(); i++) {
        std::string differs;
        if (mats[i].texture16 != mats[i].texture32) differs = " (+0xB8 says " + std::to_string(mats[i].texture32) + ")";
        std::printf("  %3zu  %-14s texture %d%s\n", i, f.describe(mats[i].record).c_str(), mats[i].texture16, differs.c_str());
    }
    return 0;
}

static int cmdMaterial(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    auto mats = materials(f);
    uint32_t n = num(a[1]);
    if (n >= mats.size()) throw Error("no material " + a[1]);
    const uint8_t* rec = &f.data[mats[n].record];
    std::vector<uint8_t> def = buildMaterialRecord({});
    std::printf("material %u at %s\n", n, f.describe(mats[n].record).c_str());
    for (auto& fld : materialFields()) {
        std::string v = formatMaterialField(rec, fld);
        bool changed = v != formatMaterialField(def.data(), fld);
        std::printf("  +0x%03X  %-22s %-12s%s%s\n", fld.off, fld.name, v.c_str(), fld.shading ? "  (shading)" : "", changed && !fld.shading ? "  *" : "");
    }
    std::printf("(* = not the default)\n");
    return 0;
}

static int addTextureFile(const Args& a, bool cube) {
    Nu20File f = Nu20File::load(a[0]);
    Nu20File orig = f;
    std::string how;
    std::vector<uint8_t> dds = cube ? cubeTextureBytes(a[2], &how) : textureBytes(a[2], &how);
    bool isCube = parseDds(dds).cube;
    if (cube && !isCube) throw Error(a[2] + " isnt a cubemap dds");
    uint32_t idx = addTexture(f, dds);
    if (!how.empty()) std::printf("%s\n", how.c_str());
    if (isCube) std::printf("added cubemap %u (takes texture numbers %u-%u)\n", idx, idx, idx + 5);
    else std::printf("added texture %u\n", idx);
    return saveEdit(orig, f, a[1]);
}

static int cmdAddTexture(const Args& a) { return addTextureFile(a, false); }
static int cmdAddCubemap(const Args& a) { return addTextureFile(a, true); }

static Args expandList(const Args& args);

static const std::map<std::string, size_t> SCRIPT_FRAMES = {
    {"coin", 32}, {"coin_pause", 32}, {"roof_flight_field", 32}, {"WaterCaustics", 30}, {"mothshadow", 29},
    {"computer", 8}, {"cctv", 5}, {"cctv2", 5}, {"heli_elec", 3}, {"whitenoise", 2},
};

static int cmdAnimations(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    auto anims = textureAnimations(f);
    std::printf("%zu animations\n", anims.size());
    for (size_t i = 0; i < anims.size(); i++) {
        std::string frames;
        for (uint16_t t : anims[i].frames) frames += " " + std::to_string(t);
        std::printf("  %3zu  material %-4u %-18s %zu frames:%s\n", i, anims[i].material, anims[i].script.empty() ? "(no script)" : anims[i].script.c_str(), anims[i].frames.size(), frames.c_str());
    }
    return 0;
}

static bool isDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

static int cmdAddAnimation(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    Nu20File orig = f;
    uint32_t material = num(a[2]);
    std::string script = a[3];
    bool custom = false;
    Args tokens;
    for (size_t i = 4; i < a.size(); i++) {
        if (a[i] == "--custom-script") custom = true;
        else tokens.push_back(a[i]);
    }
    std::map<std::string, uint32_t> added;
    std::vector<uint16_t> frames;
    std::string log;
    for (auto& token : expandList(tokens)) {
        size_t dash = token.find('-');
        if (isDigits(token)) {
            frames.push_back((uint16_t)num(token));
        } else if (dash != std::string::npos && isDigits(token.substr(0, dash)) && isDigits(token.substr(dash + 1))) {
            uint32_t from = num(token.substr(0, dash)), to = num(token.substr(dash + 1));
            if (to < from) throw Error("backwards frame range " + token);
            for (uint32_t t = from; t <= to; t++) frames.push_back((uint16_t)t);
        } else {
            auto it = added.find(token);
            if (it == added.end()) {
                std::string how;
                it = added.emplace(token, addTexture(f, textureBytes(token, &how))).first;
                log += "  texture       " + std::to_string(it->second) + " from " + token + "\n";
            }
            frames.push_back((uint16_t)it->second);
        }
    }
    auto known = SCRIPT_FRAMES.find(script);
    if (known == SCRIPT_FRAMES.end() && !custom) {
        std::string names;
        for (auto& s : SCRIPT_FRAMES) names += " " + s.first;
        throw Error("no game script called " + script + " (scripts:" + names + "). made ur own .ATS? use --custom-script");
    }
    if (known != SCRIPT_FRAMES.end() && frames.size() != known->second)
        throw Error("script " + script + " plays " + std::to_string(known->second) + " frames, got " + std::to_string(frames.size()));

    std::string report;
    uint32_t idx = addAnimation(f, material, script, frames, &report);
    std::printf("added animation %u on material %u\n%s%s", idx, material, log.c_str(), report.c_str());
    return saveEdit(orig, f, a[1]);
}

static int cmdConvertTexture(const Args& a) {
    uint32_t maxSize = a.size() > 2 ? (uint32_t)std::stoul(a[2]) : 2048;
    std::string how;
    auto dds = encodeDds(loadImage(a[0]), maxSize, &how);
    std::ofstream out(a[1], std::ios::binary);
    out.write((const char*)dds.data(), (std::streamsize)dds.size());
    std::printf("%s -> %s: %s\n", a[0].c_str(), a[1].c_str(), how.c_str());
    return 0;
}

static int cmdSetTexture(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    Nu20File orig = f;
    int32_t tex = std::stoi(a[3], nullptr, 0);
    int n = 0;
    if (a[2] == "all") {
        auto mats = materials(f);
        for (uint32_t i = 0; i < mats.size(); i++) {
            if (mats[i].texture16 < 0) continue;
            setMaterialTexture(f, i, tex);
            n++;
        }
    } else {
        setMaterialTexture(f, num(a[2]), tex);
        n = 1;
    }
    std::printf("set %d material(s) to texture %d\n", n, tex);
    return saveEdit(orig, f, a[1]);
}

static int cmdNewMaterial(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    Nu20File orig = f;
    auto mats = materials(f);
    const ShadingPreset* preset = findShadingPreset(a[2]);
    std::vector<std::pair<std::string, std::string>> values;
    int64_t groupOf = -1;
    std::string shadingName;
    if (preset) {
        values = shadingValues(*preset);
        shadingName = std::string("shading preset ") + preset->name;
    } else {
        uint32_t shadingOf = 0;
        if (!isNumber(a[2], shadingOf)) {
            std::string names;
            for (auto& p : shadingPresets()) names += std::string(" ") + p.name;
            throw Error("'" + a[2] + "' isnt a material number or a preset (presets:" + names + ")");
        }
        if (shadingOf >= mats.size()) throw Error("no material " + a[2]);
        const uint8_t* src = &f.data[mats[shadingOf].record];
        for (auto& fld : materialFields())
            if (fld.shading) values.push_back({fld.name, formatMaterialField(src, fld)});
        groupOf = shadingOf;
        shadingName = "shading of material " + a[2];
    }

    bool lastGroup = false;
    int64_t animFrom = -1;
    for (size_t i = 3; i < a.size(); i++) {
        if (a[i] == "--last-group") {
            lastGroup = true;
        } else if (a[i] == "--group-of" && i + 1 < a.size()) {
            groupOf = num64(a[++i]);
            if (groupOf < 0 || groupOf >= (int64_t)mats.size()) throw Error("no material " + a[i]);
        } else if (a[i] == "--animation-of" && i + 1 < a.size()) {
            animFrom = num64(a[++i]);
            if (animFrom < 0 || animFrom >= (int64_t)mats.size()) throw Error("no material " + a[i]);
        } else {
            size_t eq = a[i].find('=');
            if (eq == std::string::npos) throw Error("expected name=value, got " + a[i]);
            values.push_back({a[i].substr(0, eq), a[i].substr(eq + 1)});
        }
    }

    std::vector<uint8_t> rec = buildMaterialRecord(values);
    auto problems = shadingProblems(rec.data());
    for (auto& p : envmapProblems(f, rec.data())) problems.push_back(p);
    if (!problems.empty()) {
        std::string all;
        for (auto& p : problems) all += "\n  " + p;
        throw Error("those shading settings dont go together (no game mat has them):" + all); // this is for being on the safe side, im also too fucking lazy as shi to try this
    }
    if (groupOf < 0) groupOf = mostUsedOpaqueMaterial(f);
    if (groupOf < 0) throw Error("no material to put the new one next to, use --group-of N");

    std::string report;
    uint32_t idx = createMaterial(f, (uint32_t)groupOf, &report, !lastGroup, rec);
    if (animFrom >= 0) {
        uint32_t from = (uint32_t)animFrom;
        if (animFrom >= idx) from++;
        std::string animLog;
        if (!addTextureAnimations(f, from, idx, &animLog))
            throw Error("material " + std::to_string(animFrom) + " has no texture animation");
        report += animLog;
    }
    std::printf("created material %u (built record, %s)\n%s", idx, shadingName.c_str(), report.c_str());
    return saveNewMaterial(orig, f, a[1]);
}

static Args expandList(const Args& args) {
    Args paths;
    for (auto& arg : args) {
        if (arg.size() < 2 || arg[0] != '@') {
            paths.push_back(arg);
            continue;
        }
        std::ifstream in(arg.substr(1));
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) paths.push_back(line);
        }
    }
    return paths;
}

static int cmdMeshes(const Args& a) {
    Args paths = expandList(a);
    int bad = 0;
    for (auto& path : paths) {
        Nu20File f;
        std::vector<MeshRecord> meshes;
        try {
            f = Nu20File::load(path);
            meshes = meshRecords(f);
        } catch (const Error& e) {
            std::printf("%s: skipped: %s\n", path.c_str(), e.what());
            continue;
        }
        std::vector<std::string> errors;
        checkMeshes(f, errors);
        if (paths.size() == 1) {
            std::printf("%zu meshes\n", meshes.size());
            for (size_t i = 0; i < meshes.size(); i++) {
                auto& m = meshes[i];
                std::printf("  %4zu  %-14s type %u  stride %2u  vb %u  vertices %u+%u  ib %u  indices %u+%u%s\n", i, f.describe(m.record).c_str(), m.type, m.stride, m.vertexBuffer, m.baseVertex, m.vertexCount, m.indexBuffer, m.firstIndex, m.primitives + 2, m.dynamic ? "  (dynamic)" : "");
            }
        }
        for (auto& e : errors) std::printf("%s: ERROR: %s\n", path.c_str(), e.c_str());
        if (!errors.empty()) bad++;
    }
    if (paths.size() > 1) std::printf("%zu files, %d with mesh errors\n", paths.size(), bad);
    return bad ? 1 : 0;
}

static int cmdBones(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    auto bones = characterBones(f);
    if (bones.empty()) throw Error("no skeleton, not a GHG character");
    std::printf("%zu bones (bind pose position, character space)\n", bones.size());
    for (size_t i = 0; i < bones.size(); i++) {
        auto& b = bones[i];
        std::printf("  %3zu  %-24s parent %3d  %8.4f %8.4f %8.4f\n", i, b.name.c_str(), b.parent, b.position[0], b.position[1], b.position[2]);
    }
    std::printf("skinned game models (what all picks):");
    for (uint32_t m : skinnedModels(f)) std::printf(" %u", m);
    std::printf("\n");
    auto onBone = modelBones(f);
    for (size_t b = 0; b < bones.size(); b++) {
        std::string list;
        for (uint32_t m = 0; m < onBone.size(); m++)
            if (onBone[m] == (int)b) list += " " + std::to_string(m);
        if (!list.empty()) std::printf("rigid game models on bone %zu (%s):%s\n", b, bones[b].name.c_str(), list.c_str());
    }
    return 0;
}

// "all" = every model that draws a mesh
static std::vector<uint32_t> parseModels(const Nu20File& f, const std::string& spec, int bone) {
    std::vector<uint32_t> models;
    if (spec != "all") {
        std::stringstream ss(spec);
        std::string n;
        while (std::getline(ss, n, ',')) models.push_back(num(n));
        return models;
    }
    if (f.isCharacterModel()) {
        models = skinnedModels(f);
        if (models.empty()) {
            auto onBone = modelBones(f);
            for (uint32_t m = 0; m < onBone.size(); m++)
                if (onBone[m] == bone) models.push_back(m);
        }
        if (models.empty()) throw Error("char has no skinned game models and nothing on bone " + std::to_string(bone) + " (check bones)");
        return models;
    }
    if (!f.findChunk("DISP")) throw Error("no DISP chunk");
    DispLayout L = readDisp(f, 0);
    uint32_t count = f.u32(L.payload + 0x10);
    uint32_t first = f.ptr(L.payload + 0x14);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t m = first + i * 12;
        if (!f.u32(m)) continue;
        uint32_t cmds = f.ptr(m + 8);
        if (f.data[L.commandAt(f.u32(cmds))] == GEOMCALL) models.push_back(i);
    }
    return models;
}

struct Placement {
    int bone = -1;
    bool atBone = false;
};

static bool placeOption(const Args& a, size_t& i, LoadOptions& opt, Placement& pl) {
    if (a[i] == "--scale" && i + 1 < a.size()) {
        opt.scale = std::stof(a[++i]);
    } else if (a[i] == "--bone" && i + 1 < a.size()) {
        pl.bone = std::stoi(a[++i], nullptr, 0);
        if (pl.bone < 0 || pl.bone > 127) throw Error("--bone must be 0..127");
    } else if (a[i] == "--at-bone") {
        pl.atBone = true;
    } else if (a[i] == "--offset" && i + 3 < a.size()) {
        for (int k = 0; k < 3; k++) opt.offset[k] = std::stof(a[++i]);
    } else if (a[i] == "--mirror-x") {
        opt.mirrorX = true;
    } else if (a[i] == "--flip-winding") {
        opt.flipWinding = true;
    } else {
        return false;
    }
    return true;
}

static void placeAtBone(const Nu20File& f, LoadOptions& opt, Placement& pl) {
    if (pl.bone < 0) {
        pl.bone = 0;
        if (f.isCharacterModel() && skinnedModels(f).empty()) {
            int first = 128;
            for (int b : modelBones(f))
                if (b >= 0) first = std::min(first, b);
            if (first < 128) pl.bone = first;
        }
    }
    if (!pl.atBone) return;
    auto bones = characterBones(f);
    if (pl.bone >= (int)bones.size()) throw Error("--at-bone: no bone " + std::to_string(pl.bone));
    for (int k = 0; k < 3; k++) opt.offset[k] += bones[pl.bone].position[k];
}

static Model loadFor(const Nu20File& f, const std::string& path, const LoadOptions& opt, std::string* log) {
    auto bones = characterBones(f);
    return loadModel(path, bones.empty() ? nullptr : &bones, opt, log);
}

static MeshGeometry mergeParts(const Model& model, const std::string& path) {
    MeshGeometry g;
    for (auto& p : model.parts) {
        uint32_t base = (uint32_t)g.vertexCount();
        g.positions.insert(g.positions.end(), p.positions.begin(), p.positions.end());
        g.normals.insert(g.normals.end(), p.normals.begin(), p.normals.end());
        g.uvs.insert(g.uvs.end(), p.uvs.begin(), p.uvs.end());
        for (uint32_t t : p.triangles) g.triangles.push_back(base + t);
        if (g.skinned() && !p.skinned()) throw Error("some meshes in " + path + " are skinned and some arent, add-mesh needs all one or the other");
        if (p.skinned()) {
            g.skinBones.resize(base * 3, -1);
            g.skinWeights.resize(base * 3, 0);
            g.skinBones.insert(g.skinBones.end(), p.skinBones.begin(), p.skinBones.end());
            g.skinWeights.insert(g.skinWeights.end(), p.skinWeights.begin(), p.skinWeights.end());
        }
    }
    if (g.vertexCount() > 0xFFFF) throw Error(path + " has more than 65535 verts total (16 bit indices)");
    return g;
}

static bool takeLayerOption(Args& a, size_t from, uint32_t& layer) {
    for (size_t i = from; i < a.size(); i++) {
        if (a[i] != "--layer") continue;
        if (i + 1 >= a.size() || !isNumber(a[i + 1], layer)) throw Error("--layer needs a layer number");
        a.erase(a.begin() + i, a.begin() + i + 2);
        return true;
    }
    return false;
}

static std::vector<uint32_t> layerModels(const Nu20File& f, uint32_t layer, Placement& pl) {
    auto layers = characterLayers(f);
    if (layers.empty()) throw Error("--layer is only for chars");
    if (layer >= layers.size()) throw Error("no layer " + std::to_string(layer) + " (this file has " + std::to_string(layers.size()) + ")");
    const LayerInfo& info = layers[layer];
    std::string who = "layer " + std::to_string(layer);
    if (info.skinned.empty() && info.rigid.empty()) throw Error(who + " has nothing in it to add to, use add-layer");

    std::vector<uint32_t> models;
    auto skinned = skinnedModels(f);
    for (uint32_t m : info.skinned)
        if (std::find(skinned.begin(), skinned.end(), m) != skinned.end()) models.push_back(m);
    if (!models.empty()) return models;
    if (info.rigid.empty()) throw Error(who + " only has a morphing face model, cant add to that");

    int bone = pl.bone >= 0 ? pl.bone : info.rigid[0].second;
    for (auto& r : info.rigid) {
        if (r.second == bone) models.push_back(r.first);
        else if (pl.bone < 0) throw Error(who + " has models on more than one bone, pick one with --bone (check layers)");
    }
    if (models.empty()) throw Error(who + " has nothing on bone " + std::to_string(bone) + " (check layers)");
    pl.bone = bone;
    return models;
}

static int cmdAddMesh(const Args& args) {
    Args a = args;
    uint32_t layer = 0;
    bool byLayer = takeLayerOption(a, 3, layer);
    if (a.size() < 4) throw Error("add-mesh needs a material");
    Nu20File f = Nu20File::load(a[0]);
    Nu20File orig = f;
    LoadOptions opt;
    Placement pl;
    for (size_t i = byLayer ? 4 : 5; i < a.size(); i++)
        if (!placeOption(a, i, opt, pl)) throw Error("unknown add-mesh option " + a[i]);
    std::vector<uint32_t> targets;
    if (byLayer) targets = layerModels(f, layer, pl);
    placeAtBone(f, opt, pl);

    std::string loadLog;
    MeshGeometry g = mergeParts(loadFor(f, a[2], opt, &loadLog), a[2]);
    uint32_t material = num(a[3]);
    std::vector<uint32_t> models = byLayer ? targets : parseModels(f, a[4], pl.bone);

    float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::string report;
    uint32_t idx = addMesh(f, g, material, models, matrix, &report, (uint8_t)pl.bone);
    std::printf("added mesh %u (material %u) to %zu game model(s)\n%s%s", idx, material, models.size(), loadLog.c_str(), report.c_str());
    return saveMeshEdit(orig, f, a[1]);
}

static int cmdLayers(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    if (!f.isCharacterModel()) throw Error("no layers, not a GHG char");
    auto layers = characterLayers(f);
    std::printf("%zu layers\n", layers.size());
    for (size_t i = 0; i < layers.size(); i++) {
        std::string models;
        for (uint32_t m : layers[i].skinned) models += " " + std::to_string(m);
        for (auto& r : layers[i].rigid) models += " " + std::to_string(r.first) + "(bone " + std::to_string(r.second) + ")";
        std::printf("  %3zu  %-24s%s\n", i, layers[i].name.c_str(), models.empty() ? "  (nothing)" : models.c_str());
    }
    return 0;
}

static int cmdAddLayer(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    Nu20File orig = f;
    LoadOptions opt;
    ImportOptions io;
    bool reflectGiven = false;
    Placement pl;
    for (size_t i = 4; i < a.size(); i++) {
        if (placeOption(a, i, opt, pl)) continue;
        if (a[i] == "--presets") io.presets = true;
        else if (a[i] == "--shading-of" && i + 1 < a.size()) io.texturedShading = num64(a[++i]);
        else if (a[i] == "--plain-shading-of" && i + 1 < a.size()) io.plainShading = num64(a[++i]);
        else if (a[i] == "--envmap" && i + 1 < a.size()) io.envMap = (int32_t)num64(a[++i]);
        else if (a[i] == "--reflect" && i + 1 < a.size()) { io.reflect = std::stof(a[++i]); reflectGiven = true; }
        else throw Error("unknown add-layer option " + a[i]);
    }
    if (reflectGiven && io.envMap < 0) throw Error("--reflect needs --envmap");
    placeAtBone(f, opt, pl);
    io.bone = (uint8_t)pl.bone;

    std::string loadLog;
    Model m = loadFor(f, a[3], opt, &loadLog);
    std::string report;
    uint32_t idx = addLayer(f, a[2], m, io, &report);
    std::printf("added layer %u '%s' with %zu part(s)\n%s%s", idx, a[2].c_str(), m.parts.size(), loadLog.c_str(), report.c_str());
    return saveMeshEdit(orig, f, a[1]);
}

static int cmdImportModel(const Args& args) {
    Args a = args;
    uint32_t layer = 0;
    bool byLayer = takeLayerOption(a, 3, layer);
    Nu20File f = Nu20File::load(a[0]);
    Nu20File orig = f;
    LoadOptions opt;
    ImportOptions io;
    bool reflectGiven = false;
    Placement pl;
    for (size_t i = byLayer ? 3 : 4; i < a.size(); i++) {
        if (placeOption(a, i, opt, pl)) continue;
        if (a[i] == "--replace") io.replace = true;
        else if (a[i] == "--presets") io.presets = true;
        else if (a[i] == "--shading-of" && i + 1 < a.size()) io.texturedShading = num64(a[++i]);
        else if (a[i] == "--plain-shading-of" && i + 1 < a.size()) io.plainShading = num64(a[++i]);
        else if (a[i] == "--envmap" && i + 1 < a.size()) io.envMap = (int32_t)num64(a[++i]);
        else if (a[i] == "--reflect" && i + 1 < a.size()) { io.reflect = std::stof(a[++i]); reflectGiven = true; }
        else throw Error("unknown import-model option " + a[i]);
    }
    if (reflectGiven && io.envMap < 0) throw Error("--reflect needs --envmap");
    std::vector<uint32_t> targets;
    if (byLayer) targets = layerModels(f, layer, pl);
    placeAtBone(f, opt, pl);
    io.bone = (uint8_t)pl.bone;

    std::string loadLog;
    Model m = loadFor(f, a[2], opt, &loadLog);
    std::vector<uint32_t> models = byLayer ? targets : parseModels(f, a[3], pl.bone);

    std::string report;
    importModel(f, m, models, io, &report);
    std::printf("imported %zu part(s) into %zu game model(s)\n%s%s", m.parts.size(), models.size(), loadLog.c_str(), report.c_str());

    std::vector<uint32_t> replaced;
    if (io.replace) replaced = models;
    return saveMeshEdit(orig, f, a[1], replaced);
}

static int saveRemoved(const Nu20File& f, const std::string& out, const std::string& report) {
    std::printf("%s", report.c_str());
    return writeIfOk(f, out, verify(f));
}

static int cmdRemoveAnimation(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    std::string report;
    removeAnimations(f, num(a[2]), &report);
    return saveRemoved(f, a[1], report);
}

static int cmdRemoveLayer(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    std::string report;
    removeLayer(f, num(a[2]), &report);
    return saveRemoved(f, a[1], report);
}

static int cmdRemoveMesh(const Args& args) {
    Args a = args;
    uint32_t layer = 0;
    bool byLayer = takeLayerOption(a, 2, layer);
    Nu20File f = Nu20File::load(a[0]);
    std::vector<uint32_t> models;
    int64_t part = -1;
    if (byLayer) {
        Placement pl;
        auto layers = characterLayers(f);
        if (layer >= layers.size()) throw Error("no layer " + std::to_string(layer));
        for (uint32_t m : layers[layer].skinned) models.push_back(m);
        for (auto& r : layers[layer].rigid) models.push_back(r.first);
        if (models.empty()) throw Error("layer " + std::to_string(layer) + " has nothing in it");
    } else {
        if (a.size() < 3) throw Error("remove-mesh needs a game model number or --layer N");
        models.push_back(num(a[2]));
        if (a.size() > 3) part = num(a[3]);
    }
    std::string report;
    std::sort(models.begin(), models.end());
    models.erase(std::unique(models.begin(), models.end()), models.end());
    for (uint32_t m : models) {
        std::string one;
        removeModelParts(f, m, part, &one);
        report += one;
    }
    return saveRemoved(f, a[1], report);
}

static int cmdRemoveMaterial(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    std::string report;
    removeMaterial(f, num(a[2]), &report);
    return saveRemoved(f, a[1], report);
}

static int cmdRemoveTexture(const Args& a) {
    Nu20File f = Nu20File::load(a[0]);
    std::string report;
    removeTexture(f, num(a[2]), &report);
    return saveRemoved(f, a[1], report);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("no20 <command> <arguments> [options], see MANUAL.txt");
        return 2;
    }
    std::string cmd = argv[1];
    Args a(argv + 2, argv + argc);
    size_t n = a.size();
    try {
        if (cmd == "info" && n == 1) return cmdInfo(a);
        if (cmd == "verify" && n >= 1) return cmdVerify(a);
        if (cmd == "textures" && n == 1) return cmdTextures(a);
        if (cmd == "materials" && n == 1) return cmdMaterials(a);
        if (cmd == "material" && n == 2) return cmdMaterial(a);
        if (cmd == "meshes" && n >= 1) return cmdMeshes(a);
        if (cmd == "bones" && n == 1) return cmdBones(a);
        if (cmd == "add-texture" && n == 3) return cmdAddTexture(a);
        if (cmd == "add-cubemap" && n == 3) return cmdAddCubemap(a);
        if (cmd == "animations" && n == 1) return cmdAnimations(a);
        if (cmd == "add-animation" && n >= 5) return cmdAddAnimation(a);
        if (cmd == "set-texture" && n == 4) return cmdSetTexture(a);
        if (cmd == "convert-texture" && (n == 2 || n == 3)) return cmdConvertTexture(a);
        if (cmd == "new-material" && n >= 3) return cmdNewMaterial(a);
        if (cmd == "add-mesh" && n >= 5) return cmdAddMesh(a);
        if (cmd == "import-model" && n >= 4) return cmdImportModel(a);
        if (cmd == "add-layer" && n >= 4) return cmdAddLayer(a);
        if (cmd == "layers" && n == 1) return cmdLayers(a);
        if (cmd == "remove-animation" && n == 3) return cmdRemoveAnimation(a);
        if (cmd == "remove-layer" && n == 3) return cmdRemoveLayer(a);
        if (cmd == "remove-mesh" && n >= 3) return cmdRemoveMesh(a);
        if (cmd == "remove-material" && n == 3) return cmdRemoveMaterial(a);
        if (cmd == "remove-texture" && n == 3) return cmdRemoveTexture(a);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::printf("unknown command or wrong arguments: %s (see MANUAL.txt)\n", cmd.c_str());
    return 2;
}