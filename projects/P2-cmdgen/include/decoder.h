#pragma once

#include <cstdint>
#include <vector>

#include "ir.h"
#include "isa_table.h"

namespace cmdgen {

class Decoder {
public:
    Instruction decodeInstruction(Architecture architecture, std::uint32_t encoded_word) const;
    Program decodeProgram(Architecture architecture, const std::vector<std::uint32_t>& words) const;
};

} // namespace cmdgen
