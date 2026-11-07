#include "content.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <unordered_set>

#include "display.h"
#include "edit.h"
#include "mesh.h"

namespace no20 {

static constexpr uint32_t kDescriptorSize = 0x80;
static constexpr uint32_t kOldDescriptorSize = 0x28;

static uint32_t rd32(const std::vector<uint8_t>& v, size_t o) {
    uint32_t x;
    std::memcpy(&x, &v[o], 4);
    return x;
}
static void wr32(std::vector<uint8_t>& v, size_t o, uint32_t x) { std::memcpy(&v[o], &x, 4); }

DdsInfo parseDds(const std::vector<uint8_t>& dds) {
    if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0) throw Error("not a dds file");
    DdsInfo info{rd32(dds, 0x10), rd32(dds, 0x0C), std::max(1u, rd32(dds, 0x1C)), 0};
    if (const uint32_t caps2 = rd32(dds, 0x70); caps2 & 0x200) {
        if ((caps2 & 0xFE00) != 0xFE00) throw Error("cubemap dds is missing faces, the game needs all 6");
        if (info.width != info.height) throw Error("cubemap dds faces arent square");
        info.cube = true;
    }
    std::string fourcc((const char*)&dds[0x54], 4);
    if (fourcc == "DXT1") info.format = 1;
    else if (fourcc == "DXT3") info.format = 4;
    else if (fourcc == "DXT5") info.format = 6;
    else throw Error("cant use dds format '" + fourcc + "', the game only does DXT1/DXT3/DXT5");
    if (info.width == 0 || info.height == 0) throw Error("dds is 0x0");

    size_t face = 0, blockBytes = info.format == 1 ? 8 : 16;
    for (uint32_t m = 0, w = info.width, h = info.height; m < info.mips; m++) {
        face += (size_t)std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * blockBytes;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    size_t need = 128 + face * (info.cube ? 6 : 1);
    if (dds.size() < need) throw Error("dds is cut off (" + std::to_string(dds.size()) + " < " + std::to_string(need) + " bytes)");
    return info;
}

std::vector<Material> materials(const Nu20File& f) {
    std::vector<Material> out;
    uint32_t g = f.gsnhPayload();
    int32_t rel = f.s32(g + 0x0C);
    uint32_t count = f.u32(g + 0x10);
    if (rel == 0 || count == 0) return out;
    if (count > 100000) throw Error("material count makes no sense, broken file?");
    uint32_t array = (uint32_t)((int64_t)g + 0x0C + rel);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = array + i * 4;
        uint32_t rec = f.ptr(slot);
        if (rec + 0xBC > f.pntr) throw Error("material " + std::to_string(i) + " is outside the scene");
        out.push_back({slot, rec, (int16_t)f.u16(rec + 0x74), f.s32(rec + 0xB8)});
    }
    return out;
}

