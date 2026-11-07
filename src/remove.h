#pragma once
#include <cstdint>
#include <string>
#include "nu20.h"

namespace no20 {

uint32_t removeAnimations(Nu20File& f, uint32_t material, std::string* report = nullptr);
void removeLayer(Nu20File& f, uint32_t layer, std::string* report = nullptr);
void removeModelParts(Nu20File& f, uint32_t model, int64_t part, std::string* report = nullptr);
void removeMaterial(Nu20File& f, uint32_t material, std::string* report = nullptr);
uint32_t removeTexture(Nu20File& f, uint32_t texture, std::string* report = nullptr);

}