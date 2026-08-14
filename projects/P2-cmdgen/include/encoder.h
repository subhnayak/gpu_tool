#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ir.h"
#include "isa_table.h"

namespace cmdgen {

class Encoder {
public:
    virtual ~Encoder() = default;
    virtual Architecture architecture() const = 0;
    virtual std::string name() const = 0;
    virtual std::uint32_t encodeInstruction(const Instruction& instruction) const = 0;

    virtual std::vector<std::uint32_t> encodeProgram(const Program& program) const;
};

class EncoderRegistry {
public:
    using Factory = std::function<std::unique_ptr<Encoder>()>;

    // Meyers singleton avoids static initialization order problems for the map
    // itself. Individual encoder registrations still happen during static
    // initialization, but they register into a singleton created on first use.
    static EncoderRegistry& instance();

    void registerFactory(const std::string& key, Factory factory);
    std::unique_ptr<Encoder> create(const std::string& key) const;
    std::vector<std::string> names() const;

private:
    std::map<std::string, Factory> factories_;
};

struct EncoderRegistrar {
    EncoderRegistrar(const std::string& key, EncoderRegistry::Factory factory);
};

#define REGISTER_ENCODER(CLASS_NAME, KEY) \
    namespace { \
    const ::cmdgen::EncoderRegistrar g_register_##CLASS_NAME((KEY), []() { \
        return std::make_unique<CLASS_NAME>(); \
    }); \
    }

} // namespace cmdgen
