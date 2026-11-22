/* This file is part of the dynarmic project.
 * SPDX-License-Identifier: 0BSD
 */

#include <array>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <mcl/stdint.hpp>

#include "./testenv.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/translate/a32_translate.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/opt/passes.h"

using namespace Dynarmic;

namespace {

class ReadOnlyTestEnv final : public ArmTestEnv {
public:
    bool IsReadOnlyMemory(u32) override { return true; }
};

// What A32ConstantMemoryReads() did before it limited itself to the reads the frontend records:
// look at every instruction. Returns the number of reads replaced.
size_t ConstantMemoryReadsFullWalk(IR::Block& block, A32::UserCallbacks* cb) {
    size_t replaced = 0;
    for (auto& inst : block) {
        const IR::Opcode op = inst.GetOpcode();
        if (op != IR::Opcode::A32ReadMemory8 && op != IR::Opcode::A32ReadMemory16 && op != IR::Opcode::A32ReadMemory32 && op != IR::Opcode::A32ReadMemory64) {
            continue;
        }
        if (!inst.AreAllArgsImmediates()) {
            continue;
        }
        const u32 vaddr = inst.GetArg(1).GetU32();
        if (!cb->IsReadOnlyMemory(vaddr)) {
            continue;
        }
        switch (op) {
        case IR::Opcode::A32ReadMemory8:
            inst.ReplaceUsesWith(IR::Value{cb->MemoryRead8(vaddr)});
            break;
        case IR::Opcode::A32ReadMemory16:
            inst.ReplaceUsesWith(IR::Value{cb->MemoryRead16(vaddr)});
            break;
        case IR::Opcode::A32ReadMemory32:
            inst.ReplaceUsesWith(IR::Value{cb->MemoryRead32(vaddr)});
            break;
        default:
            inst.ReplaceUsesWith(IR::Value{cb->MemoryRead64(vaddr)});
            break;
        }
        ++replaced;
    }
    return replaced;
}

// IR::DumpBlock() without each line's instruction address.
std::string Dump(IR::Block& block) {
    Optimization::NamingPass(block);
    const std::string text = IR::DumpBlock(block);
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        end = end == std::string::npos ? text.size() : end + 1;
        const size_t start = text[pos] == '[' ? pos + 19 : pos;
        out.append(text, start, end - start);
        pos = end;
    }
    return out;
}

}  // namespace

TEST_CASE("A32ConstantMemoryReads replaces the reads a full walk replaces", "[a32][opt]") {
    // Register writes with immediates and reads whose address is an immediate, a register or
    // neither, before and after the other reads.
    constexpr std::array<u32, 14> templates{
        0xE3A00000,  // MOV r0, #imm8
        0xE3A01000,  // MOV r1, #imm8
        0xE59F0000,  // LDR r0, [PC, #imm12]
        0xE5DF1000,  // LDRB r1, [PC, #imm12]
        0xE1DF00B0,  // LDRH r0, [PC, #imm8]
        0xE1CF20D0,  // LDRD r2, r3, [PC, #imm8]
        0xED9F0B00,  // VLDR d0, [PC, #imm8]
        0xE8900006,  // LDM r0, {r1, r2}
        0xE4910004,  // LDR r0, [r1], #4
        0xE5910000,  // LDR r0, [r1]
        0xE5910004,  // LDR r0, [r1, #4]
        0xE1A01000,  // MOV r1, r0
        0xE0800001,  // ADD r0, r0, r1
        0xECB00B02,  // VLDMIA r0!, {d0}
    };

    std::mt19937 rng{0xc0de};
    ReadOnlyTestEnv env;
    size_t replaced = 0;
    for (int i = 0; i < 2000; ++i) {
        env.code_mem.clear();
        const size_t count = 1 + rng() % 12;
        for (size_t j = 0; j < count; ++j) {
            const u32 word = templates[rng() % templates.size()];
            env.code_mem.push_back(word | (rng() & 0x3C));  // vary the immediates
        }
        env.code_mem.push_back(0xEAFFFFFE);  // B .

        const auto prepare = [&] {
            IR::Block block = A32::Translate(A32::LocationDescriptor{0, A32::PSR{}, A32::FPSCR{}}, &env, {});
            Optimization::NamingPass(block);
            Optimization::A32GetSetElimination(block, {.convert_nzc_to_nz = true});
            Optimization::DeadCodeElimination(block);
            return block;
        };
        IR::Block expected = prepare();
        replaced += ConstantMemoryReadsFullWalk(expected, &env);
        IR::Block got = prepare();
        Optimization::A32ConstantMemoryReads(got, &env);

        const std::string expected_dump = Dump(expected);
        const std::string got_dump = Dump(got);
        INFO(expected_dump);
        REQUIRE(got_dump == expected_dump);
    }
    CHECK(replaced > 0);
}
