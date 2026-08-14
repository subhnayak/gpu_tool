/*
 * assembler.cpp — Two-pass assembler implementation.
 *
 * Pass 1: Scan lines, collect labels, determine instruction sizes
 *         (most are 1 word = 4 bytes; LIMM is 2 words = 8 bytes).
 * Pass 2: Encode each instruction using the opcode table, resolving
 *         labels to PC-relative offsets for branches.
 *
 * The assembler uses the SAME opcode table as the decoder. This is
 * the key design decision that prevents encoder/decoder drift.
 */

#include "assembler.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iostream>

namespace toyisa {

// ---- Tokenizer helpers ----

static std::string to_upper(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Remove comments (everything after ; or //)
static std::string strip_comment(const std::string& line) {
    auto pos = line.find(';');
    auto pos2 = line.find("//");
    if (pos2 != std::string::npos && (pos == std::string::npos || pos2 < pos))
        pos = pos2;
    if (pos != std::string::npos)
        return line.substr(0, pos);
    return line;
}

// Split a string by commas and whitespace into tokens
static std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '\t') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

// Parse a register operand like "R5" or "r31", returns register number or -1
static int parse_register(const std::string& s) {
    std::string u = to_upper(s);
    if (u.size() < 2 || u[0] != 'R') return -1;
    try {
        int n = std::stoi(u.substr(1));
        if (n < 0 || n > 31) return -1;
        return n;
    } catch (...) {
        return -1;
    }
}

// Parse a predicate register like "P3", returns index or -1
static int parse_pred_register(const std::string& s) {
    std::string u = to_upper(s);
    if (u.size() < 2 || u[0] != 'P') return -1;
    try {
        int n = std::stoi(u.substr(1));
        if (n < 0 || n > 7) return -1;
        return n;
    } catch (...) {
        return -1;
    }
}

// Parse an immediate value (decimal or 0xHex)
static bool parse_immediate(const std::string& s, int32_t& out) {
    try {
        if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            out = static_cast<int32_t>(std::stoul(s, nullptr, 16));
        } else {
            out = static_cast<int32_t>(std::stol(s, nullptr, 10));
        }
        return true;
    } catch (...) {
        return false;
    }
}

// Parse a memory operand like "[R5+4]" or "[R5-8]" or "[R5]"
static bool parse_memory_operand(const std::string& s, int& base_reg, int32_t& offset) {
    // Expect format: [Rn], [Rn+imm], [Rn-imm]
    if (s.size() < 3 || s.front() != '[' || s.back() != ']') return false;
    std::string inner = s.substr(1, s.size() - 2);  // strip [ ]

    // Find + or - (not at position 0)
    size_t sign_pos = std::string::npos;
    for (size_t i = 1; i < inner.size(); i++) {
        if (inner[i] == '+' || inner[i] == '-') {
            sign_pos = i;
            break;
        }
    }

    if (sign_pos == std::string::npos) {
        // Just [Rn]
        base_reg = parse_register(inner);
        offset = 0;
        return base_reg >= 0;
    }

    std::string reg_part = inner.substr(0, sign_pos);
    std::string imm_part = inner.substr(sign_pos);  // includes sign
    base_reg = parse_register(reg_part);
    if (base_reg < 0) return false;
    return parse_immediate(imm_part, offset);
}

// ---- Parsed line ----
struct ParsedLine {
    int line_number;
    std::string label;       // "" if no label
    std::string predicate;   // "" if no predicate, else "@P3" or "@!P3"
    std::string mnemonic;    // Upper-cased
    std::vector<std::string> operands;
    bool is_empty;           // blank/comment-only
};

static ParsedLine parse_line(const std::string& raw_line, int line_num) {
    ParsedLine pl;
    pl.line_number = line_num;
    pl.is_empty = true;

    std::string line = trim(strip_comment(raw_line));
    if (line.empty()) return pl;
    pl.is_empty = false;

    // Check for label (ends with ':')
    auto colon_pos = line.find(':');
    if (colon_pos != std::string::npos) {
        // Ensure it's a label (no spaces before colon)
        std::string maybe_label = trim(line.substr(0, colon_pos));
        if (!maybe_label.empty() && maybe_label.find(' ') == std::string::npos) {
            pl.label = maybe_label;
            line = trim(line.substr(colon_pos + 1));
            if (line.empty()) return pl;  // Label-only line
        }
    }

    auto tokens = tokenize(line);
    if (tokens.empty()) return pl;

    size_t tok_idx = 0;

    // Check for predicate prefix: @P3 or @!P3
    if (tokens[0][0] == '@') {
        pl.predicate = tokens[0];
        tok_idx++;
    }

    if (tok_idx >= tokens.size()) return pl;

    pl.mnemonic = to_upper(tokens[tok_idx]);
    tok_idx++;

    // Extract operands from the original line text (not from pre-tokenized tokens)
    // Find where the mnemonic ends in the line and take everything after it.
    std::string rest;
    {
        // Build the prefix to skip: [predicate] mnemonic
        std::string prefix;
        if (!pl.predicate.empty()) {
            auto ppos = line.find(pl.predicate);
            if (ppos != std::string::npos) {
                prefix = line.substr(0, ppos + pl.predicate.size());
            }
        }
        // Find mnemonic in line after predicate
        size_t mnem_start = prefix.size();
        while (mnem_start < line.size() && (line[mnem_start] == ' ' || line[mnem_start] == '\t'))
            mnem_start++;
        // Skip past mnemonic
        size_t mnem_end = mnem_start;
        while (mnem_end < line.size() && line[mnem_end] != ' ' && line[mnem_end] != '\t' && line[mnem_end] != ',')
            mnem_end++;
        rest = (mnem_end < line.size()) ? trim(line.substr(mnem_end)) : "";
    }

    // Split by comma, preserving [...] groups
    if (!rest.empty()) {
        std::string current;
        int bracket_depth = 0;
        for (char c : rest) {
            if (c == '[') bracket_depth++;
            if (c == ']') bracket_depth--;
            if (c == ',' && bracket_depth == 0) {
                std::string t = trim(current);
                if (!t.empty()) pl.operands.push_back(t);
                current.clear();
            } else if (c != ' ' && c != '\t') {
                current += c;
            } else if (bracket_depth > 0) {
                // Keep spaces inside brackets? No, strip them.
            }
        }
        std::string t = trim(current);
        if (!t.empty()) pl.operands.push_back(t);
    }

    return pl;
}

// ---- Find opcode entry by mnemonic ----
static const OpcodeEntry* find_by_mnemonic(const std::string& mnemonic) {
    const auto& table = get_opcode_table();
    for (const auto& e : table) {
        if (to_upper(e.mnemonic) == mnemonic) return &e;
    }
    return nullptr;
}

// ---- assemble ----
AssemblerResult assemble(const std::string& source) {
    AssemblerResult result;

    // Split source into lines
    std::vector<std::string> lines;
    // Strip UTF-8 BOM if present
    std::string clean_source = source;
    if (clean_source.size() >= 3 &&
        static_cast<unsigned char>(clean_source[0]) == 0xEF &&
        static_cast<unsigned char>(clean_source[1]) == 0xBB &&
        static_cast<unsigned char>(clean_source[2]) == 0xBF) {
        clean_source = clean_source.substr(3);
    }
    std::istringstream iss(clean_source);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }

