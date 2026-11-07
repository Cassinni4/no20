#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "nu20.h"

namespace no20 {

struct RenderGroup {
    uint32_t record;
    uint32_t firstCommand;
    uint16_t count, first;
};

struct DispLayout {
    uint32_t payload = 0;
    uint32_t commands = 0, commandCount = 0;
    std::vector<uint32_t> materials;
    std::vector<uint32_t> subs;
    std::vector<RenderGroup> groups;
    uint32_t commandAt(uint32_t index) const { return commands + index * 16; }
    uint32_t commandIndex(uint32_t pos) const { return (pos - commands) / 16; }
};

DispLayout readDisp(const Nu20File& f, int64_t materialCount = -1, bool strict = true);

std::vector<uint32_t> modelCommandIndexFields(const Nu20File& f);
std::vector<uint32_t> modelMaterialIndexFields(const Nu20File& f);

struct TexAnimLayout {
    uint32_t payload = 0, count = 0;
    uint32_t records = 0;
    uint32_t totalField = 0;
    uint32_t table = 0, total = 0;
    uint32_t chunkEnd = 0;
    uint32_t record(uint32_t i) const { return records + i * 0x20; }
};
bool readTexAnims(const Nu20File& f, TexAnimLayout& out);

std::vector<uint32_t> animatedMaterialFields(const Nu20File& f);

uint32_t addTextureAnimations(Nu20File& f, uint32_t from, uint32_t to, std::string* report = nullptr);

struct AnimInfo {
    uint32_t material = 0;
    std::string script;
    std::vector<uint16_t> frames;
};
std::vector<AnimInfo> textureAnimations(const Nu20File& f);
uint32_t addAnimation(Nu20File& f, uint32_t material, const std::string& script, const std::vector<uint16_t>& frames, std::string* report = nullptr);

uint32_t createMaterial(Nu20File& f, uint32_t groupOf, std::string* report, bool inItsGroup, const std::vector<uint8_t>& record);

uint32_t materialUses(const Nu20File& f, uint32_t material);
bool capacityRuleHolds(const Nu20File& f);
void setMaterialCapacity(Nu20File& f, uint32_t material, uint32_t capacity);

std::vector<std::string> checkModelCommandRefs(const Nu20File& before, const Nu20File& after);

std::vector<std::string> checkMaterialRefs(const Nu20File& before, const Nu20File& after);

}