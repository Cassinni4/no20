#include "mesh.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include "content.h"
#include "display.h"
#include "edit.h"
#include "image.h"

namespace no20 {

namespace {

constexpr uint32_t kMeshRecord = 0x38, kMatrix = 0x40, kCmd = 16;
constexpr uint8_t kMtxLoad = 0x83, kGeomCall = 0x82, kEnd = 0x8E;

void put16(Nu20File& f, uint32_t pos, uint16_t v) { std::memcpy(&f.data[pos], &v, 2); }

void putf(std::vector<uint8_t>& out, size_t at, float v) { std::memcpy(&out[at], &v, 4); }

uint16_t toHalf(float v) {
    uint32_t x;
    std::memcpy(&x, &v, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exp = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFF;
    if (exp <= 0) return (uint16_t)sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7C00);
    return (uint16_t)(sign | (exp << 10) | (mant >> 13));
}

uint8_t packUnit(float v) { return (uint8_t)std::clamp((int)std::lround((v + 1.0f) * 127.5f), 0, 255); }

uint32_t meshHeader(const Nu20File& f) { return f.follow(f.gsnhPayload() + 0x1D0); }

void shiftAll(std::vector<uint32_t*> ps, uint32_t at, uint32_t n) {
    for (auto* p : ps)
        if (*p >= at) *p += n;
}

}

const VertexAttr* VertexLayout::find(const std::string& name) const {
    for (auto& a : attrs)
        if (a.name == name) return &a;
    return nullptr;
}

VertexLayout vertexLayout(uint32_t vf) {
    VertexLayout L;
    L.format = vf;
    uint32_t o = 0;
    auto add = [&](const std::string& name, uint32_t size, char kind) {
        L.attrs.push_back({name, o, size, kind});
        o += size;
    };
    add("position", 12, 'f');
    int normal = ((vf & 8) == 0 && (vf & 0x880000) == 0) ? (vf >> 2) & 1 : 2;
    int tangent = ((vf & 0x20) == 0 && (vf & 0x1000000) == 0) ? (vf >> 4) & 1 : 2;
    int bitangent = (vf & 0x2000080) == 0 ? (vf >> 6) & 1 : 2;
    if (normal) add("normal", normal == 1 ? 12 : 4, normal == 1 ? 'f' : 'p');
    if (tangent) add("tangent", tangent == 1 ? 12 : 4, tangent == 1 ? 'f' : 'p');
    if (bitangent) add("bitangent", bitangent == 1 ? 12 : 4, bitangent == 1 ? 'f' : 'p');
    if ((vf >> 8) & 1) add("colour", 4, 'p');
    if (vf & 0x600) add("colour2", 4, 'p');
    uint32_t uvs = (vf >> 11) & 7;
    bool half = (vf >> 27) & 1;
    for (uint32_t i = 0; i < std::min(uvs, 4u); i++) add("uv" + std::to_string(i), half ? 4 : 8, half ? 'h' : 'u');
    int transparency = (vf & 0x8000) ? 2 : (vf >> 14) & 1;
    if (transparency) add("transparency", transparency == 1 ? 8 : 4, transparency == 1 ? 'q' : 'p');
    int blend = (vf & 0x20000) ? 2 : (vf >> 16) & 1;
    if (blend) add("blend", blend == 1 ? 12 : 4, blend == 1 ? 'f' : 'p');
    if ((vf >> 26) & 1) {
        add("light_dir", 4, 'p');
        add("light_colour", 4, 'p');
    }
    L.stride = o;
    return L;
}

std::vector<MeshRecord> meshRecords(const Nu20File& f) {
    std::vector<MeshRecord> out;
    uint32_t h = meshHeader(f);
    if (!h) return out;
    uint32_t list = f.follow(h + 0x10), n = f.u32(h + 0x14);
    if (!list || n > 100000) return out;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = f.follow(list + i * 4);
        if (!r || r + kMeshRecord > f.pntr) throw Error("mesh " + std::to_string(i) + " is outside the scene");
        MeshRecord m{r, f.u32(r), f.u32(r + 4), f.u16(r + 8), f.u32(r + 0x14), f.u32(r + 0x18), f.u32(r + 0x1C), f.u32(r + 0x20), f.u32(r + 0x24), f.u32(r + 0x28)};
        std::memcpy(m.palette, &f.data[r + 0xA], 8);
        out.push_back(m);
    }
    return out;
}

std::vector<Bone> characterBones(const Nu20File& f) {
    std::vector<Bone> out;
    if (!f.isCharacterModel()) return out;
    uint32_t g = f.gsnh();
    int32_t n = f.s32(g + 0x164);
    uint32_t b = f.follow(g + 0x168);
    if (n <= 0 || n > 127 || !b || b + (uint64_t)n * (0x60 + 0x80) > f.pntr) return out;
    // row vectors, world = local * parent
    std::vector<std::array<float, 16>> world(n);
    for (int32_t i = 0; i < n; i++) {
        uint32_t r = b + i * 0x60;
        Bone bone;
        uint32_t name = f.follow(r + 0x4C);
        if (name) bone.name = (const char*)&f.data[name];
        bone.parent = (int8_t)f.data[r + 0x50];
        std::array<float, 16> local;
        std::memcpy(local.data(), &f.data[b + n * 0x60 + i * 64], 64);
        if (bone.parent >= 0 && bone.parent < i) {
            auto& p = world[bone.parent];
            for (int row = 0; row < 4; row++)
                for (int col = 0; col < 4; col++) {
                    float s = 0;
                    for (int k = 0; k < 4; k++) s += local[row * 4 + k] * p[k * 4 + col];
                    world[i][row * 4 + col] = s;
                }
        } else {
            world[i] = local;
        }
        for (int k = 0; k < 3; k++) bone.position[k] = world[i][12 + k];
        std::memcpy(bone.inverseBind, &f.data[b + n * (0x60 + 64) + i * 64], 64);
        out.push_back(bone);
    }
    return out;
}

std::vector<int> modelBones(const Nu20File& f) {
    std::vector<int> out;
    if (!f.isCharacterModel()) return out;
    const Chunk* c = f.findChunk("DISP");
    if (!c) return out;
    uint32_t pl = c->offset + 8;
    uint32_t nmod = f.u32(pl + 0x10), models = f.follow(pl + 0x14);
    out.assign(nmod, -1);
    uint32_t g = f.gsnh();
    int32_t nb = f.s32(g + 0x164), nl = f.s32(g + 0x18C);
    uint32_t layers = f.follow(g + 0x190);
    if (nb <= 0 || nl <= 0 || nl > 4096 || !layers || !models) return out;
    auto modelOf = [&](uint32_t object) -> int64_t { // obj +8 -> block, block +0xB0 -> model
        uint32_t block = f.follow(object + 8);
        uint32_t m = block ? f.follow(block + 0xB0) : 0;
        if (m < models || m >= models + nmod * 12 || (m - models) % 12) return -1;
        return (m - models) / 12;
    };
    for (int32_t i = 0; i < nl; i++) {
        uint32_t L = layers + i * 0x14;
        for (uint32_t arrField : {L + 4, L + 0xC}) {
            uint32_t arr = f.follow(arrField);
            for (int32_t b = 0; arr && b < nb; b++)
                if (uint32_t object = f.follow(arr + b * 4))
                    if (int64_t m = modelOf(object); m >= 0) out[m] = b;
        }
    }
    return out;
}

namespace {

void finishPart(MeshGeometry& g, bool anyNormal, const LoadOptions& opt, const std::string& what) {
    if (opt.mirrorX)
        for (size_t i = 0; i < g.vertexCount(); i++) {
            g.positions[i * 3] = -g.positions[i * 3];
            g.normals[i * 3] = -g.normals[i * 3];
        }
    if (opt.mirrorX != opt.flipWinding)
        for (size_t t = 0; t < g.triangles.size(); t += 3) std::swap(g.triangles[t + 1], g.triangles[t + 2]);
    if (!anyNormal) {
        std::fill(g.normals.begin(), g.normals.end(), 0.0f);
        for (size_t t = 0; t < g.triangles.size(); t += 3) {
            const float* a = &g.positions[g.triangles[t] * 3];
            const float* b = &g.positions[g.triangles[t + 1] * 3];
            const float* c = &g.positions[g.triangles[t + 2] * 3];
            float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            for (int k = 0; k < 3; k++)
                for (int j = 0; j < 3; j++) g.normals[g.triangles[t + k] * 3 + j] += n[j];
        }
    }
    for (size_t i = 0; i < g.vertexCount(); i++) {
        float* n = &g.normals[i * 3];
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0) for (int k = 0; k < 3; k++) n[k] /= len;
        else n[1] = 1;
    }
    if (g.vertexCount() > 0xFFFF) throw Error(what + " has more than 65k verts");
}