const std::vector<MaterialField>& materialFields() {
    constexpr uint32_t F1 = 0x3F800000, NONE = 0xFFFFFFFF;
    static const std::vector<MaterialField> fields = [] {
        std::vector<MaterialField> v = {
            {0x40, 'x', "render_state", 0x3E160010}, // b0-3 blend, b10-11 wrap, b12-13 cull, b14-15 depth, b20-22 alpha test, b23-30 alpha ref
            {0x44, 'x', "flags", 0x10},
            {0x54, 'f', "colour_r", F1},
            {0x58, 'f', "colour_g", F1},
            {0x5C, 'f', "colour_b", F1},
            {0x60, 'f', "colour_a", 0},
            {0x64, 'f', "fx_y", 0},
            {0x68, 'f', "fx_z", 0},
            {0x6C, 'f', "unk_6c", 0},
            {0x76, 'h', "unk_76", 0},
            {0x78, 'x', "unk_78", 0xFFA6},
            {0x7C, 'x', "unk_7c", 0},
            {0x80, 'f', "lightmap_offset0", 0},
            {0x84, 'f', "lightmap_offset1", 0},
            {0x88, 'f', "lightmap_offset2", 0},
            {0x8C, 'f', "lightmap_offset3", 0},
            {0x98, 'x', "render_path", 0x9D00}, //+0x99 picks the draw path 9D normal, AB lightmapped, A8 layered, 97 special
            {0x9C, 'x', "unk_9c", 0xFF000000},
            {0xA0, 'x', "unk_a0", 0},
            {0xB0, 'x', "unk_b0", 1},
            {0xB4, 'x', "texture_flags", 0, true},
            {0xB8, 't', "texture", NONE},
            {0xBC, 't', "layer1_texture", NONE},
            {0xC0, 't', "layer2_texture", NONE},
            {0xC4, 't', "layer3_texture", NONE},
            {0xC8, 'x', "layer0_colour", NONE},
            {0xCC, 'x', "layer1_colour", 0xFF7F7F7F},
            {0xD0, 'x', "layer2_colour", 0xFF7F7F7F},
            {0xD4, 'x', "layer3_colour", 0xFF7F7F7F},
            {0xD8, 'f', "layer0_alpha", F1},
            {0xDC, 'f', "layer1_alpha", F1},
            {0xE0, 'f', "layer2_alpha", F1},
            {0xE4, 'f', "layer3_alpha", F1},
            {0xE8, 't', "specular2_texture", NONE},
            {0xEC, 't', "surface2_texture", NONE},
            {0xF0, 'f', "surface_scale", F1},
            {0xF4, 'x', "specular2_colour", NONE},
            {0xF8, 'b', "layer1_blend", 0},
            {0xF9, 'b', "layer2_blend", 0},
            {0xFA, 'b', "layer3_blend", 0},
            {0xFB, 'b', "bitangent_flip", 0},
            {0xFC, 't', "specular_texture", NONE},
            {0x100, 't', "surface_texture", NONE},
            {0x104, 't', "envmap_texture", NONE},
            {0x108, 't', "shine_texture", NONE},
            {0x10C, 't', "vertex_fetch_texture", NONE},
            {0x110, 't', "vtf_normal_texture", NONE},
            {0x114, 'f', "vtf_height", F1},
            {0x118, 'f', "vtf_normal", F1},
            {0x11C, 'x', "ambient_colour", 0xFF000000},
            {0x120, 'x', "glow_colour", 0xFF000000},
            {0x124, 'x', "refraction_colour", 0xFF004C00},
            {0x128, 'x', "specular_colour", NONE},
            {0x12C, 'f', "reflection_power", 0x3F000000},
            {0x130, 'f', "specular_exponent", 0x41C80000},
            {0x134, 'f', "surface_p0", F1},
            {0x138, 'f', "surface_p1", F1},
            {0x13C, 'f', "envmap_p1", 0x3F000000},
            {0x140, 'f', "envmap_p0", 0x3F000000},
            {0x144, 'f', "fresnel_mul", F1},
            {0x148, 'f', "fresnel_coeff", 0x40200000},
            {0x14C, 'f', "surface_p3", 0},
            {0x150, 'f', "refraction_index", F1},
            {0x154, 'f', "dapple_limit", 0x43480000},
            {0x158, 'f', "refraction_alpha", 0x3DCCCCCD},
            {0x15C, 'b', "uv_lightmap", 1, true},
            {0x15D, 'b', "uv_surface", 1, true},
            {0x15E, 'b', "uv_specular", 1, true},
            {0x15F, 'b', "uv_layer1", 1, true},
            {0x160, 'b', "uv_layer2", 1, true},
            {0x161, 'b', "uv_layer3", 1, true},
            {0x162, 'b', "uv_detail", 1, true},
            {0x18C, 'f', "layer0_lod_bias", 0},
            {0x190, 'f', "layer1_lod_bias", 0},
            {0x194, 'f', "layer2_lod_bias", 0},
            {0x198, 'f', "layer3_lod_bias", 0},
            {0x19C, 'f', "specular_lod_bias", 0},
            {0x1A0, 'f', "surface_lod_bias", 0},
            {0x1A8, 'f', "vtf_normal_lod_bias", 0},
            {0x1AC, 'f', "surface2_lod_bias", 0},
            {0x1B0, 'f', "specular2_lod_bias", 0},
            {0x1B4, 'f', "glow", 0},
            {0x1B8, 'f', "gooch_p0", 0x3ECCCCCD},
            {0x1BC, 'f', "gooch_p1", 0x40000000},
            {0x1C0, 'x', "scroll0_enable", NONE},
            {0x1C4, 'x', "scroll1_enable", NONE},
            {0x1C8, 'x', "scroll2_enable", NONE},
            {0x1CC, 'x', "scroll3_enable", NONE},
            {0x1F0, 'x', "vertex_format", 0, true},
            {0x248, 'f', "envmap_p2", 0},
            {0x24C, 't', "diffenv_texture", NONE},
            {0x250, 'x', "carpaint_tint0", 0xFF7F7F7F},
            {0x254, 'x', "carpaint_tint1", 0xFF7F7F7F},
            {0x258, 'x', "carpaint_tint2", 0xFF7F7F7F},
            {0x25C, 'x', "carpaint_tint3", 0xFF7F7F7F},
            {0x260, 'f', "carpaint_p0", F1},
            {0x264, 'f', "carpaint_p1", F1},
            {0x268, 'x', "input_defines", 0},
            {0x26C, 'x', "shader_flags", 0, true},
            {0x270, 'x', "shader_flags2", 0, true},
            {0x274, 'f', "fractal_p0", 0},
            {0x278, 'f', "fractal_p3", 0},
            {0x27C, 'f', "surface2_p2", 0},
            {0x280, 'f', "fractal_p1", 0},
            {0x284, 'f', "fractal_p2", 0},
            {0x288, 'f', "surface2_p1", 0},
            {0x28C, 'f', "env_rotation_x", 0},
            {0x290, 'f', "env_rotation_y", 0},
            {0x294, 'f', "env_rotation_z", 0},
            {0x298, 't', "tex_anim_map", NONE},
            {0x29C, 't', "tex_anim_curves", NONE},
            {0x2A0, 'f', "diffenv_p0", F1},
            {0x2A4, 'f', "diffenv_p1", 0},
            {0x2A8, 'f', "diffenv_p2", F1},
            {0x2AC, 'f', "diffenv_p3", 0x40200000},
            {0x2B0, 'f', "gooch_p2", F1},
            {0x2B4, 'f', "vtf_offset", 0},
        };
        for (int i = 0; i < 4; i++) {
            static const char* names[4][6] = {
                {"scroll0_type_u", "scroll0_type_v", "scroll0_trig_u", "scroll0_trig_v", "scroll0_speed_u", "scroll0_speed_v"},
                {"scroll1_type_u", "scroll1_type_v", "scroll1_trig_u", "scroll1_trig_v", "scroll1_speed_u", "scroll1_speed_v"},
                {"scroll2_type_u", "scroll2_type_v", "scroll2_trig_u", "scroll2_trig_v", "scroll2_speed_u", "scroll2_speed_v"},
                {"scroll3_type_u", "scroll3_type_v", "scroll3_trig_u", "scroll3_trig_v", "scroll3_speed_u", "scroll3_speed_v"}};
            uint16_t at = (uint16_t)(0x1F8 + i * 0x14);
            v.push_back({at, 'b', names[i][0], 0});
            v.push_back({(uint16_t)(at + 1), 'b', names[i][1], 0});
            for (int k = 0; k < 4; k++) v.push_back({(uint16_t)(at + 4 + 4 * k), 'f', names[i][2 + k], 0});
        }
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.off < b.off; });
        return v;
    }();
    return fields;
}

