#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace no20 {

struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba; // w*h*4, top row first
};

Image loadImage(const std::string& path);

std::vector<uint8_t> encodeDds(const Image& img, uint32_t maxSize = 2048, std::string* report = nullptr);
std::vector<uint8_t> textureBytes(const std::string& path, std::string* report = nullptr);

std::vector<uint8_t> encodeCubeDds(const std::vector<Image>& faces, uint32_t maxSize = 512, std::string* report = nullptr);

std::vector<uint8_t> cubeTextureBytes(const std::string& path, std::string* report = nullptr);

}