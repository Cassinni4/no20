#include "image.h"
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include "nu20.h"
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace no20 {

static void wicCheck(HRESULT hr, const std::string& what) {
    if (!FAILED(hr)) return;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08lX", (unsigned long)hr);
    throw Error(what + " (WIC error 0x" + buf + ")");
}

static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::vector<uint8_t> fileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cant open " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

static void wr32(std::vector<uint8_t>& v, size_t at, uint32_t x) { std::memcpy(&v[at], &x, 4); }

static const uint8_t* pixel(const Image& img, uint32_t x, uint32_t y) {
    return &img.rgba[((size_t)y * img.width + x) * 4];
}

static const char* dxtName(bool alpha) { return alpha ? "DXT5" : "DXT1"; }

static bool hasAlpha(const Image& img) {
    for (size_t i = 3; i < img.rgba.size(); i += 4)
        if (img.rgba[i] != 255) return true;
    return false;
}

// closest power of 2, never under 4 or over maxSize
static uint32_t nearestPow2(uint32_t v, uint32_t maxSize) {
    uint32_t p = 1;
    while (p < v && p < maxSize) p <<= 1;
    if (p > v && p / 2 >= 4 && v - p / 2 < p - v) p /= 2;
    return std::max(4u, std::min(p, maxSize));
}

static uint32_t mipCount(uint32_t w, uint32_t h) {
    uint32_t mips = 1;
    while (w > 1 || h > 1) {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
        mips++;
    }
    return mips;
}

static Image resize(const Image& src, uint32_t w, uint32_t h) {
    if (src.width == w && src.height == h) return src;
    Image out;
    out.width = w;
    out.height = h;
    out.rgba.resize((size_t)w * h * 4);
    uint8_t* dst = out.rgba.data();
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            float fx = std::max(0.0f, (x + 0.5f) * src.width / w - 0.5f);
            float fy = std::max(0.0f, (y + 0.5f) * src.height / h - 0.5f);
            uint32_t x0 = std::min((uint32_t)fx, src.width - 1);
            uint32_t y0 = std::min((uint32_t)fy, src.height - 1);
            uint32_t x1 = std::min(x0 + 1, src.width - 1);
            uint32_t y1 = std::min(y0 + 1, src.height - 1);
            float ax = fx - x0;
            float ay = fy - y0;
            const uint8_t* tl = pixel(src, x0, y0);
            const uint8_t* tr = pixel(src, x1, y0);
            const uint8_t* bl = pixel(src, x0, y1);
            const uint8_t* br = pixel(src, x1, y1);
            for (int c = 0; c < 4; c++) {
                float top = tl[c] * (1 - ax) + tr[c] * ax;
                float bottom = bl[c] * (1 - ax) + br[c] * ax;
                *dst++ = (uint8_t)std::lround(top * (1 - ay) + bottom * ay);
            }
        }
    }
    return out;
}

static Image halve(const Image& src) {
    Image out;
    out.width = std::max(1u, src.width / 2);
    out.height = std::max(1u, src.height / 2);
    out.rgba.resize((size_t)out.width * out.height * 4);
    uint8_t* dst = out.rgba.data();
    for (uint32_t y = 0; y < out.height; y++) {
        for (uint32_t x = 0; x < out.width; x++) {
            uint32_t x1 = std::min(x * 2 + 1, src.width - 1);
            uint32_t y1 = std::min(y * 2 + 1, src.height - 1);
            const uint8_t* tl = pixel(src, x * 2, y * 2);
            const uint8_t* tr = pixel(src, x1, y * 2);
            const uint8_t* bl = pixel(src, x * 2, y1);
            const uint8_t* br = pixel(src, x1, y1);
            for (int c = 0; c < 4; c++) *dst++ = (uint8_t)((tl[c] + tr[c] + bl[c] + br[c] + 2) / 4);
        }
    }
    return out;
}

static int quantize(float v, int bits) {
    int top = (1 << bits) - 1;
    return std::clamp((int)std::lround(v / 255.0f * top), 0, top);
}

static uint16_t to565(const float c[3]) {
    return (uint16_t)(quantize(c[0], 5) << 11 | quantize(c[1], 6) << 5 | quantize(c[2], 5));
}

static void from565(uint16_t v, float c[3]) {
    c[0] = ((v >> 11) & 31) * 255.0f / 31;
    c[1] = ((v >> 5) & 63) * 255.0f / 63;
    c[2] = (v & 31) * 255.0f / 31;
}