const MaterialField* findMaterialField(const std::string& name) {
    for (auto& fld : materialFields())
        if (name == fld.name) return &fld;
    return nullptr;
}

static uint32_t fieldBits(const uint8_t* rec, const MaterialField& fld) {
    uint32_t v = 0;
    std::memcpy(&v, rec + fld.off, fld.type == 'b' ? 1 : fld.type == 'h' ? 2 : 4);
    return v;
}

static void putFieldBits(uint8_t* rec, const MaterialField& fld, uint32_t v) {
    std::memcpy(rec + fld.off, &v, fld.type == 'b' ? 1 : fld.type == 'h' ? 2 : 4);
}

std::string formatMaterialField(const uint8_t* rec, const MaterialField& fld) {
    uint32_t v = fieldBits(rec, fld);
    char buf[32];
    switch (fld.type) {
    case 'f': { float x; std::memcpy(&x, &v, 4); std::snprintf(buf, sizeof buf, "%g", x); break; }
    case 't': std::snprintf(buf, sizeof buf, "%d", (int32_t)v); break;
    case 'h': std::snprintf(buf, sizeof buf, "%d", (int16_t)v); break;
    case 'b': std::snprintf(buf, sizeof buf, "%u", v); break;
    default: std::snprintf(buf, sizeof buf, "0x%08X", v);
    }
    return buf;
}

