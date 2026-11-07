#pragma once
#include "nu20.h"

namespace no20 {

struct InsertOptions {
    bool targetAtPointMoves = true;
    bool growChunks = true;
    bool allowInsideIndexedList = false;
};

void insertBytes(Nu20File& f, uint32_t at, uint32_t n, const uint8_t* fill = nullptr, const InsertOptions& opt = {});
void removeBytes(Nu20File& f, uint32_t at, uint32_t n, const InsertOptions& opt = {});
void addReloc(Nu20File& f, uint32_t field);

void writePointer(Nu20File& f, uint32_t field, uint32_t target, bool registerReloc = true);

}