static void colourBlock(const uint8_t px[16][4], uint8_t* out) {
    float mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) mean[c] += px[i][c] / 16.0f;

    float xx = 0, xy = 0, xz = 0, yy = 0, yz = 0, zz = 0;
    for (int i = 0; i < 16; i++) {
        float dx = px[i][0] - mean[0], dy = px[i][1] - mean[1], dz = px[i][2] - mean[2];
        xx += dx * dx;
        xy += dx * dy;
        xz += dx * dz;
        yy += dy * dy;
        yz += dy * dz;
        zz += dz * dz;
    }

    float axis[3] = {1, 1, 1};
    for (int it = 0; it < 8; it++) {
        float ax = xx * axis[0] + xy * axis[1] + xz * axis[2];
        float ay = xy * axis[0] + yy * axis[1] + yz * axis[2];
        float az = xz * axis[0] + yz * axis[1] + zz * axis[2];
        float len = std::sqrt(ax * ax + ay * ay + az * az);
        if (len < 1e-6f) break;
        axis[0] = ax / len;
        axis[1] = ay / len;
        axis[2] = az / len;
    }

    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 16; i++) {
        float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }

    float e0[3], e1[3];
    for (int c = 0; c < 3; c++) {
        e0[c] = std::clamp(mean[c] + axis[c] * hi, 0.0f, 255.0f);
        e1[c] = std::clamp(mean[c] + axis[c] * lo, 0.0f, 255.0f);
    }
    uint16_t c0 = to565(e0), c1 = to565(e1);
    if (c0 < c1) std::swap(c0, c1);  // c0 > c1 = 4 color mode

    float pal[4][3];
    from565(c0, pal[0]);
    from565(c1, pal[1]);
    for (int c = 0; c < 3; c++) {
        pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
        pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
    }

    uint32_t idx = 0;
    if (c0 != c1) {
        for (int i = 0; i < 16; i++) {
            int best = 0;
            float bestDist = 1e18f;
            for (int k = 0; k < 4; k++) {
                float dist = 0;
                for (int c = 0; c < 3; c++) dist += (px[i][c] - pal[k][c]) * (px[i][c] - pal[k][c]);
                if (dist < bestDist) {
                    bestDist = dist;
                    best = k;
                }
            }
            idx |= (uint32_t)best << (2 * i);
        }
    }
    std::memcpy(out, &c0, 2);
    std::memcpy(out + 2, &c1, 2);
    std::memcpy(out + 4, &idx, 4);
}

static void alphaBlock(const uint8_t px[16][4], uint8_t* out) {
    uint8_t a0 = 0, a1 = 255;
    for (int i = 0; i < 16; i++) {
        a0 = std::max(a0, px[i][3]);
        a1 = std::min(a1, px[i][3]);
    }
    float pal[8];
    pal[0] = a0;
    pal[1] = a1;
    for (int k = 1; k < 7; k++) pal[k + 1] = ((7 - k) * a0 + k * a1) / 7.0f;

    uint64_t bits = 0;
    if (a0 != a1) {
        for (int i = 0; i < 16; i++) {
            int best = 0;
            float bestDist = 1e9f;
            for (int k = 0; k < 8; k++) {
                float dist = std::fabs(px[i][3] - pal[k]);
                if (dist < bestDist) {
                    bestDist = dist;
                    best = k;
                }
            }
            bits |= (uint64_t)best << (3 * i);
        }
    }
    out[0] = a0;
    out[1] = a1;
    for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(bits >> (8 * k));
}

static std::vector<uint8_t> ddsHeader(uint32_t w, uint32_t h, uint32_t mips, bool alpha, bool cube) {
    std::vector<uint8_t> out(128, 0);
    std::memcpy(&out[0], "DDS ", 4);
    wr32(out, 0x04, 124);
    wr32(out, 0x08, 0x000A1007); //CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT|LINEARSIZE
    wr32(out, 0x0C, h);
    wr32(out, 0x10, w);
    wr32(out, 0x14, ((w + 3) / 4) * ((h + 3) / 4) * (alpha ? 16 : 8));
    wr32(out, 0x1C, mips);
    wr32(out, 0x4C, 32);
    wr32(out, 0x50, 0x4);//DDPF_FOURCC
    std::memcpy(&out[0x54], dxtName(alpha), 4);
    wr32(out, 0x6C, 0x401008); // COMPLEX|TEXTURE|MIPMAP
    if (cube) wr32(out, 0x70, 0xFE00); // CUBEMAP + all 6 faces
    return out;
}

static void appendMips(std::vector<uint8_t>& out, Image level, uint32_t mips, bool alpha) {
    uint32_t blockBytes = alpha ? 16 : 8;
    for (uint32_t m = 0; m < mips; m++) {
        uint32_t blocksHigh = std::max(1u, (level.height + 3) / 4);
        uint32_t blocksWide = std::max(1u, (level.width + 3) / 4);
        for (uint32_t by = 0; by < blocksHigh; by++) {
            for (uint32_t bx = 0; bx < blocksWide; bx++) {
                uint8_t px[16][4];
                for (int i = 0; i < 16; i++) {
                    uint32_t x = std::min(bx * 4 + i % 4, level.width - 1);
                    uint32_t y = std::min(by * 4 + i / 4, level.height - 1);
                    std::memcpy(px[i], pixel(level, x, y), 4);
                }
                uint8_t block[16];
                if (alpha) {
                    alphaBlock(px, block);
                    colourBlock(px, block + 8);
                } else {
                    colourBlock(px, block);
                }
                out.insert(out.end(), block, block + blockBytes);
            }
        }
        if (m + 1 < mips) level = halve(level);
    }
}