std::string folderOf(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? "" : path.substr(0, s + 1);
}

std::vector<uint8_t> readBinary(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cant open " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

struct BtReader {
    std::vector<uint8_t> d;
    std::string path;
    size_t at = 0;

    void need(size_t n) const {
        if (at + n > d.size()) throw Error(path + ": file ends early");
    }
    std::string str() {
        size_t end = at;
        while (end < d.size() && d[end]) end++;
        if (end >= d.size()) throw Error(path + ": file ends early");
        std::string s((const char*)&d[at], end - at);
        at = end + 1;
        return s;
    }
    void tag(std::initializer_list<const char*> want) {
        std::string s = str();
        for (const char* w : want)
            if (s == w) return;
        throw Error(path + ": expected '" + *want.begin() + "', found '" + s + "'");
    }
    uint32_t u32() { need(4); uint32_t v; std::memcpy(&v, &d[at], 4); at += 4; return v; }
    int32_t s32() { return (int32_t)u32(); }
    float f32() { need(4); float v; std::memcpy(&v, &d[at], 4); at += 4; return v; }
    // "PCGHG" f32 version (0.4 or 0.5)
    void header(const char* magic) {
        tag({magic});
        std::string engine = str();
        float version = f32();
        if (engine != "PCGHG" || (version != 0.4f && version != 0.5f))
            throw Error(path + ": cant read this " + magic + " (" + engine + " v" + std::to_string(version) + "), only PCGHG 0.4/0.5 works");
    }
};

int boneByName(const std::vector<Bone>& bones, const std::string& name) {
    int found = -1;
    for (size_t j = 0; j < bones.size(); j++)
        if (_stricmp(bones[j].name.c_str(), name.c_str()) == 0) found = (int)j;
    return found;
}

MeshGeometry readBtMesh(BtReader& r, const std::vector<std::string>& names, const std::vector<Bone>* bones, bool& hasNormals, std::string& log) {
    const std::string& path = r.path;
    r.tag({"Mesh"});
    uint32_t nv = r.u32(), strip = r.u32() + 2;
    r.need(8);
    int8_t linked[8];
    std::memcpy(linked, &r.d[r.at], 8);
    r.at += 8;
    r.tag({"MeshAttributes"});
    std::vector<std::string> attrs(r.u32());
    for (auto& a : attrs) a = r.str();
    r.tag({"Vertices"});

    MeshGeometry g;
    g.positions.resize(nv * 3);
    g.normals.assign(nv * 3, 0.0f);
    g.uvs.assign(nv * 2, 0.0f);
    hasNormals = false;
    std::vector<int8_t> slots;
    std::vector<uint8_t> weights;
    static const std::map<std::string, size_t> sizes = {
        {"Position", 12}, {"Normal", 4}, {"ColourSet1", 4}, {"ColourSet2", 4}, {"UVSet1", 8},
        {"UVSet2", 8},    {"Tangents", 4}, {"BlendIndices", 4}, {"BlendWeights", 4}};
    for (size_t k = 0; k < attrs.size(); k++) {
        r.tag({attrs[k].c_str()});
        auto it = sizes.find(attrs[k]);
        if (it == sizes.end()) throw Error(path + ": unknown attribute " + attrs[k]);
        r.need(it->second * nv);
        const uint8_t* p = &r.d[r.at];
        for (uint32_t i = 0; i < nv; i++, p += it->second) {
            if (attrs[k] == "Position") {
                std::memcpy(&g.positions[i * 3], p, 12);
            } else if (attrs[k] == "Normal") {
                for (int c = 0; c < 3; c++) g.normals[i * 3 + c] = p[c] / 127.5f - 1.0f;
                hasNormals = true;
            } else if (attrs[k] == "UVSet1") {
                float uv[2];
                std::memcpy(uv, p, 8);
                g.uvs[i * 2] = uv[0], g.uvs[i * 2 + 1] = 1.0f - uv[1];
            } else if (attrs[k] == "BlendIndices") {
                slots.insert(slots.end(), (const int8_t*)p, (const int8_t*)p + 3);
            } else if (attrs[k] == "BlendWeights") {
                weights.insert(weights.end(), p, p + 3);
            }
        }
        r.at += it->second * nv;
    }
    r.tag({"Triangles", "Trianges"}); // not my fault the addon does ts
    r.need(strip * 2);
    std::vector<uint16_t> s(strip);
    std::memcpy(s.data(), &r.d[r.at], strip * 2);
    r.at += strip * 2;
    for (uint32_t k = 0; k + 2 < strip; k++) {
        uint32_t a = s[k], b = s[k + 1], c = s[k + 2];
        if (a == b || b == c || a == c) continue;
        if (a >= nv || b >= nv || c >= nv) throw Error(path + ": strip index past the vertex count");
        if (k & 1) std::swap(b, c);
        g.triangles.insert(g.triangles.end(), {a, b, c});
    }
    if (r.at < r.d.size()) {
        size_t save = r.at;
        if (std::string t = r.str(); t == "DynamicBuffers" || t == "Dynamic Buffers") {
            uint32_t n = r.u32();
            r.need((size_t)n * nv * 12);
            r.at += (size_t)n * nv * 12;
            if (n) log += "  note: " + std::to_string(n) + " face shape(s) skipped\n";
        } else {
            r.at = save;
        }
    }

    if (!slots.empty() && !weights.empty()) {
        std::map<int, int> boneOf;
        for (int8_t b : linked) {
            if (b < 0 || boneOf.count(b)) continue;
            int game = b;
            if (bones && b < (int)names.size()) {
                game = boneByName(*bones, names[b]);
                if (game < 0) throw Error(path + ": bone '" + names[b] + "' isnt in the chars skeleton");
            }
            if (bones && game >= (int)bones->size()) throw Error(path + ": bone " + std::to_string(game) + " isnt in the chars skeleton");
            boneOf[b] = game;
        }
        g.skinBones.assign(nv * 3, -1);
        g.skinWeights.assign(nv * 3, 0);
        size_t lost = 0;
        for (uint32_t i = 0; i < nv; i++) {
            int sum = 0;
            for (int k = 0; k < 3; k++) {
                int slot = slots[i * 3 + k], w = weights[i * 3 + k];
                if (slot < 0 || slot >= 8 || linked[slot] < 0) {
                    if (w) lost++;
                    continue;
                }
                g.skinBones[i * 3 + k] = (int16_t)boneOf[linked[slot]];
                g.skinWeights[i * 3 + k] = (uint8_t)w;
                sum += w;
            }
            if (!sum) throw Error(path + ": vertex " + std::to_string(i) + " has no bone weight");
            int total = 0; // gotta add up to 255, leftover goes on the first one
            for (int k = 0; k < 3; k++) total += g.skinWeights[i * 3 + k] = (uint8_t)(g.skinWeights[i * 3 + k] * 255 / sum);
            for (int k = 0; k < 3; k++)
                if (g.skinBones[i * 3 + k] >= 0) { g.skinWeights[i * 3 + k] += (uint8_t)(255 - total); break; }
        }
        log += "  skin          " + std::to_string(boneOf.size()) + " bone(s):";
        for (auto& [arm, game] : boneOf) log += " " + std::to_string(game) + (bones ? " " + (*bones)[game].name : "");
        log += "\n";
        if (lost)
            log += "  WARN: " + std::to_string(lost) + " weight(s) point at no bone in the palette (BactaTank only keeps 7 bones per mesh)\n";
    }
    return g;
}

void placePart(MeshGeometry& g, bool hasNormals, const LoadOptions& opt, const std::string& what) {
    for (size_t i = 0; i < g.positions.size(); i++) g.positions[i] = g.positions[i] * opt.scale + opt.offset[i % 3];
    finishPart(g, hasNormals, opt, what);
}

void appendPart(MeshGeometry& g, const MeshGeometry& p) {
    uint32_t base = (uint32_t)g.vertexCount();
    g.positions.insert(g.positions.end(), p.positions.begin(), p.positions.end());
    g.normals.insert(g.normals.end(), p.normals.begin(), p.normals.end());
    g.uvs.insert(g.uvs.end(), p.uvs.begin(), p.uvs.end());
    for (uint32_t t : p.triangles) g.triangles.push_back(base + t);
    g.skinBones.insert(g.skinBones.end(), p.skinBones.begin(), p.skinBones.end());
    g.skinWeights.insert(g.skinWeights.end(), p.skinWeights.begin(), p.skinWeights.end());
}

}

