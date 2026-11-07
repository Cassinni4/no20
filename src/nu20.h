#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace no20 {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Chunk {
    std::string tag;
    uint32_t offset = 0;
    uint32_t size = 0;
    int depth = 0;
    uint32_t end() const { return offset + size; }
};

enum class PtrKind { Zero, InFile, Sentinel };

struct Pointer {
    uint32_t field;
    int32_t value;
    int64_t target;
    PtrKind kind;
};

struct Texture {
    uint32_t slot;
    uint32_t descriptor;
    uint32_t width, height, size;
    uint32_t data = 0;
};

struct GeomBuffer {
    uint32_t sizeField;
    uint32_t size;
};

// nu20 first
// [u32 size][dds data][u16 n][n x vb][u16 m][m x ib][trailer]
// texture first
// [u32 size][u16 t][t x (0x18 hdr, data)][u16 n][n x vb][u16 m][m x ib][0 pad]
struct Package {
    bool parsed = false;
    bool textureFirst = false;
    uint32_t sizeWord = 0;
    uint32_t texturesEnd = 0;
    std::vector<GeomBuffer> vertexBuffers, indexBuffers;
    uint32_t end = 0;
    std::string error;
};

struct ExtraPointer {
    uint32_t field;
    uint8_t width;
    int32_t constant;
    const char* what;
};

class Nu20File {
public:
    std::vector<uint8_t> data;
    std::vector<ExtraPointer> extras;

    uint32_t base = 0;
    uint32_t version = 0;
    uint32_t pntr = 0;
    uint32_t package = 0;
    uint32_t sceneRoot = 0;
    uint32_t hdr(uint32_t o) const { return base + o; }
    uint32_t sceneStart() const { return base + 0x20; }
    std::vector<Chunk> chunks;
    std::vector<uint32_t> relocs;

    static Nu20File load(const std::string& path);
    void save(const std::string& path) const;
    void parse();
    void rebuildPntr();

    uint32_t u32(uint32_t o) const { check(o, 4); uint32_t v; std::memcpy(&v, &data[o], 4); return v; }
    int32_t s32(uint32_t o) const { return (int32_t)u32(o); }
    uint16_t u16(uint32_t o) const { check(o, 2); uint16_t v; std::memcpy(&v, &data[o], 2); return v; }
    void put32(uint32_t o, uint32_t v) { check(o, 4); std::memcpy(&data[o], &v, 4); }
    std::string tagAt(uint32_t o) const { check(o, 4); return std::string((const char*)&data[o], 4); }
    uint32_t ptr(uint32_t field) const { return (uint32_t)((int64_t)field + s32(field)); }
    uint32_t follow(uint32_t field) const { return s32(field) ? ptr(field) : 0; }

    Pointer pointerAt(uint32_t field) const;
    std::vector<Pointer> pointers() const;
    int64_t extraTarget(const ExtraPointer& e) const;
    void setExtraTarget(const ExtraPointer& e, int64_t target);

    uint32_t gsnh() const { return sceneRoot + 4; }
    bool isCharacterModel() const;

    uint32_t gsnhPayload() const { return sceneRoot; }
    std::vector<Texture> textures() const;
    Package parsePackage() const;

    struct Range { uint32_t start, end; const char* what; };
    std::vector<Range> indexedLists() const;
    bool insideIndexedList(uint32_t pos) const;

    const Chunk* chunkContaining(uint32_t pos) const;
    const Chunk* findChunk(const std::string& tag, int nth = 0) const;
    std::string describe(int64_t pos) const;

private:
    void check(uint32_t o, uint32_t n) const {
        if ((uint64_t)o + n > data.size()) throw Error("read out of bounds at " + std::to_string(o));
    }
    void parseChunks();
    void findExtras();
};

bool plausibleTag(const uint8_t* p);
std::string hex(int64_t v);

}