void setMaterialField(uint8_t* rec, const MaterialField& fld, const std::string& value) {
    size_t used = 0;
    uint32_t v = 0;
    try {
        if (fld.type == 'f') {
            float x = std::stof(value, &used);
            std::memcpy(&v, &x, 4);
        } else {
            long long x = std::stoll(value, &used, 0);
            int64_t lo = fld.type == 'b' ? 0 : fld.type == 'h' ? -0x8000 : fld.type == 't' ? -1 : 0;
            int64_t hi = fld.type == 'b' ? 0xFF : fld.type == 'h' ? 0x7FFF : fld.type == 't' ? 0x3FFF : 0xFFFFFFFFll;
            if (fld.type == 't' && x != -1 && (x & 0x4000)) hi = 0x7FFF;  // b14 = shared tex
            if (x < lo || x > hi) throw Error("");
            v = (uint32_t)x;
        }
    } catch (...) {
        used = 0;
    }
    if (used != value.size()) throw Error(std::string("bad value for ") + fld.name + ": " + value);
    putFieldBits(rec, fld, v);
}

std::vector<uint8_t> buildMaterialRecord(const std::vector<std::pair<std::string, std::string>>& values) {
    std::vector<uint8_t> rec(kMaterialRecordSize, 0);
    for (auto& fld : materialFields()) putFieldBits(rec.data(), fld, fld.def);
    for (auto& [name, value] : values) {
        const MaterialField* fld = findMaterialField(name);
        if (!fld) throw Error("theres no material setting called " + name);
        setMaterialField(rec.data(), *fld, value);
    }
    int32_t tex;
    std::memcpy(&tex, &rec[0xB8], 4);
    int16_t t16 = (int16_t)tex;
    std::memcpy(&rec[0x74], &t16, 2);
    std::memcpy(&rec[0x70], &rec[0xD8], 4);
    return rec;
}

const std::vector<ShadingPreset>& shadingPresets() {
    static const std::vector<ShadingPreset> presets = {
        {"standard", 0x909, 0x8, 0x8, 0, "textured or plain color: position, normal, colour, one uv set"},
        {"normalmap", 0x929, 0x9, 0x9, 0x800, "standard plus a normal map, adds a packed tangent"},
        {"envmap", 0x909, 0x28, 0x28, 0, "standard plus an environment cube map"},
        {"skinned", 0x28909, 0x8, 0x8, 0, "standard for skinned chars"},
        {"skinned-normalmap", 0x28929, 0x1, 0x1, 0x800, "skinned plus a normal map"},
        {"normalmap-envmap", 0x929, 0x29, 0x29, 0x800, "normalmap plus an environment cube map"},
        {"skinned-envmap", 0x28909, 0x28, 0x28, 0, "skinned plus an environment cube map"},
        {"skinned-normalmap-envmap", 0x28929, 0x21, 0x21, 0x800, "skinned-normalmap plus an environment cube map"},
    };
    return presets;
}

const ShadingPreset* findShadingPreset(const std::string& name) {
    for (auto& p : shadingPresets())
        if (name == p.name) return &p;
    return nullptr;
}

std::vector<std::pair<std::string, std::string>> shadingValues(const ShadingPreset& p) {
    return {{"vertex_format", hex(p.vertexFormat)}, {"texture_flags", hex(p.textureFlags)}, {"shader_flags", hex(p.shaderFlags)}, {"shader_flags2", hex(p.shaderFlags2)}};
}

