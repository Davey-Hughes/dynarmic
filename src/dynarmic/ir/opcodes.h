/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <string>

#include <fmt/format.h>
#include <mcl/stdint.hpp>

namespace Dynarmic::IR {

enum class Type;

// <winnt.h> defines RotateRight32 and RotateRight64 as macros, and MinGW's <intrin.h> later changes
// what RotateRight64 expands to. Drop them so the enum and later Opcode:: uses name the same opcode.
#undef RotateRight32
#undef RotateRight64

/**
 * The Opcodes of our intermediate representation.
 * Type signatures for each opcode can be found in opcodes.inc
 */
enum class Opcode {
#define OPCODE(name, type, ...) name,
#define A32OPC(name, type, ...) A32##name,
#define A64OPC(name, type, ...) A64##name,
#include "./opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC
    NUM_OPCODE
};

constexpr size_t OpcodeCount = static_cast<size_t>(Opcode::NUM_OPCODE);

namespace OpcodeInfo {

// The return type and the number of arguments of every opcode, as flat tables for the two inline lookups below.
extern const std::array<Type, OpcodeCount> types;
extern const std::array<u8, OpcodeCount> arg_counts;

}  // namespace OpcodeInfo

/// Get return type of an opcode
inline Type GetTypeOf(Opcode op) {
    return OpcodeInfo::types.at(static_cast<size_t>(op));
}

/// Get the number of arguments an opcode accepts
inline size_t GetNumArgsOf(Opcode op) {
    return OpcodeInfo::arg_counts.at(static_cast<size_t>(op));
}

/// Get the required type of an argument of an opcode
Type GetArgTypeOf(Opcode op, size_t arg_index);

/// Get the name of an opcode.
std::string GetNameOf(Opcode op);

}  // namespace Dynarmic::IR

template<>
struct fmt::formatter<Dynarmic::IR::Opcode> : fmt::formatter<std::string> {
    template<typename FormatContext>
    auto format(Dynarmic::IR::Opcode op, FormatContext& ctx) const {
        return formatter<std::string>::format(Dynarmic::IR::GetNameOf(op), ctx);
    }
};