    // ---- Pass 1: collect labels and instruction sizes ----
    // We parse each line to determine if it has a label and an instruction.
    // Instructions are sized (1 word or 2 for LIMM).
    struct Pass1Entry {
        ParsedLine parsed;
        uint32_t address;       // Byte address of this instruction
        bool has_instruction;   // False for label-only or empty lines
        const OpcodeEntry* entry;
    };

    std::vector<Pass1Entry> entries;
    uint32_t current_address = 0;

    for (int i = 0; i < static_cast<int>(lines.size()); i++) {
        ParsedLine pl = parse_line(lines[i], i + 1);
        Pass1Entry e;
        e.parsed = pl;
        e.address = current_address;
        e.has_instruction = false;
        e.entry = nullptr;

        if (!pl.label.empty()) {
            if (result.labels.count(pl.label)) {
                result.errors.push_back({pl.line_number, "duplicate label: " + pl.label});
            } else {
                result.labels[pl.label] = current_address;
            }
        }

        if (!pl.mnemonic.empty()) {
            e.has_instruction = true;
            e.entry = find_by_mnemonic(pl.mnemonic);
            if (!e.entry) {
                result.errors.push_back({pl.line_number, "unknown mnemonic: " + pl.mnemonic});
            } else {
                current_address += e.entry->is_two_word ? 8 : 4;
            }
        }

        entries.push_back(e);
    }

    if (!result.errors.empty()) return result;