std::vector<std::string> shadingProblems(const uint8_t* rec) {
    auto u32 = [&](uint32_t o) { uint32_t v; std::memcpy(&v, rec + o, 4); return v; };
    auto s32 = [&](uint32_t o) { return (int32_t)u32(o); };
    uint32_t vf = u32(0x1F0), tf = u32(0xB4), sf = u32(0x26C);
    bool normal = vf & 0x88000C, tangent = vf & 0x1000030, bitangent = vf & 0x20000C0, colour = vf & 0x100;
    uint32_t uvs = (vf >> 11) & 7;
    std::vector<std::string> out;
    auto need = [&](bool ok, const std::string& what) {
        if (!ok) out.push_back(what);
    };
    need(uvs >= 1 || s32(0xB8) == -1, "a diffuse texture needs a uv set in vertex_format");
    uint32_t flags = tf | sf;
    need(!(flags & 0x1) || (normal && tangent), "normal-map flag needs a normal and a tangent");
    need(!(flags & 0x6000) || (tangent && bitangent), "vertex-texture flags need a tangent and a bitangent");
    need(!(flags & 0x8) || (normal && colour), "flag bit 3 needs a normal and a vertex colour");
    need(!(flags & 0x20) || normal, "envmap flag needs a normal");
    need(!(tf & 0x200000) == !(vf & 0x4000000), "texture_flags bit 21 and vertex_format bit 26 go together");
    need(!(tf & 0x100) || uvs >= 3, "layer 3 flag needs three uv sets");
    need(!(sf & 0x80000000) == !(vf & 0x4000000), "shader_flags bit 31 and vertex_format bit 26 go together");
    return out;
}

void setMaterialTexture(Nu20File& f, uint32_t material, int32_t texture) {
    auto mats = materials(f);
    if (material >= mats.size()) throw Error("no material " + std::to_string(material));
    if (texture < -1 || texture >= (int32_t)f.textures().size()) throw Error("no texture " + std::to_string(texture));
    uint32_t rec = mats[material].record;
    uint16_t t16 = (uint16_t)(int16_t)texture;
    std::memcpy(&f.data[rec + 0x74], &t16, 2);
    f.put32(rec + 0xB8, (uint32_t)texture);
}

bool isCubemap(const Nu20File& f, int32_t texture) {
    if (f.base) return false;
    auto tex = f.textures();
    if (texture < 0 || texture >= (int32_t)tex.size()) return false;
    uint32_t d = tex[texture].descriptor;
    return f.u32(d + 0x2C) == 8 && f.u32(d + 0x34) == 1;
}

std::vector<std::string> envmapProblems(const Nu20File& f, const uint8_t* rec) {
    int32_t env;
    uint32_t flags;
    std::memcpy(&env, rec + 0x104, 4);
    std::memcpy(&flags, rec + 0xB4, 4);
    bool flag = flags & 0x20;
    std::vector<std::string> out;
    if (env != -1 && !isCubemap(f, env)) out.push_back("envmap_texture " + std::to_string(env) + " isnt a cubemap in this file (add one with add-cubemap)");
    if (env != -1 && !flag) out.push_back("envmap_texture needs the envmap flag (texture_flags b5), use an envmap preset");
    if (env == -1 && flag) out.push_back("the envmap flag (texture_flags b5) needs envmap_texture=N");
    return out;
}

