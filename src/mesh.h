#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "nu20.h"

namespace no20 {

struct VertexAttr {
    std::string name;
    uint32_t offset, size;
    char kind; // f float3, p u8x4, u float2, h half2, q float2
};
struct VertexLayout {
    uint32_t format = 0;
    uint32_t stride = 0;
    std::vector<VertexAttr> attrs;
    const VertexAttr* find(const std::string& name) const;
};
VertexLayout vertexLayout(uint32_t vertexFormat);

struct MeshRecord {
    uint32_t record;
    uint32_t type, primitives, stride, baseVertex, vertexCount, firstIndex, indexBuffer, vertexBuffer, dynamic;
    int8_t palette[8]; // +0xA, -1 = empty
    bool skinned() const { return palette[0] >= 0; }
};
std::vector<MeshRecord> meshRecords(const Nu20File& f);

void packageInsert(Nu20File& f, uint32_t at, const uint8_t* bytes, uint32_t n);

struct Bone {
    std::string name;
    int parent;
    float position[3];
    float inverseBind[16]; // row vectors, p * M
};
std::vector<Bone> characterBones(const Nu20File& f);

// bone each model sits on, -1 = skinned. layers are GSNH +0x18C count, +0x190 -> 0x14 byte layers (+0 name, +4 rigid array, +8 skinned obj, +C/+10 same again) obj +8 -> block, block +0xB0 -> model
std::vector<int> modelBones(const Nu20File& f);

std::vector<uint32_t> skinnedModels(const Nu20File& f);

int64_t mostUsedOpaqueMaterial(const Nu20File& f);
int64_t mostUsedMaterialWithBlend(const Nu20File& f, uint32_t blend);

struct MeshGeometry {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint32_t> triangles;
    std::vector<int16_t> skinBones;
    std::vector<uint8_t> skinWeights;
    size_t vertexCount() const { return positions.size() / 3; }
    bool skinned() const { return !skinBones.empty(); }
};
struct LoadOptions {
    float scale = 1.0f;
    float offset[3] = {0, 0, 0};
    bool mirrorX = false;
    bool flipWinding = false;
};
struct ModelMaterial {
    std::string name;
    float kd[3] = {1, 1, 1};
    bool hasKd = false;
    std::string texture;
    std::string normalMap;
    float alpha = 1.0f;
    enum Blend : uint8_t { Opaque, Alpha, Additive, Cutout } blend = Opaque;
    bool doubleSided = false;
    float emission[3] = {0, 0, 0};
    float glow = 0.0f;
};
struct Model {
    std::vector<ModelMaterial> materials;
    std::vector<MeshGeometry> parts;
};

MeshGeometry loadBmesh(const std::string& path, const std::vector<Bone>* bones, const LoadOptions& opt = {}, std::string* report = nullptr);
Model loadBmodel(const std::string& path, const std::vector<Bone>* bones, const LoadOptions& opt = {}, std::string* report = nullptr);
Model loadModel(const std::string& path, const std::vector<Bone>* bones, const LoadOptions& opt = {}, std::string* report = nullptr);

struct ImportOptions {
    int64_t texturedShading = -1, plainShading = -1;
    bool presets = false;
    bool replace = false;
    uint8_t bone = 0;
    int32_t envMap = -1;
    float reflect = 0.5f;
};
void importModel(Nu20File& f, const Model& m, const std::vector<uint32_t>& models, const ImportOptions& opt, std::string* report = nullptr);
void keepNewestParts(Nu20File& f, uint32_t model, uint32_t count);

std::vector<uint16_t> triangleStrip(const std::vector<uint32_t>& triangles);

std::vector<uint8_t> encodeVertices(const MeshGeometry& g, const VertexLayout& layout);

struct SkinPiece {
    MeshGeometry g;
    int8_t palette[8];
};
std::vector<SkinPiece> splitByPalette(const MeshGeometry& g);
std::string paletteText(const int8_t palette[8]);

uint32_t addMesh(Nu20File& f, const MeshGeometry& g, uint32_t material, const std::vector<uint32_t>& models, const float matrix[16], std::string* report = nullptr, uint8_t bone = 0);

std::vector<std::string> checkModelPartsKept(const Nu20File& before, const Nu20File& after, const std::vector<uint32_t>& replaced = {});

void syncFirstParts(Nu20File& f);
void checkModelLists(const Nu20File& f, std::vector<std::string>& errors);

void checkMeshes(const Nu20File& f, std::vector<std::string>& errors);

}