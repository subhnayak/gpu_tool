#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoder.h"
#include "encoder.h"
#include "isa_table.h"
#include "random_gen.h"
#include "visitor.h"

namespace cmdgen {
namespace {

constexpr std::uint32_t kFileMagic = 0x434D4447U; // 'CMDG'
constexpr std::uint32_t kFileVersion = 1U;

struct FileImage {
    Architecture architecture = Architecture::ARCH_A;
    std::vector<std::uint32_t> words;
};

void writeU32LE(std::ostream& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.put(static_cast<char>((value >> shift) & 0xFFU));
    }
}

std::uint32_t readU32LE(std::istream& in) {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const int byte = in.get();
        if (byte == std::char_traits<char>::eof()) {
            throw std::runtime_error("Unexpected end of file");
        }
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(byte)) << shift;
    }
    return value;
}

void writeFile(const std::string& path, const FileImage& image) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Failed to open output file: " + path);
    }

    writeU32LE(out, kFileMagic);
    writeU32LE(out, kFileVersion);
    writeU32LE(out, image.architecture == Architecture::ARCH_A ? 0U : 1U);
    writeU32LE(out, static_cast<std::uint32_t>(image.words.size()));
    for (std::uint32_t word : image.words) {
        writeU32LE(out, word);
    }
}

FileImage readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to open input file: " + path);
    }

    const std::uint32_t magic = readU32LE(in);
    if (magic != kFileMagic) {
        throw std::runtime_error("Bad file magic");
    }

    const std::uint32_t version = readU32LE(in);
    if (version != kFileVersion) {
        throw std::runtime_error("Unsupported file version");
    }

    const std::uint32_t arch_id = readU32LE(in);
    if (arch_id > 1U) {
        throw std::runtime_error("Unsupported architecture id in file");
    }
    const std::uint32_t count = readU32LE(in);

    FileImage image;
    image.architecture = arch_id == 0U ? Architecture::ARCH_A : Architecture::ARCH_B;
    image.words.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        image.words.push_back(readU32LE(in));
    }
    return image;
}

void printUsage(std::ostream& out) {
    out << "Usage:\n"
        << "  cmdgen gen --seed N --count M --arch A|B --out file [--dry-run]\n"
        << "  cmdgen disasm file\n"
        << "  cmdgen stats file\n";
}

std::string requireValue(int& index, int argc, char** argv) {
    if (index + 1 >= argc) {
        throw std::runtime_error("Missing value for argument: " + std::string(argv[index]));
    }
    ++index;
    return argv[index];
}

int runGen(int argc, char** argv) {
    std::uint64_t seed = 1;
    std::size_t count = 16;
    Architecture architecture = Architecture::ARCH_A;
    std::string out_file;
    bool dry_run = false;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seed") {
            seed = static_cast<std::uint64_t>(std::stoull(requireValue(i, argc, argv)));
        } else if (arg == "--count") {
            count = static_cast<std::size_t>(std::stoull(requireValue(i, argc, argv)));
        } else if (arg == "--arch") {
            Architecture parsed = Architecture::ARCH_A;
            if (!parseArchitecture(requireValue(i, argc, argv), parsed)) {
                throw std::runtime_error("Unknown architecture");
            }
            architecture = parsed;
        } else if (arg == "--out") {
            out_file = requireValue(i, argc, argv);
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (!dry_run && out_file.empty()) {
        throw std::runtime_error("gen requires --out unless --dry-run is used");
    }

    RandomGeneratorConfig config;
    config.master_seed = seed;
    config.instruction_count = count;
    config.block_count = count < 8 ? 1U : 4U;
    config.register_count = 8U;

    RandomProgramGenerator generator(config);
    const Program program = generator.generate();
    auto encoder = EncoderRegistry::instance().create(architectureName(architecture));
    if (!encoder) {
        throw std::runtime_error("Requested encoder is not registered");
    }

    const std::vector<std::uint32_t> words = encoder->encodeProgram(program);
    const FileImage image{architecture, words};

    if (!dry_run) {
        writeFile(out_file, image);
    }

    std::cout << "Generated " << words.size() << " words for ARCH_" << architectureName(architecture)
              << " with seed " << seed;
    if (!dry_run) {
        std::cout << " -> " << out_file;
    }
    std::cout << "\n";

    if (dry_run) {
        PrinterVisitor printer(std::cout);
        walkProgram(program, printer);
    }

    return 0;
}

int runDisasm(int argc, char** argv) {
    if (argc != 3) {
        throw std::runtime_error("disasm expects exactly one file path");
    }

    const FileImage image = readFile(argv[2]);
    Decoder decoder;
    const Program program = decoder.decodeProgram(image.architecture, image.words);
    PrinterVisitor printer(std::cout);
    walkProgram(program, printer);
    return 0;
}

int runStats(int argc, char** argv) {
    if (argc != 3) {
        throw std::runtime_error("stats expects exactly one file path");
    }

    const FileImage image = readFile(argv[2]);
    Decoder decoder;
    const Program program = decoder.decodeProgram(image.architecture, image.words);
    StatsVisitor stats;
    walkProgram(program, stats);
    stats.report(std::cout);
    return 0;
}

} // namespace
} // namespace cmdgen

int main(int argc, char** argv) {
    using namespace cmdgen;

    try {
        if (argc < 2) {
            printUsage(std::cerr);
            return 2;
        }

        const std::string command = argv[1];
        if (command == "gen") {
            return runGen(argc, argv);
        }
        if (command == "disasm") {
            return runDisasm(argc, argv);
        }
        if (command == "stats") {
            return runStats(argc, argv);
        }

        std::cerr << "Unknown command: " << command << "\n";
        printUsage(std::cerr);
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