uint32_t addTexture(Nu20File& f, const std::vector<uint8_t>& dds) {
    DdsInfo info = parseDds(dds);
    Package pk = f.parsePackage();
    if (!pk.parsed) throw Error("couldnt read the package: " + pk.error);
    bool first = pk.textureFirst;
    if (info.cube && first) throw Error("texture first files cant have cubemaps"); // they probably can but im staying on the safe side
    uint32_t n = info.cube ? 6 : 1;
    uint32_t descSize = first ? kOldDescriptorSize : kDescriptorSize;
    uint32_t texDataEndRel = pk.texturesEnd - f.package;

    auto tex = f.textures();
    uint32_t at = 0;
    for (auto& t : tex) at = std::max(at, t.descriptor + descSize);
    const Chunk* home = tex.empty() ? f.findChunk("TST0") : f.chunkContaining(tex[0].descriptor);
    if (!home) throw Error("no TST0 chunk to put the texture in");
    if (tex.empty()) at = home->offset + 8;
    if (at > home->end()) throw Error("texture descriptors arent all in one chunk");

    // +0x1C +0x28 +0x30 +0x3C +0x4C are runtime ptrs, they still go in PNTR
    std::vector<uint8_t> desc(descSize * n, 0);
    wr32(desc, 0x00, info.width);
    wr32(desc, 0x04, info.height);
    std::random_device rng;
    for (int i = 0; i < 4; i++) wr32(desc, 0x08 + i * 4, rng());
    if (!first) {
        wr32(desc, 0x2C, info.cube ? 8 : 0x10);
        if (info.cube) {
            wr32(desc, 0x34, 1);
            wr32(desc, descSize * 5 + 0x2C, 0x10);
        }
        wr32(desc, 0x38, info.format);
        wr32(desc, 0x40, info.mips);
        wr32(desc, 0x44, (uint32_t)dds.size());
        wr32(desc, 0x50, 3);
        wr32(desc, 0x54, 1);
    }
    uint32_t descBytes = (uint32_t)desc.size();
    bool spare = at + descBytes <= home->end() && std::all_of(f.data.begin() + at, f.data.begin() + at + descBytes, [](uint8_t b) { return b == 0; }) && std::none_of(f.relocs.begin(), f.relocs.end(), [&](uint32_t r) { return r >= at && r < at + descBytes; });
    if (spare) std::copy(desc.begin(), desc.end(), f.data.begin() + at);
    else insertBytes(f, at, descBytes, desc.data());
    if (!first)
        for (uint32_t off : {0x1Cu, 0x28u, 0x30u, 0x3Cu, 0x4Cu}) addReloc(f, at + off);


    uint32_t g = f.gsnhPayload();
    uint32_t count = f.u32(g + 4);
    uint32_t ptrArray = f.ptr(g + 8);
    uint32_t list = f.s32(g) ? f.ptr(g) : 0;
    uint32_t pad = n % 2 ? 8 : 0;
    if (list && !count && list == ptrArray) {
        insertBytes(f, ptrArray, n * 8 + pad);
        g = f.gsnhPayload();
        writePointer(f, g, list, false);
        writePointer(f, g + 8, list + n * 4, false);
        ptrArray = list + n * 4;
    } else if (list && list + count * 4 == ptrArray) {
        insertBytes(f, ptrArray, n * 4);
        ptrArray += n * 4;
        insertBytes(f, ptrArray + count * 4, n * 4 + pad);
    } else {
        uint32_t room = (n * 4 + 15) & ~15u;
        insertBytes(f, ptrArray + count * 4, room);
        g = f.gsnhPayload();
        if (list) insertBytes(f, f.ptr(g) + count * 4, room);
        g = f.gsnhPayload();
        ptrArray = f.ptr(g + 8);
    }
    writePointer(f, ptrArray + count * 4, at);
    for (uint32_t k = 1; k < n; k++) writePointer(f, ptrArray + (count + k) * 4, at + descSize * 5);
    f.put32(f.gsnhPayload() + 4, count + n);

    uint32_t dataAt;
    if (first) {
        uint32_t entryAt = f.parsePackage().texturesEnd;
        std::vector<uint8_t> entry(0x18, 0);
        wr32(entry, 0x00, info.width);
        wr32(entry, 0x04, info.height);
        wr32(entry, 0x08, 1);
        wr32(entry, 0x0C, info.format == 6 ? 15 : 12);
        wr32(entry, 0x14, (uint32_t)dds.size());
        entry.insert(entry.end(), dds.begin(), dds.end());
        packageInsert(f, entryAt, entry.data(), (uint32_t)entry.size());
        uint16_t n16 = (uint16_t)(f.u16(4) + 1);
        std::memcpy(&f.data[4], &n16, 2);
        dataAt = entryAt + 0x18;
    } else {
        dataAt = f.package + texDataEndRel;
        f.data.insert(f.data.begin() + dataAt, dds.begin(), dds.end());
        f.put32(f.package, f.u32(f.package) + (uint32_t)dds.size());
    }

    auto after = f.textures();
    if (after.size() != count + n || after[count].data != dataAt ||
        !std::equal(dds.begin(), dds.end(), f.data.begin() + dataAt))
        throw Error("new texture doesnt read back right");
    if (!f.parsePackage().parsed) throw Error("package doesnt parse anymore");
    return count;
}

}