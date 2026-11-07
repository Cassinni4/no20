#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "nu20.h"

namespace no20 {

struct DdsInfo {
    uint32_t width, height, mips;
    uint32_t format;
    bool cube = false;
};

DdsInfo parseDds(const std::vector<uint8_t>& dds);

struct Material {
    uint32_t slot, record;
    int16_t texture16;
    int32_t texture32;
};
std::vector<Material> materials(const Nu20File& f);

constexpr uint32_t kMaterialRecordSize = 0x2C4;

struct MaterialField {
    uint16_t off;
    char type;
    const char* name;
    uint32_t def;
    bool shading = false;
};
const std::vector<MaterialField>& materialFields();
const MaterialField* findMaterialField(const std::string& name);
std::string formatMaterialField(const uint8_t* record, const MaterialField& fld);
void setMaterialField(uint8_t* record, const MaterialField& fld, const std::string& value);

std::vector<uint8_t> buildMaterialRecord(const std::vector<std::pair<std::string, std::string>>& values);

struct ShadingPreset {
    const char* name;
    uint32_t vertexFormat, textureFlags, shaderFlags, shaderFlags2;
    const char* use;
};
const std::vector<ShadingPreset>& shadingPresets();
const ShadingPreset* findShadingPreset(const std::string& name);
std::vector<std::pair<std::string, std::string>> shadingValues(const ShadingPreset& p);

std::vector<std::string> shadingProblems(const uint8_t* record);
std::vector<std::string> envmapProblems(const Nu20File& f, const uint8_t* record);
bool isCubemap(const Nu20File& f, int32_t texture);

uint32_t addTexture(Nu20File& f, const std::vector<uint8_t>& dds);
void setMaterialTexture(Nu20File& f, uint32_t material, int32_t texture);

}