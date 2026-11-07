#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "mesh.h"
#include "nu20.h"

namespace no20 {

struct LayerInfo {
    std::string name;
    std::vector<uint32_t> skinned;
    std::vector<std::pair<uint32_t, int>> rigid; // (model, bone)
};
std::vector<LayerInfo> characterLayers(const Nu20File& f);

uint32_t addLayer(Nu20File& f, const std::string& name, const Model& m, const ImportOptions& opt, std::string* report = nullptr);

}