Image loadImage(const std::string& path) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Image img;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> conv;
        wicCheck(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "cant start Windows Imaging Component");
        wicCheck(factory->CreateDecoderFromFilename(widen(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "cant read image " + path);
        wicCheck(decoder->GetFrame(0, &frame), "cant read image " + path);
        wicCheck(factory->CreateFormatConverter(&conv), "cant convert image " + path);
        wicCheck(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "cant convert image " + path);
        UINT w = 0, h = 0;
        wicCheck(conv->GetSize(&w, &h), "cant get the size of " + path);
        if (!w || !h || w > 16384 || h > 16384) throw Error("image " + path + " is 0 or bigger than 16384 px");
        img.width = w;
        img.height = h;
        img.rgba.resize((size_t)w * h * 4);
        wicCheck(conv->CopyPixels(nullptr, w * 4, (UINT)img.rgba.size(), img.rgba.data()), "cant decode image " + path);
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return img;
}

std::vector<uint8_t> encodeDds(const Image& src, uint32_t maxSize, std::string* report) {
    bool alpha = hasAlpha(src);
    Image level = resize(src, nearestPow2(src.width, maxSize), nearestPow2(src.height, maxSize));
    uint32_t mips = mipCount(level.width, level.height);
    std::vector<uint8_t> out = ddsHeader(level.width, level.height, mips, alpha, false);
    if (report) *report = std::to_string(src.width) + "x" + std::to_string(src.height) + " -> " + std::to_string(level.width) + "x" + std::to_string(level.height) + " " + dxtName(alpha) + ", " + std::to_string(mips) + " mips";
    appendMips(out, std::move(level), mips, alpha);
    return out;
}

std::vector<uint8_t> encodeCubeDds(const std::vector<Image>& faces, uint32_t maxSize, std::string* report) {
    if (faces.size() != 6) throw Error("cubemap needs 6 faces");
    bool alpha = false;
    for (auto& face : faces)
        if (hasAlpha(face)) alpha = true;
    uint32_t size = nearestPow2(faces[0].width, maxSize);
    uint32_t mips = mipCount(size, size);
    std::vector<uint8_t> out = ddsHeader(size, size, mips, alpha, true);
    for (auto& face : faces) appendMips(out, resize(face, size, size), mips, alpha);
    if (report) *report = "6 faces -> " + std::to_string(size) + "x" + std::to_string(size) + " " + dxtName(alpha) + " cubemap, " + std::to_string(mips) + " mips";
    return out;
}

std::vector<uint8_t> textureBytes(const std::string& path, std::string* report) {
    size_t dot = path.find_last_of('.');
    if (lower(path.substr(dot == std::string::npos ? path.size() : dot)) == ".dds") return fileBytes(path);
    std::string how;
    auto dds = encodeDds(loadImage(path), 2048, &how);
    if (report) *report = path + " converted: " + how;
    return dds;
}

std::vector<uint8_t> cubeTextureBytes(const std::string& path, std::string* report) {
    size_t slash = path.find_last_of("/\\"), dot = path.find_last_of('.');
    size_t stemEnd = dot == std::string::npos || (slash != std::string::npos && dot < slash) ? path.size() : dot;
    std::string ext = path.substr(stemEnd);
    if (lower(ext) == ".dds") return fileBytes(path);

    static const char* sets[3][6] = { {"px", "nx", "py", "ny", "pz", "nz"}, {"posx", "negx", "posy", "negy", "posz", "negz"}, {"right", "left", "top", "bottom", "front", "back"}, };
    std::string stem = path.substr(0, stemEnd);
    size_t us = stem.find_last_of('_');
    if (us != std::string::npos && (slash == std::string::npos || us > slash)) {
        std::string base = stem.substr(0, us + 1);
        std::string suffix = lower(stem.substr(us + 1));
        for (auto& set : sets) {
            bool match = false;
            for (const char* s : set)
                if (suffix == s) match = true;
            if (!match) continue;
            std::vector<Image> faces;
            for (const char* s : set) faces.push_back(loadImage(base + s + ext));
            std::string how;
            auto dds = encodeCubeDds(faces, 512, &how);
            if (report) *report = base + "{" + set[0] + ".." + set[5] + "}" + ext + " converted: " + how;
            return dds;
        }
    }
    throw Error("cubemap " + path + ": use a cubemap .dds or one of 6 face images named <name>_px..<name>_nz, _posx.._negz or _right/_left/_top/_bottom/_front/_back");
}

}