#pragma once
#include <string>
#include <vector>
#include "nu20.h"

namespace no20 {

struct Report {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::vector<std::string> notes;
    bool ok() const { return errors.empty(); }
};

Report verify(const Nu20File& f);

std::vector<std::string> checkIndexedLists(const Nu20File& before, const Nu20File& after);

}