MeshGeometry loadBmesh(const std::string& path, const std::vector<Bone>* bones, const LoadOptions& opt, std::string* report) {
    BtReader r{readBinary(path), path};
    r.header("BactaTankMesh");
    r.tag({"Bones"});
    std::vector<std::string> names(r.u32());
    for (auto& n : names) n = r.str();
    bool hasNormals = false;
    std::string log;
    MeshGeometry g = readBtMesh(r, names, bones, hasNormals, log);
    if (g.triangles.empty()) throw Error(path + " has no triangles");
    placePart(g, hasNormals, opt, path);
    if (report) *report = log;
    return g;
}

Model loadBmodel(const std::string& path, const std::vector<Bone>* bones, const LoadOptions& opt, std::string* report) {
    BtReader r{readBinary(path), path};
    r.header("BactaTankModel");

    r.tag({"BactaTankArmature"});
    r.tag({"Bones"});
    int32_t nb = r.s32();
    if (nb < 0 || nb > 4096) throw Error(path + ": bad bone count " + std::to_string(nb));
    std::vector<std::string> names(nb);
    std::vector<std::array<float, 16>> world(nb);
    for (int32_t i = 0; i < nb; i++) {
        names[i] = r.str();
        r.s32();
        for (float& v : world[i]) v = r.f32();
    }


    std::string log;
    std::string dir = folderOf(path);
    r.tag({"BactaTankMaterials"});
    int32_t nmat = r.s32();
    if (nmat < 0 || nmat > 65535) throw Error(path + ": bad material count " + std::to_string(nmat));
    std::vector<ModelMaterial> mats(nmat);
    for (auto& mt : mats) {
        mt.name = r.str();
        float colour[4];
        for (float& v : colour) v = r.f32();
        r.need(0x28);
        r.at += 0x28;
        r.u32(); // vertex_format
        uint32_t renderState = r.u32();
        r.u32(); // shader_flags
        for (int c = 0; c < 3; c++) mt.kd[c] = colour[c];
        mt.hasKd = true;
        uint32_t blend = renderState & 0xF;
        mt.blend = blend == 1 ? ModelMaterial::Alpha : blend == 2 ? ModelMaterial::Additive : ((renderState >> 20) & 7) == 5 ? ModelMaterial::Cutout : ModelMaterial::Opaque;
        if (mt.blend == ModelMaterial::Alpha) mt.alpha = std::clamp(colour[3], 0.0f, 1.0f);
        mt.doubleSided = ((renderState >> 12) & 3) == 2;
        for (std::string* slot : {&mt.texture, &mt.normalMap}) {
            std::string t = r.str();
            if (t == "None") continue;
            if (!std::ifstream(dir + t, std::ios::binary).good()) {
                log += "  WARN: material '" + mt.name + "': " + t + " isnt next to the .bmodel, skipped\n";
                continue;
            }
            *slot = dir + t;
        }
    }

    r.tag({"BactaTankMeshes"});
    int32_t nm = r.s32();
    if (nm < 0) throw Error(path + ": bad mesh count " + std::to_string(nm));
    Model m;
    std::map<std::pair<int32_t, bool>, size_t> partOf; // (mat, skinned) -> part
    size_t droppedSkin = 0;
    for (int32_t k = 0; k < nm; k++) {
        std::string name = r.str();
        int32_t material = r.s32(), bone = r.s32();
        bool hasNormals = false;
        std::string meshLog;
        MeshGeometry g = readBtMesh(r, names, bones, hasNormals, meshLog);
        std::string what = path + " mesh '" + name + "'";
        if (material < 0 || material >= nmat)
            throw Error(what + " uses material " + std::to_string(material) + " of " + std::to_string(nmat));
        std::string kind = g.skinned() ? ", skinned" : "";
        if (!g.skinned() && bone >= 0) {
            if (bone >= nb) throw Error(what + " is on bone " + std::to_string(bone) + " of " + std::to_string(nb));
            const auto& M = world[bone];
            for (size_t i = 0; i < g.vertexCount(); i++) {
                float* p = &g.positions[i * 3];
                float* n = &g.normals[i * 3];
                float x = p[0], y = p[1], z = p[2], nx = n[0], ny = n[1], nz = n[2];
                for (int c = 0; c < 3; c++) {
                    p[c] = x * M[c] + y * M[4 + c] + z * M[8 + c] + M[12 + c];
                    n[c] = nx * M[c] + ny * M[4 + c] + nz * M[8 + c];
                }
            }
            if (bones) {
                int game = boneByName(*bones, names[bone]);
                if (game < 0) throw Error(what + " is on bone '" + names[bone] + "', which isnt in the chars skeleton");
                g.skinBones.assign(g.vertexCount() * 3, -1);
                g.skinWeights.assign(g.vertexCount() * 3, 0);
                for (size_t i = 0; i < g.vertexCount(); i++) g.skinBones[i * 3] = (int16_t)game, g.skinWeights[i * 3] = 255;
            }
            kind = ", on bone " + names[bone];
        }
        if (!bones && g.skinned()) {
            g.skinBones.clear();
            g.skinWeights.clear();
            droppedSkin++;
        }
        log += "  mesh in file  '" + name + "': " + std::to_string(g.vertexCount()) + " vertices, " +
               std::to_string(g.triangles.size() / 3) + " triangles, material '" + mats[material].name + "'" + kind + "\n" + meshLog;
        if (g.triangles.empty()) continue;
        placePart(g, hasNormals, opt, what);
        auto key = std::make_pair(material, g.skinned());
        auto it = partOf.find(key);
        if (it != partOf.end() && m.parts[it->second].vertexCount() + g.vertexCount() <= 0xFFFF) {
            appendPart(m.parts[it->second], g);
        } else {
            partOf[key] = m.parts.size();
            m.materials.push_back(mats[material]);
            m.parts.push_back(std::move(g));
        }
    }
    if (m.parts.empty()) throw Error(path + " has no triangles");
    if (droppedSkin) log += "  note: skin weights dropped, file has no skeleton\n";
    if (report) *report = log;
    return m;
}

Model loadModel(const std::string& path, const std::vector<Bone>* bones, const LoadOptions& opt, std::string* report) {
    std::string ext = path.substr(std::min(path.size(), path.find_last_of('.')));
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".bmodel") return loadBmodel(path, bones, opt, report);
    if (ext != ".bmesh") throw Error(path + ": only .bmesh and .bmodel work (export one from BactaTank Classic or its blender addon)");
    Model m;
    ModelMaterial mat;
    size_t slash = path.find_last_of("/\\");
    mat.name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    mat.name = mat.name.substr(0, mat.name.size() - 6);
    m.materials.push_back(mat);
    m.parts.push_back(loadBmesh(path, bones, opt, report));
    return m;
}

std::vector<uint16_t> triangleStrip(const std::vector<uint32_t>& tris) {
    std::vector<uint16_t> s;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        uint16_t a = (uint16_t)tris[t], b = (uint16_t)tris[t + 1], c = (uint16_t)tris[t + 2];
        if (!s.empty()) {
            if (s.size() % 2) s.push_back(s.back());// every tri starts on an even spot or the winding flips
            s.push_back(s.back());
            s.push_back(a);
        }
        s.insert(s.end(), {a, b, c});
    }
    return s;
}