    // ---- Pass 2: encode ----
    for (const auto& e : entries) {
        if (!e.has_instruction || !e.entry) continue;

        const auto& pl = e.parsed;
        const auto* op = e.entry;
        uint32_t word = 0;
        int ln = pl.line_number;

        // Encode predicate
        int pred_reg = 0;
        bool pred_neg = false;
        if (!pl.predicate.empty()) {
            std::string p = pl.predicate;
            // Skip @
            size_t idx = 1;
            if (idx < p.size() && p[idx] == '!') {
                pred_neg = true;
                idx++;
            }
            int pr = parse_pred_register(p.substr(idx));
            if (pr < 0) {
                result.errors.push_back({ln, "invalid predicate: " + pl.predicate});
                continue;
            }
            pred_reg = pr;
        }
        encode_field(word, FIELD_PN, pred_neg ? 1 : 0);
        encode_field(word, FIELD_PRED, static_cast<uint32_t>(pred_reg));
        encode_field(word, FIELD_OPCODE, op->opcode);

        switch (op->format) {
            case InstFormat::R: {
                encode_field(word, FIELD_FUNC, op->func);

                if (op->mnemonic == "NOP" || op->mnemonic == "HALT") {
                    // No operands
                } else if (op->mnemonic == "BAR") {
                    // BAR Rn
                    if (pl.operands.size() < 1) {
                        result.errors.push_back({ln, "BAR requires 1 operand"});
                        continue;
                    }
                    int rs1 = parse_register(pl.operands[0]);
                    if (rs1 < 0) { result.errors.push_back({ln, "invalid register: " + pl.operands[0]}); continue; }
                    encode_field(word, FIELD_RS1, static_cast<uint32_t>(rs1));
                } else if (op->sets_predicate) {
                    // SETP.xx Pn, Rs1, Rs2
                    if (pl.operands.size() < 3) {
                        result.errors.push_back({ln, op->mnemonic + " requires 3 operands (Pd, Rs1, Rs2)"});
                        continue;
                    }
                    int pd = parse_pred_register(pl.operands[0]);
                    if (pd < 0) { result.errors.push_back({ln, "invalid pred register: " + pl.operands[0]}); continue; }
                    int rs1 = parse_register(pl.operands[1]);
                    if (rs1 < 0) { result.errors.push_back({ln, "invalid register: " + pl.operands[1]}); continue; }
                    int rs2 = parse_register(pl.operands[2]);
                    if (rs2 < 0) { result.errors.push_back({ln, "invalid register: " + pl.operands[2]}); continue; }
                    encode_field(word, FIELD_RD, static_cast<uint32_t>(pd));  // pred in RD field
                    encode_field(word, FIELD_RS1, static_cast<uint32_t>(rs1));
                    encode_field(word, FIELD_RS2, static_cast<uint32_t>(rs2));
                } else {
                    // Standard R-type: Rd, Rs1, Rs2
                    if (pl.operands.size() < 3) {
                        result.errors.push_back({ln, op->mnemonic + " requires 3 operands (Rd, Rs1, Rs2)"});
                        continue;
                    }
                    int rd = parse_register(pl.operands[0]);
                    if (rd < 0) { result.errors.push_back({ln, "invalid register: " + pl.operands[0]}); continue; }
                    int rs1 = parse_register(pl.operands[1]);
                    if (rs1 < 0) { result.errors.push_back({ln, "invalid register: " + pl.operands[1]}); continue; }
                    int rs2 = parse_register(pl.operands[2]);
                    if (rs2 < 0) { result.errors.push_back({ln, "invalid register: " + pl.operands[2]}); continue; }
                    encode_field(word, FIELD_RD, static_cast<uint32_t>(rd));
                    encode_field(word, FIELD_RS1, static_cast<uint32_t>(rs1));
                    encode_field(word, FIELD_RS2, static_cast<uint32_t>(rs2));
                }
                break;
            }

            case InstFormat::I: {
                bool is_load_store = (op->mnemonic == "LD" || op->mnemonic == "ST" ||
                                      op->mnemonic == "LDB" || op->mnemonic == "STB");
                if (is_load_store) {
                    // LD/ST Rd, [Rs1+imm] or LD/ST Rd, [Rs1]
                    if (pl.operands.size() < 2) {
                        result.errors.push_back({ln, op->mnemonic + " requires 2 operands (Rd, [Rs1+imm])"});
                        continue;
                    }
                    int rd = parse_register(pl.operands[0]);
                    if (rd < 0) { result.errors.push_back({ln, "invalid register: " + pl.operands[0]}); continue; }
                    int base_reg; int32_t offset;
                    if (!parse_memory_operand(pl.operands[1], base_reg, offset)) {
                        result.errors.push_back({ln, "invalid memory operand: " + pl.operands[1]});
                        continue;
                    }
                    // Check IMM12 range
                    if (offset < -2048 || offset > 2047) {
                        result.errors.push_back({ln, "immediate out of 12-bit signed range: " + std::to_string(offset)});
                        continue;
                    }
                    encode_field(word, FIELD_RD, static_cast<uint32_t>(rd));
                    encode_field(word, FIELD_RS1, static_cast<uint32_t>(base_reg));
                    encode_field(word, FIELD_IMM12, static_cast<uint32_t>(offset) & 0xFFF);
                } else {
                    // ADDI/ANDI/ORI/SLTI Rd, Rs1, imm
                    if (pl.operands.size() < 3) {
                        result.errors.push_back({ln, op->mnemonic + " requires 3 operands (Rd, Rs1, imm)"});
                        continue;
                    }
                    int rd = parse_register(pl.operands[0]);
                    if (rd < 0) { result.errors.push_back({ln, "invalid register: " + pl.operands[0]}); continue; }
                    int rs1 = parse_register(pl.operands[1]);
                    if (rs1 < 0) { result.errors.push_back({ln, "invalid register: " + pl.operands[1]}); continue; }
                    int32_t imm;
                    if (!parse_immediate(pl.operands[2], imm)) {
                        result.errors.push_back({ln, "invalid immediate: " + pl.operands[2]});
                        continue;
                    }
                    if (op->imm_is_unsigned) {
                        if (imm < 0 || imm > 4095) {
                            result.errors.push_back({ln, "immediate out of 12-bit unsigned range"});
                            continue;
                        }
                    } else {
                        if (imm < -2048 || imm > 2047) {
                            result.errors.push_back({ln, "immediate out of 12-bit signed range"});
                            continue;
                        }
                    }
                    encode_field(word, FIELD_RD, static_cast<uint32_t>(rd));
                    encode_field(word, FIELD_RS1, static_cast<uint32_t>(rs1));
                    encode_field(word, FIELD_IMM12, static_cast<uint32_t>(imm) & 0xFFF);
                }
                break;
            }

            case InstFormat::B: {
                if (op->is_return) {
                    // RET — no operands needed
                    // (RS1 and offset are don't-care, encode as 0)
                } else {
                    // BRA target | BEZ Rs1, target | BNZ Rs1, target | CALL target
                    size_t expected = (op->mnemonic == "BRA" || op->mnemonic == "CALL") ? 1 : 2;
                    if (pl.operands.size() < expected) {
                        result.errors.push_back({ln, op->mnemonic + " requires " + std::to_string(expected) + " operand(s)"});
                        continue;
                    }

                    size_t target_idx = expected - 1;
                    if (expected == 2) {
                        int rs1 = parse_register(pl.operands[0]);
                        if (rs1 < 0) { result.errors.push_back({ln, "invalid register: " + pl.operands[0]}); continue; }
                        encode_field(word, FIELD_RS1_B, static_cast<uint32_t>(rs1));
                    }

                    // Target is a label or an absolute address
                    std::string target_str = pl.operands[target_idx];
                    int32_t offset_words;
                    int32_t abs_imm;

                    if (parse_immediate(target_str, abs_imm)) {
                        // Numeric absolute address
                        int32_t byte_diff = abs_imm - static_cast<int32_t>(e.address + 4);
                        offset_words = byte_diff / 4;
                    } else {
                        // Label reference
                        auto it = result.labels.find(target_str);
                        if (it == result.labels.end()) {
                            result.errors.push_back({ln, "undefined label: " + target_str});
                            continue;
                        }
                        int32_t byte_diff = static_cast<int32_t>(it->second) - static_cast<int32_t>(e.address + 4);
                        offset_words = byte_diff / 4;
                    }

                    // Check 17-bit signed range
                    if (offset_words < -65536 || offset_words > 65535) {
                        result.errors.push_back({ln, "branch offset out of 17-bit signed range"});
                        continue;
                    }
                    encode_field(word, FIELD_OFFSET17, static_cast<uint32_t>(offset_words) & 0x1FFFF);
                }
                break;
            }

            case InstFormat::L: {
                // LIMM Rd, imm32
                if (pl.operands.size() < 2) {
                    result.errors.push_back({ln, "LIMM requires 2 operands (Rd, imm32)"});
                    continue;
                }
                int rd = parse_register(pl.operands[0]);
                if (rd < 0) { result.errors.push_back({ln, "invalid register: " + pl.operands[0]}); continue; }
                int32_t imm;
                if (!parse_immediate(pl.operands[1], imm)) {
                    result.errors.push_back({ln, "invalid immediate: " + pl.operands[1]});
                    continue;
                }
                encode_field(word, FIELD_RD, static_cast<uint32_t>(rd));
                // IMM_HI gets upper bits but for LIMM we put rd and the full imm in word2
                // Word 0: opcode + rd + imm_hi (17 bits, but we ignore for 32-bit imm)
                // Word 1: full 32-bit immediate
                // For simplicity, imm_hi = 0, word2 = full immediate
                result.words.push_back(word);
                result.words.push_back(static_cast<uint32_t>(imm));
                continue;  // Skip the push_back below
            }
        }

        result.words.push_back(word);
    }

    return result;
}

} // namespace toyisa