namespace {

void computeTangents(const MeshGeometry& g, std::vector<float>& tan, std::vector<float>& bitan) {
    size_t n = g.vertexCount();
    std::vector<float> s(n * 3, 0.0f), t(n * 3, 0.0f);
    for (size_t k = 0; k + 2 < g.triangles.size(); k += 3) {
        uint32_t i[3] = {g.triangles[k], g.triangles[k + 1], g.triangles[k + 2]};
        const float* p0 = &g.positions[i[0] * 3];
        const float* p1 = &g.positions[i[1] * 3];
        const float* p2 = &g.positions[i[2] * 3];
        float u0 = g.uvs[i[0] * 2], v0 = 1 - g.uvs[i[0] * 2 + 1];
        float u1 = g.uvs[i[1] * 2] - u0, v1 = 1 - g.uvs[i[1] * 2 + 1] - v0;
        float u2 = g.uvs[i[2] * 2] - u0, v2 = 1 - g.uvs[i[2] * 2 + 1] - v0;
        float det = u1 * v2 - u2 * v1;
        if (std::fabs(det) < 1e-12f) continue;
        float r = 1.0f / det;
        for (int c = 0; c < 3; c++) {
            float e1 = p1[c] - p0[c], e2 = p2[c] - p0[c];
            float sd = (e1 * v2 - e2 * v1) * r, td = (e2 * u1 - e1 * u2) * r;
            for (int j = 0; j < 3; j++) s[i[j] * 3 + c] += sd, t[i[j] * 3 + c] += td;
        }
    }
    tan.assign(n * 3, 0.0f);
    bitan.assign(n * 3, 0.0f);
    for (size_t v = 0; v < n; v++) {
        const float* nm = &g.normals[v * 3];
        float* T = &tan[v * 3];
        float d = nm[0] * s[v * 3] + nm[1] * s[v * 3 + 1] + nm[2] * s[v * 3 + 2];
        for (int c = 0; c < 3; c++) T[c] = s[v * 3 + c] - nm[c] * d;
        float len = std::sqrt(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]);
        if (len < 1e-6f) {
            float a[3] = {std::fabs(nm[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(nm[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
            float d2 = nm[0] * a[0] + nm[1] * a[1];
            for (int c = 0; c < 3; c++) T[c] = a[c] - nm[c] * d2;
            len = std::sqrt(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]);
        }
        for (int c = 0; c < 3; c++) T[c] /= len;
        float B[3] = {nm[1] * T[2] - nm[2] * T[1], nm[2] * T[0] - nm[0] * T[2], nm[0] * T[1] - nm[1] * T[0]};
        float hand = B[0] * t[v * 3] + B[1] * t[v * 3 + 1] + B[2] * t[v * 3 + 2] < 0 ? -1.0f : 1.0f;
        for (int c = 0; c < 3; c++) bitan[v * 3 + c] = B[c] * hand;
    }
}

}

std::vector<uint8_t> encodeVertices(const MeshGeometry& g, const VertexLayout& L) {
    std::vector<float> tan, bitan;
    if (L.find("tangent") || L.find("bitangent")) computeTangents(g, tan, bitan);
    bool baked = (L.format >> 26) & 1;
    std::vector<uint8_t> out(g.vertexCount() * L.stride, 0);
    auto put4 = [&](size_t at, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        out[at] = a, out[at + 1] = b, out[at + 2] = c, out[at + 3] = d;
    };
    auto putVec = [&](const VertexAttr& a, size_t at, const float* v, uint8_t w) {
        if (a.kind == 'f') for (int k = 0; k < 3; k++) putf(out, at + 4 * k, v[k]);
        else put4(at, packUnit(v[0]), packUnit(v[1]), packUnit(v[2]), w);
    };
    for (size_t i = 0; i < g.vertexCount(); i++) {
        size_t base = i * L.stride;
        for (auto& a : L.attrs) {
            size_t at = base + a.offset;
            if (a.name == "position") {
                for (int k = 0; k < 3; k++) putf(out, at + 4 * k, g.positions[i * 3 + k]);
            } else if (a.name == "normal") {
                putVec(a, at, &g.normals[i * 3], 0);
            } else if (a.name == "tangent") {
                putVec(a, at, &tan[i * 3], 0xFF);
            } else if (a.name == "bitangent") {
                putVec(a, at, &bitan[i * 3], 0xFF);
            } else if (a.name == "colour") {
                if (baked) put4(at, 0xFF, 0xFF, 0xFF, 0x7F);
                else put4(at, 0x7F, 0x7F, 0x7F, 0x7F);
            } else if (a.name == "colour2" || a.name == "light_colour") {
                put4(at, 0, 0, 0, 0);
            } else if (a.name == "transparency") { // weights
                uint8_t w[3] = {0xFF, 0, 0};
                if (g.skinned()) std::memcpy(w, &g.skinWeights[i * 3], 3);
                if (a.kind == 'q') putf(out, at, w[0] / 255.0f), putf(out, at + 4, w[1] / 255.0f);
                else put4(at, w[0], w[1], w[2], 0xFF);
            } else if (a.name == "blend") { // palette slots
                uint8_t s[3] = {0, 0, 0};
                if (g.skinned())
                    for (int k = 0; k < 3; k++) s[k] = (uint8_t)std::max<int16_t>(0, g.skinBones[i * 3 + k]);
                if (a.kind == 'f') putf(out, at, s[0]), putf(out, at + 4, s[1]), putf(out, at + 8, s[2]);
                else put4(at, s[0], s[1], s[2], 0xFF);
            } else if (a.name == "light_dir") {
                put4(at, 0x80, 0x00, 0x80, 0xFF);
            } else {
                float u = g.uvs[i * 2], v = 1.0f - g.uvs[i * 2 + 1];
                if (a.kind == 'h') {
                    uint16_t h[2] = {toHalf(u), toHalf(v)};
                    std::memcpy(&out[at], h, 4);
                } else {
                    putf(out, at, u);
                    putf(out, at + 4, v);
                }
            }
        }
    }
    return out;
}

namespace {

struct Part { uint32_t material, command; };

std::vector<std::vector<Part>> modelParts(const Nu20File& f) {
    std::vector<std::vector<Part>> out;
    const Chunk* c = f.findChunk("DISP");
    if (!c) return out;
    uint32_t pl = c->offset + 8;
    uint32_t n = f.u32(pl + 0x10), models = f.follow(pl + 0x14);
    if (!n || !models) return out;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t m = models + i * 12, parts = f.u32(m);
        uint32_t ml = f.follow(m + 4), cl = f.follow(m + 8);
        std::vector<Part> ps;
        for (uint32_t j = 0; ml && cl && j < parts; j++) ps.push_back({f.u32(ml + j * 4), f.u32(cl + j * 4)});
        out.push_back(ps);
    }
    return out;
}

uint32_t partMesh(const Nu20File& f, const DispLayout& l, const Part& p) {
    uint32_t cmd = l.commandAt(p.command);
    return f.data[cmd] == kGeomCall ? f.follow(cmd + 4) : 0;
}

bool skinnedPart(const Nu20File& f, const DispLayout& l, const Part& p) {
    uint32_t rec = partMesh(f, l, p);
    return rec && (int8_t)f.data[rec + 0xA] >= 0 && !f.u32(rec + 0x28);
}

}

int64_t mostUsedOpaqueMaterial(const Nu20File& f) { return mostUsedMaterialWithBlend(f, 0); }

int64_t mostUsedMaterialWithBlend(const Nu20File& f, uint32_t blend) {
    auto mats = materials(f);
    std::map<uint32_t, uint32_t> uses;
    DispLayout l = readDisp(f, 0);
    for (auto& parts : modelParts(f))
        for (auto& p : parts)
            if (partMesh(f, l, p) && p.material < mats.size() && (f.u32(mats[p.material].record + 0x40) & 0xF) == blend)
                uses[p.material]++;
    int64_t best = -1;
    uint32_t n = 0;
    for (auto& [m, k] : uses)
        if (k > n) best = m, n = k;
    return best;
}

std::vector<uint32_t> skinnedModels(const Nu20File& f) {
    std::vector<uint32_t> out;
    if (!f.isCharacterModel()) return out;
    DispLayout l = readDisp(f, 0);
    auto all = modelParts(f);
    for (uint32_t i = 0; i < all.size(); i++) {
        bool ok = !all[i].empty();
        for (auto& p : all[i]) ok = ok && skinnedPart(f, l, p);
        if (ok) out.push_back(i);
    }
    return out;
}

void packageInsert(Nu20File& f, uint32_t at, const uint8_t* bytes, uint32_t n) {
    Package pk = f.parsePackage();
    if (!pk.parsed) throw Error("couldnt read the package: " + pk.error);
    f.data.insert(f.data.begin() + at, bytes, bytes + n);
    if (!pk.textureFirst) {
        f.put32(f.package, f.u32(f.package) + n);
        return;
    }
    uint32_t pad = (0x100 - n % 0x100) % 0x100;
    f.data.insert(f.data.begin() + pk.end + n, pad, 0);
    f.put32(0, f.u32(0) + n + pad);
    f.parse();
}

namespace {

uint32_t appendToBuffer(Nu20File& f, bool vertex, uint32_t idx, const uint8_t* bytes, uint32_t n) {
    Package pk = f.parsePackage();
    if (!pk.parsed) throw Error("couldnt read the package: " + pk.error);
    auto& list = vertex ? pk.vertexBuffers : pk.indexBuffers;
    if (idx >= list.size()) throw Error("mesh buffer ids dont match the package");
    GeomBuffer b = list[idx];
    packageInsert(f, b.sizeField + 4 + b.size, bytes, n);
    f.put32(b.sizeField, b.size + n);  // its before the insert so it didnt move
    return b.size;
}

// new vb + a runtime slot at the end of VBIBs slots (ABABABAB
uint32_t newVertexBuffer(Nu20File& f, const std::vector<uint8_t>& bytes) {
    Package pk = f.parsePackage();
    if (!pk.parsed) throw Error("couldnt read the package: " + pk.error);
    uint32_t id = (uint32_t)pk.vertexBuffers.size();
    uint32_t at = id ? pk.vertexBuffers.back().sizeField + 4 + pk.vertexBuffers.back().size : pk.texturesEnd + 2;
    std::vector<uint8_t> entry(4 + bytes.size());
    uint32_t n = (uint32_t)bytes.size();
    std::memcpy(entry.data(), &n, 4);
    std::memcpy(entry.data() + 4, bytes.data(), n);
    packageInsert(f, at, entry.data(), (uint32_t)entry.size());
    put16(f, pk.texturesEnd, (uint16_t)(id + 1)); // its before the insert so it didnt move

    uint32_t h = meshHeader(f);
    uint16_t slots = f.u16(h);
    if (slots != id) throw Error("mesh headers vb count doesnt match the package");
    const uint8_t fill[4] = {0xAB, 0xAB, 0xAB, 0xAB};
    insertBytes(f, f.follow(h + 4) + slots * 4, 4, fill);
    put16(f, meshHeader(f), (uint16_t)(slots + 1));
    return id;
}

}

std::string paletteText(const int8_t palette[8]) {
    std::string s;
    for (int k = 0; k < 8 && palette[k] >= 0; k++) s += " " + std::to_string(palette[k]);
    return s;
}

std::vector<SkinPiece> splitByPalette(const MeshGeometry& g) {
    std::vector<SkinPiece> out;
    std::vector<int> pieceOf(g.triangles.size() / 3);
    std::vector<std::vector<int16_t>> palettes(1);
    for (size_t t = 0; t < pieceOf.size(); t++) {
        std::vector<int16_t> used;
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++) {
                int16_t b = g.skinBones[g.triangles[t * 3 + c] * 3 + k];
                if (b >= 0 && std::find(used.begin(), used.end(), b) == used.end()) used.push_back(b);
            }
        if (used.size() > 8) throw Error("a triangle uses " + std::to_string(used.size()) + " bones, a mesh can only use 8");
        auto& cur = palettes.back();
        size_t extra = 0;
        for (int16_t b : used) extra += std::find(cur.begin(), cur.end(), b) == cur.end();
        if (cur.size() + extra > 8) palettes.emplace_back();
        for (int16_t b : used)
            if (std::find(palettes.back().begin(), palettes.back().end(), b) == palettes.back().end()) palettes.back().push_back(b);
        pieceOf[t] = (int)palettes.size() - 1;
    }
    for (size_t p = 0; p < palettes.size(); p++) {
        SkinPiece piece;
        std::fill(std::begin(piece.palette), std::end(piece.palette), (int8_t)-1);
        for (size_t k = 0; k < palettes[p].size(); k++) piece.palette[k] = (int8_t)palettes[p][k];
        std::map<uint32_t, uint32_t> remap;
        MeshGeometry& h = piece.g;
        for (size_t t = 0; t < pieceOf.size(); t++) {
            if (pieceOf[t] != (int)p) continue;
            for (int c = 0; c < 3; c++) {
                uint32_t v = g.triangles[t * 3 + c];
                auto [it, fresh] = remap.emplace(v, (uint32_t)remap.size());
                if (fresh) {
                    h.positions.insert(h.positions.end(), &g.positions[v * 3], &g.positions[v * 3] + 3);
                    h.normals.insert(h.normals.end(), &g.normals[v * 3], &g.normals[v * 3] + 3);
                    h.uvs.insert(h.uvs.end(), &g.uvs[v * 2], &g.uvs[v * 2] + 2);
                    for (int k = 0; k < 3; k++) {
                        int16_t b = g.skinBones[v * 3 + k];
                        auto slot = std::find(palettes[p].begin(), palettes[p].end(), b);
                        h.skinBones.push_back(b < 0 ? -1 : (int16_t)(slot - palettes[p].begin()));
                        h.skinWeights.push_back(g.skinWeights[v * 3 + k]);
                    }
                }
                h.triangles.push_back(it->second);
            }
        }
        out.push_back(std::move(piece));
    }
    return out;
}

namespace {

uint32_t addOneMesh(Nu20File& f, const MeshGeometry& g, uint32_t material, const std::vector<uint32_t>& models, const float matrix[16], std::string* report, const int8_t palette[8]);
}

uint32_t addMesh(Nu20File& f, const MeshGeometry& g, uint32_t material, const std::vector<uint32_t>& models, const float matrix[16], std::string* report, uint8_t bone) {
    auto mats = materials(f);
    if (material >= mats.size()) throw Error("no material " + std::to_string(material));
    bool skinnedMaterial = f.isCharacterModel() && vertexLayout(f.u32(mats[material].record + 0x1F0)).find("blend");
    if (!g.skinned() || !skinnedMaterial) {
        if (g.skinned() && f.isCharacterModel())
            throw Error("mesh has skin weights but material " + std::to_string(material) + " isnt skinned");
        int8_t palette[8] = {(int8_t)bone, -1, -1, -1, -1, -1, -1, -1};
        if (!g.skinned()) return addOneMesh(f, g, material, models, matrix, report, palette);
        MeshGeometry plain = g;
        plain.skinBones.clear(), plain.skinWeights.clear();
        return addOneMesh(f, plain, material, models, matrix, report, palette);
    }
    auto pieces = splitByPalette(g);
    std::string log;
    uint32_t first = 0;
    for (size_t i = 0; i < pieces.size(); i++) {
        std::string pieceLog;
        uint32_t idx = addOneMesh(f, pieces[i].g, material, models, matrix, &pieceLog, pieces[i].palette);
        if (!i) first = idx;
        log += pieceLog;
    }
    if (pieces.size() > 1) log += "  split         " + std::to_string(pieces.size()) + " meshes of at most 8 bones each\n";
    if (report) *report = log;
    return first;
}

namespace {

uint32_t addOneMesh(Nu20File& f, const MeshGeometry& g, uint32_t material, const std::vector<uint32_t>& models, const float matrix[16], std::string* report, const int8_t palette[8]) {
    if (models.empty()) throw Error("no game model to add the mesh to");
    auto mats = materials(f);
    if (material >= mats.size()) throw Error("no material " + std::to_string(material));
    uint32_t vf = f.u32(mats[material].record + 0x1F0);
    const VertexLayout layout = vertexLayout(vf);


    bool character = f.isCharacterModel();
    bool skinned = character && layout.find("blend");
    MeshGeometry local;
    int rigidBone = -1;
    if (character) {
        auto bones = characterBones(f);
        for (int k = 0; skinned && k < 8; k++)
            if (palette[k] >= (int)bones.size())
                throw Error("no bone " + std::to_string(palette[k]) + " (char only has " + std::to_string(bones.size()) + ")");
        DispLayout l = readDisp(f, 0);
        auto all = modelParts(f);
        auto onBone = modelBones(f);
        for (uint32_t m : models) {
            if (m >= all.size() || all[m].empty()) continue;
            bool modelSkinned = skinnedPart(f, l, all[m][0]);
            if (modelSkinned != skinned)
                throw Error("game model " + std::to_string(m) + (modelSkinned ? " is skinned but material " : " isnt skinned but material ") + std::to_string(material) + (skinned ? " is" : " isnt") + " (skinned mats have bone indices in vertex_format)");
            if (skinned) continue;
            int b = m < onBone.size() ? onBone[m] : -1;
            if (b < 0 || b >= (int)bones.size()) throw Error("game model " + std::to_string(m) + " isnt on any layer bone");
            if (rigidBone >= 0 && b != rigidBone)
                throw Error("game models " + std::to_string(models[0]) + " and " + std::to_string(m) + " are on different bones, add the mesh to each bones models separately");
            rigidBone = b;
        }
        if (rigidBone >= 0) {
            const float* M = bones[rigidBone].inverseBind;
            local = g;
            for (size_t i = 0; i < g.vertexCount(); i++) {
                const float* p = &g.positions[i * 3];
                const float* nrm = &g.normals[i * 3];
                float q[3], n[3], len = 0;
                for (int c = 0; c < 3; c++) {
                    q[c] = p[0] * M[c] + p[1] * M[4 + c] + p[2] * M[8 + c] + M[12 + c];
                    n[c] = nrm[0] * M[c] + nrm[1] * M[4 + c] + nrm[2] * M[8 + c];
                    len += n[c] * n[c];
                }
                len = len > 0 ? std::sqrt(len) : 1.0f;
                for (int c = 0; c < 3; c++) local.positions[i * 3 + c] = q[c], local.normals[i * 3 + c] = n[c] / len;
            }
        }
    }
    const std::vector<uint8_t> verts = encodeVertices(rigidBone >= 0 ? local : g, layout);
    const std::vector<uint16_t> strip = triangleStrip(g.triangles);
    if (strip.size() < 3) throw Error("mesh has no triangles");

    //exactly one vb per vertex_format, find ours thru the mats of each static mesh
    auto meshes = meshRecords(f);
    if (meshes.empty()) throw Error("file has no mesh list");
    std::map<uint32_t, uint32_t> bufferOf; // format -> vb
    std::map<uint32_t, uint32_t> indexOf; // vb -> ib
    std::map<uint32_t, uint32_t> ibUses;
    {
        Package pk = f.parsePackage();
        if (!pk.parsed) throw Error("couldnt read the package: " + pk.error);
        for (auto& m : meshes) {
            if (m.indexBuffer >= pk.indexBuffers.size()) continue;
            if (!m.dynamic) indexOf.emplace(m.vertexBuffer, m.indexBuffer);
            ibUses[m.indexBuffer]++;
        }
    }
    {
        DispLayout l = readDisp(f, 0);
        for (auto& parts : modelParts(f))
            for (auto& p : parts) {
                uint32_t cmd = l.commandAt(p.command);
                if (f.data[cmd] != kGeomCall || p.material >= mats.size()) continue;
                uint32_t rec = f.follow(cmd + 4);
                if (rec && !f.u32(rec + 0x28)) bufferOf.emplace(f.u32(mats[p.material].record + 0x1F0), f.u32(rec + 0x24));
            }
    }
    if (ibUses.empty()) throw Error("no meshes in the file to share an index buffer with");
    std::string log = "  vertex format " + hex(vf) + ", stride " + std::to_string(layout.stride) + ", " + std::to_string(g.vertexCount()) + " vertices, " + std::to_string(g.triangles.size() / 3) + " triangles -> strip of " + std::to_string(strip.size()) + " indices\n";

    uint32_t vb, ib, baseVertex;
    if (auto it = bufferOf.find(vf); it != bufferOf.end()) {
        vb = it->second;
        ib = indexOf.count(vb) ? indexOf[vb] : ibUses.begin()->first;
        uint32_t old = appendToBuffer(f, true, vb, verts.data(), (uint32_t)verts.size());
        if (old % layout.stride) throw Error("vertex buffer " + std::to_string(vb) + " size isnt a multiple of its stride");
        baseVertex = old / layout.stride;
        log += "  geometry      vertex buffer " + std::to_string(vb) + " from vertex " + std::to_string(baseVertex);
    } else {
        ib = std::max_element(ibUses.begin(), ibUses.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
        vb = newVertexBuffer(f, verts);
        baseVertex = 0;
        log += "  geometry      new vertex buffer " + std::to_string(vb) + " (nothing had this format yet)";
    }
    const uint32_t firstIndex =
        appendToBuffer(f, false, ib, (const uint8_t*)strip.data(), (uint32_t)strip.size() * 2) / 2;
    if (!f.parsePackage().parsed) throw Error("package doesnt parse anymore");
    log += ", index buffer " + std::to_string(ib) + " from index " + std::to_string(firstIndex) + "\n";
    meshes = meshRecords(f);

    uint32_t recAt = 0;
    for (auto& m : meshes) recAt = std::max(recAt, m.record + kMeshRecord);
    std::vector<uint8_t> rec(kMeshRecord, 0);
    uint32_t words[10] = {6, (uint32_t)strip.size() - 2, 0xFFFF0000 | layout.stride, 0xFFFFFFFF, 0xFFFF, baseVertex, (uint32_t)g.vertexCount(), firstIndex, ib, vb};
    std::memcpy(rec.data(), words, sizeof words);
    if (skinned) std::memcpy(&rec[0xA], palette, 8);
    insertBytes(f, recAt, kMeshRecord, rec.data());
    uint32_t record = recAt;

    uint32_t h = meshHeader(f);
    uint32_t n = f.u32(h + 0x14);
    uint32_t slot = f.follow(h + 0x10) + n * 4;
    insertBytes(f, slot, 4);
    shiftAll({&record}, slot, 4);
    writePointer(f, slot, record);
    f.put32(meshHeader(f) + 0x14, n + 1);
    log += "  mesh          " + std::to_string(n) + " at " + f.describe(record) + (skinned ? ", skinned to bone(s)" + paletteText(palette) : "") + (rigidBone >= 0 ? ", rigid on bone " + std::to_string(rigidBone) : "") + "\n";

    DispLayout L = readDisp(f);
    uint32_t mtxEnd = 0;
    for (uint32_t i = 0; i < L.commandCount; i++) {
        uint32_t c = L.commandAt(i);
        if (f.data[c] == kMtxLoad) mtxEnd = std::max(mtxEnd, f.follow(c + 4) + kMatrix);
    }
    if (!mtxEnd) throw Error("no MTXLOAD to put the matrices after");
    uint32_t k = (uint32_t)models.size();
    std::vector<uint8_t> mtx(kMatrix * k);
    for (uint32_t j = 0; j < k; j++) std::memcpy(&mtx[j * kMatrix], matrix, kMatrix);
    insertBytes(f, mtxEnd, kMatrix * k, mtx.data());
    uint32_t mtxAt = mtxEnd;
    shiftAll({&record}, mtxEnd, kMatrix * k);

    // MTXLOAD + GEOMCALL right before END, only END moves
    L = readDisp(f);
    uint32_t endIdx = L.commandCount - 1;
    uint32_t endAt = L.commandAt(endIdx);
    if (f.data[endAt] != kEnd) throw Error("cmd list doesnt end with END");
    std::vector<uint8_t> cmds(2 * kCmd * k, 0);
    for (uint32_t j = 0; j < k; j++) {
        cmds[2 * j * kCmd] = kMtxLoad;
        cmds[(2 * j + 1) * kCmd] = kGeomCall;
    }
    InsertOptions cmdOpt;
    cmdOpt.allowInsideIndexedList = true;
    insertBytes(f, endAt, 2 * kCmd * k, cmds.data(), cmdOpt);
    shiftAll({&record, &mtxAt}, endAt, 2 * kCmd * k);
    for (uint32_t field : modelCommandIndexFields(f))
        if (f.u32(field) >= endIdx) f.put32(field, f.u32(field) + 2 * k);
    for (uint32_t j = 0; j < k; j++) {
        writePointer(f, endAt + 2 * j * kCmd + 4, mtxAt + j * kMatrix);
        writePointer(f, endAt + (2 * j + 1) * kCmd + 4, record);
    }
    f.put32(L.payload + 4, L.commandCount + 2 * k);
    log += "  commands      " + std::to_string(2 * k) + " at #" + std::to_string(endIdx) + "\n";

    for (uint32_t j = 0; j < k; j++) {
        uint32_t m = models[j];
        DispLayout l = readDisp(f, 0);
        uint32_t nmod = f.u32(l.payload + 0x10);
        if (m >= nmod) throw Error("no game model " + std::to_string(m));
        uint32_t model = f.follow(l.payload + 0x14) + m * 12;
        uint32_t parts = f.u32(model);
        if (!parts) throw Error("game model " + std::to_string(m) + " has no parts to add to");
        uint32_t sizes = f.follow(l.payload + 0x18);
        if (sizes && f.u16(sizes + m * 2) != parts) throw Error("game model " + std::to_string(m) + " size entry doesnt match its parts");
        uint32_t at = f.follow(model + 8) + parts * 4;
        insertBytes(f, at, 4);
        f.put32(at, endIdx + 2 * j + 1);
        at = f.follow(model + 4) + parts * 4;
        insertBytes(f, at, 4);
        f.put32(at, material);
        f.put32(model, parts + 1);
        if (sizes) put16(f, f.follow(readDisp(f, 0).payload + 0x18) + m * 2, (uint16_t)(parts + 1));
    }
    syncFirstParts(f);
    log += "  parts         added to " + std::to_string(k) + " game model(s)\n";

    {
        DispLayout l = readDisp(f);
        uint32_t sub = l.subs[material];
        if (f.follow(sub + 0x28)) {
            uint32_t old = f.u32(sub + 0x38), need = std::max(old, 2 * materialUses(f, material) + 1);
            if (need != old) {
                setMaterialCapacity(f, material, need);
                log += "  capacity      material " + std::to_string(material) + " " + std::to_string(old) + " -> " + std::to_string(need) + "\n";
            }
        }
    }
    if (report) *report = log;
    return n;
}

}

std::vector<std::string> checkModelPartsKept(const Nu20File& before, const Nu20File& after, const std::vector<uint32_t>& replaced) {
    std::vector<std::string> out;
    auto a = modelParts(before), b = modelParts(after);
    if (b.size() < a.size()) return {"game models were lost"};
    DispLayout la = readDisp(before, 0), lb = readDisp(after, 0);
    auto ma = materials(before), mb = materials(after);
    // mat idxs can shift, so compare what the part actually points at
    auto sameMaterial = [&](uint32_t ia, uint32_t ib) {
        if (ia >= ma.size() || ib >= mb.size()) return ia == ib;
        uint32_t ra = ma[ia].record, rb = mb[ib].record;
        for (uint32_t o = 0; o < kMaterialRecordSize && ra + o + 4 <= before.data.size() && rb + o + 4 <= after.data.size(); o += 4)
            if (o != 0x38 && o != 0x3C && before.u32(ra + o) != after.u32(rb + o)) return false;
        return true;
    };
    for (size_t i = 0; i < a.size() && out.size() < 10; i++) {
        if (std::find(replaced.begin(), replaced.end(), (uint32_t)i) != replaced.end()) continue;
        if (b[i].size() < a[i].size()) {
            out.push_back("game model " + std::to_string(i) + " lost parts");
            continue;
        }
        for (size_t j = 0; j < a[i].size(); j++) {
            uint32_t ca = la.commandAt(a[i][j].command), cb = lb.commandAt(b[i][j].command);
            uint32_t ta = before.follow(ca + 4), tb = after.follow(cb + 4);
            bool same = sameMaterial(a[i][j].material, b[i][j].material) && before.u16(ca) == after.u16(cb) && (ta == 0) == (tb == 0) && (!ta || std::equal(before.data.begin() + ta, before.data.begin() + ta + 16, after.data.begin() + tb));
            if (!same) out.push_back("game model " + std::to_string(i) + " part " + std::to_string(j) + " changed");
        }
    }
    return out;
}

void checkMeshes(const Nu20File& f, std::vector<std::string>& errors) {
    std::vector<MeshRecord> meshes;
    try {
        meshes = meshRecords(f);
    } catch (const Error& e) {
        errors.push_back(e.what());
        return;
    }
    Package pk = f.parsePackage();
    if (meshes.empty() || !pk.parsed) return;
    size_t before = errors.size();
    auto fail = [&](const std::string& s) {
        if (errors.size() - before < 10) errors.push_back(s);
    };
    std::map<uint32_t, uint32_t> strideOf;
    size_t boneCount = characterBones(f).size();
    for (size_t i = 0; i < meshes.size(); i++) {
        const MeshRecord& m = meshes[i];
        std::string who = "mesh " + std::to_string(i) + ": ";
        if (boneCount)
            for (int8_t b : m.palette)
                if (b >= (int)boneCount) fail(who + "bone palette names bone " + std::to_string(b) + " of " + std::to_string(boneCount));
        if (m.dynamic) continue;
        if (m.vertexBuffer >= pk.vertexBuffers.size() || m.indexBuffer >= pk.indexBuffers.size()) {
            fail(who + "buffer id out of range");
            continue;
        }
        if (!m.stride) { fail(who + "zero stride"); continue; }
        auto [it, fresh] = strideOf.emplace(m.vertexBuffer, m.stride);
        if (!fresh && it->second != m.stride) fail(who + "stride differs from other meshes in its vertex buffer");
        const GeomBuffer& V = pk.vertexBuffers[m.vertexBuffer];
        const GeomBuffer& I = pk.indexBuffers[m.indexBuffer];
        if ((uint64_t)(m.baseVertex + m.vertexCount) * m.stride > V.size) fail(who + "vertices run past their buffer");
        uint64_t indices = (uint64_t)m.primitives + 2;
        if (m.type != 6) continue;
        if ((m.firstIndex + indices) * 2 > I.size) { fail(who + "indices run past their buffer"); continue; }
        for (uint64_t k = 0; k < indices; k++)
            if (f.u16(I.sizeField + 4 + (uint32_t)(m.firstIndex + k) * 2) >= m.vertexCount) {
                fail(who + "index beyond its vertex count");
                break;
            }
    }
    auto mats = materials(f);
    const Chunk* c = f.findChunk("DISP");
    uint32_t cmds = c ? f.follow(c->offset + 8 + 8) : 0;
    if (!cmds) return;
    for (auto& parts : modelParts(f))
        for (auto& p : parts) {
            uint32_t cmd = cmds + p.command * kCmd;
            if (f.data[cmd] != kGeomCall || p.material >= mats.size()) continue;
            uint32_t rec = f.follow(cmd + 4);
            if (!rec || f.u32(rec + 0x28)) continue;
            uint32_t vf = f.u32(mats[p.material].record + 0x1F0);
            uint32_t want = vertexLayout(vf).stride;
            if (f.u16(rec + 8) != want)
                fail("material " + std::to_string(p.material) + " (vertex_format " + hex(vf) + ", stride " + std::to_string(want) + ") draws a mesh of stride " + std::to_string(f.u16(rec + 8)));
        }
}

namespace {

bool fileExists(const std::string& path) { return std::ifstream(path, std::ios::binary).good(); }

int64_t pickShading(const Nu20File& f, bool textured, bool skinned) {
    auto mats = materials(f);
    static const uint16_t otherSlots[] = {0xBC, 0xC0, 0xC4, 0xE8, 0xEC, 0xFC, 0x100, 0x104, 0x108, 0x10C, 0x110, 0x24C, 0x298, 0x29C};
    std::map<uint32_t, uint32_t> uses;
    DispLayout l = readDisp(f, 0);
    bool character = f.isCharacterModel();
    for (auto& parts : modelParts(f))
        for (auto& p : parts)
            if (partMesh(f, l, p) && (!character || skinnedPart(f, l, p) == skinned)) uses[p.material]++;
    int64_t best = -1;
    uint32_t bestUses = 0;
    for (auto& [m, n] : uses) {
        if (m >= mats.size()) continue;
        uint32_t rec = mats[m].record;
        if ((f.u32(rec + 0x40) & 0xF) != 0 || (f.s32(rec + 0xB8) >= 0) != textured) continue;
        bool single = true;
        for (uint16_t o : otherSlots) single = single && f.s32(rec + o) == -1;
        if (single && n > bestUses) best = m, bestUses = n;
    }
    return best;
}

void keepLastParts(Nu20File& f, uint32_t m, uint32_t n) {
    uint32_t pl = readDisp(f, 0).payload;
    uint32_t model = f.follow(pl + 0x14) + m * 12;
    uint32_t p = f.u32(model);
    if (p <= n) return;
    for (uint32_t off : {8u, 4u}) {
        uint32_t list = f.follow(model + off);
        for (uint32_t j = 0; j < n; j++) f.put32(list + j * 4, f.u32(list + (p - n + j) * 4));
        removeBytes(f, list + n * 4, (p - n) * 4);
        model = f.follow(readDisp(f, 0).payload + 0x14) + m * 12;
    }
    f.put32(model, n);
    if (uint32_t sizes = f.follow(readDisp(f, 0).payload + 0x18)) put16(f, sizes + m * 2, (uint16_t)n);
    syncFirstParts(f);
}

}

void syncFirstParts(Nu20File& f) {
    const Chunk* c = f.findChunk("DISP");
    if (!c) return;
    uint32_t pl = c->offset + 8, n = f.u32(pl + 0x10), models = f.follow(pl + 0x14);
    uint32_t first = f.follow(pl + 0x1C), cmds = f.follow(pl + 0x20);
    if (!models || !first || !cmds) return;
    for (uint32_t i = 0; i < n; i++)
        if (uint32_t cl = f.follow(models + i * 12 + 8); cl >= cmds) put16(f, first + i * 2, (uint16_t)((cl - cmds) / 4));
}

void checkModelLists(const Nu20File& f, std::vector<std::string>& errors) {
    const Chunk* c = f.findChunk("DISP");
    if (!c) return;
    uint32_t pl = c->offset + 8, n = f.u32(pl + 0x10), models = f.follow(pl + 0x14);
    uint32_t sizes = f.follow(pl + 0x18), first = f.follow(pl + 0x1C), cmds = f.follow(pl + 0x20), mats = f.follow(pl + 0x24);
    if (!models || !sizes || !first || !cmds || !mats || n > 100000) return;
    if (uint32_t bnds = f.follow(pl + 0x44); bnds && f.u32(bnds - 4) > n)
        errors.push_back("BNDS has " + std::to_string(f.u32(bnds - 4)) + " entries for " + std::to_string(n) + " game models");
    size_t before = errors.size();
    for (uint32_t i = 0; i < n && errors.size() - before < 5; i++) {
        uint32_t m = models + i * 12, cl = f.follow(m + 8), ml = f.follow(m + 4);
        std::string who = "game model " + std::to_string(i) + ": ";
        if (f.u16(sizes + i * 2) != f.u32(m)) errors.push_back(who + "size entry (DISP+0x18) doesnt match its part count");
        if (cl && f.u16(first + i * 2) != (cl - cmds) / 4) errors.push_back(who + "first part idx (DISP+0x1C) doesnt match where its cmd list is");
        if (cl && ml && (cl - cmds) != (ml - mats)) errors.push_back(who + "cmd and mat lists are at different spots");
    }
}

void keepNewestParts(Nu20File& f, uint32_t model, uint32_t count) { keepLastParts(f, model, count); }

void importModel(Nu20File& f, const Model& obj, const std::vector<uint32_t>& models, const ImportOptions& opt, std::string* report) {
    if (models.empty()) throw Error("no game model to import into");
    if (opt.envMap >= 0 && !isCubemap(f, opt.envMap)) throw Error("--envmap " + std::to_string(opt.envMap) + " isnt a cubemap in this file");
    std::string log;
    int64_t shading[2] = {opt.plainShading, opt.texturedShading};
    constexpr int64_t kPreset = -2;
    std::map<std::string, int32_t> textureOf;
    std::vector<uint32_t> created;
    int64_t placement = -1;
    bool skinned = false;
    {
        auto parts = modelParts(f);
        if (models[0] < parts.size() && !parts[models[0]].empty()) {
            placement = parts[models[0]][0].material;
            skinned = f.isCharacterModel() && skinnedPart(f, readDisp(f, 0), parts[models[0]][0]);
        }
    }
    auto addImage = [&](const std::string& image, const std::string& material) -> int32_t {
        std::string dds = image;
        size_t dot = dds.find_last_of('.');
        if (dot == std::string::npos || dds.find_first_of("/\\", dot) != std::string::npos) dds += ".dds";
        else dds = dds.substr(0, dot) + ".dds";
        std::string source = fileExists(dds) ? dds : image;
        if (!fileExists(source)) throw Error("material '" + material + "': cant find the image (tried " + image + " and " + dds + ")");
        auto it = textureOf.find(source);
        if (it == textureOf.end()) {
            std::string how;
            std::vector<uint8_t> bytes = textureBytes(source, &how);
            it = textureOf.emplace(source, (int32_t)addTexture(f, bytes)).first;
            log += "  texture       " + std::to_string(it->second) + " from " + source + (how.empty() ? "" : " (" + how.substr(how.find(": ") + 2) + ")") + "\n";
        }
        return it->second;
    };
    auto rgb = [](const float c[3]) {
        auto b = [](float x) { return (uint32_t)std::clamp((int)std::lround(x * 255), 0, 255); };
        return 0xFF000000 | b(c[2]) << 16 | b(c[1]) << 8 | b(c[0]);
    };
    static const uint32_t kRenderState[] = {0x3E160010, 0x3E160011, 0x3E160012, 0x3E560010};
    static const char* const kBlendName[] = {"opaque", "alpha", "additive", "cutout"};
    for (size_t i = 0; i < obj.parts.size(); i++) {
        const ModelMaterial& om = obj.materials[i];
        int32_t tex = om.texture.empty() ? -1 : addImage(om.texture, om.name);
        int32_t normalTex = om.normalMap.empty() ? -1 : addImage(om.normalMap, om.name);
        int64_t tmpl = kPreset;
        if (normalTex < 0 && opt.envMap < 0) {
            int64_t& chosen = shading[tex >= 0];
            if (chosen == -1 && opt.presets) chosen = kPreset;
            if (chosen == -1) chosen = pickShading(f, tex >= 0, skinned);
            if (chosen == -1) chosen = kPreset;
            tmpl = chosen;
        }
        auto mats = materials(f);
        if (tmpl >= (int64_t)mats.size()) throw Error("no material " + std::to_string(tmpl));
        std::vector<std::pair<std::string, std::string>> values;
        std::string presetName;
        if (skinned) presetName += "skinned-";
        if (normalTex >= 0) presetName += "normalmap-";
        if (opt.envMap >= 0) presetName += "envmap-";
        if (presetName.empty()) presetName = "standard";
        else presetName.pop_back();
        if (tmpl == kPreset) {
            if (placement < 0) throw Error("model " + std::to_string(models[0]) + " has no material to put new ones next to");
            values = shadingValues(*findShadingPreset(presetName));
        } else {
            for (auto& fld : materialFields())
                if (fld.shading) values.push_back({fld.name, formatMaterialField(&f.data[mats[tmpl].record], fld)});
        }
        values.push_back({"texture", std::to_string(tex)});
        if (normalTex >= 0) values.push_back({"surface_texture", std::to_string(normalTex)});
        if (opt.envMap >= 0) {
            values.push_back({"envmap_texture", std::to_string(opt.envMap)});
            values.push_back({"envmap_p1", std::to_string(opt.reflect)});
        }
        if (tex < 0 && om.hasKd) values.push_back({"layer0_colour", hex(rgb(om.kd))});
        values.push_back({"render_state", hex(kRenderState[om.blend] | (om.doubleSided ? 0x2000 : 0))});
        if (om.blend == ModelMaterial::Alpha) values.push_back({"layer0_alpha", std::to_string(om.alpha)});
        if (om.glow > 0 && om.emission[0] + om.emission[1] + om.emission[2] > 0) {
            values.push_back({"glow_colour", hex(rgb(om.emission))});
            values.push_back({"glow", std::to_string(om.glow)});
        }
        std::vector<uint8_t> rec = buildMaterialRecord(values);
        if (auto problems = envmapProblems(f, rec.data()); !problems.empty()) throw Error("material '" + om.name + "': " + problems[0]);
        int64_t group = tmpl == kPreset ? placement : tmpl;
        if (om.blend == ModelMaterial::Alpha || om.blend == ModelMaterial::Additive)
            if (int64_t g = mostUsedMaterialWithBlend(f, om.blend == ModelMaterial::Alpha ? 1 : 2); g >= 0) group = g;
        std::string matLog;
        uint32_t k = createMaterial(f, (uint32_t)group, &matLog, true, rec);
        for (auto& c : created) c += c >= k;
        for (auto& s : shading) s += s >= (int64_t)k;
        placement += placement >= (int64_t)k;
        created.push_back(k);
        std::string extras;
        if (normalTex >= 0) extras += ", normal map " + std::to_string(normalTex);
        if (opt.envMap >= 0) extras += ", cubemap " + std::to_string(opt.envMap);
        if (om.blend != ModelMaterial::Opaque) extras += std::string(", ") + kBlendName[om.blend];
        if (om.doubleSided) extras += ", double-sided";
        if (om.glow > 0) extras += ", glow";
        log += "  material      " + std::to_string(k) + " for '" + om.name + "' (" + (tmpl == kPreset ? "preset " + presetName : "shading of " + std::to_string(tmpl)) + ", texture " + std::to_string(tex) + extras + ")\n";
    }
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (size_t i = 0; i < obj.parts.size(); i++) {
        std::string meshLog;
        uint32_t idx = addMesh(f, obj.parts[i], created[i], models, identity, &meshLog, opt.bone);
        log += "  mesh          " + std::to_string(idx) + " for '" + obj.materials[i].name + "': " + std::to_string(obj.parts[i].vertexCount()) + " vertices, " + std::to_string(obj.parts[i].triangles.size() / 3) + " triangles\n";
        if (size_t at = meshLog.find("  split"); at != std::string::npos) log += meshLog.substr(at);
    }
    if (opt.replace) {
        for (uint32_t m : models) keepLastParts(f, m, (uint32_t)obj.parts.size());
        log += "  replaced      the old parts of " + std::to_string(models.size()) + " game model(s)\n";
    }
    if (report) *report = log;
}

}