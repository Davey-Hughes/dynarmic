/* This file is part of the dynarmic project.
 * SPDX-License-Identifier: 0BSD
 */

#include <array>
#include <string>
#include <utility>
#include <vector>

#include <boost/variant/get.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <mcl/stdint.hpp>

#include "./testenv.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/translate/a32_translate.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/terminal.h"
#include "rand_int.h"

using namespace Dynarmic;

namespace {

constexpr u32 user_mode_arm = 0x000001d0;

IR::Block TranslateArmBlock(ArmTestEnv& env, bool fold) {
    A32::TranslationOptions options{};
    options.arch_version = A32::ArchVersion::v8;
    options.fold_conditional_branches = fold;
    return A32::Translate(A32::LocationDescriptor{0, A32::PSR{user_mode_arm}, A32::FPSCR{}}, &env, options);
}

u32 PcOf(const IR::LocationDescriptor& location) {
    return A32::LocationDescriptor{location}.PC();
}

// Mostly AL; NV (0b1111) is another encoding space.
u32 RandomCond() {
    return RandInt<u32>(0, 2) == 0 ? RandInt<u32>(0, 13) : 14;
}

// Data processing on r0-r7, immediate or register shifted by an immediate. Rd is never PC.
u32 RandomDataProcessing() {
    const u32 cond = RandomCond();
    const u32 op = RandInt<u32>(0, 15);
    const bool compare = op >= 8 && op <= 11;  // TST, TEQ, CMP, CMN: S=0 is another instruction
    const u32 s = compare ? 1 : RandInt<u32>(0, 1);
    const u32 rn = (op == 13 || op == 15) ? 0 : RandInt<u32>(0, 7);
    const u32 rd = compare ? 0 : RandInt<u32>(0, 7);
    if (RandInt<u32>(0, 1) == 0) {
        return cond << 28 | 1 << 25 | op << 21 | s << 20 | rn << 16 | rd << 12 | RandInt<u32>(0, 15) << 8 | RandInt<u32>(0, 255);
    }
    return cond << 28 | op << 21 | s << 20 | rn << 16 | rd << 12 | RandInt<u32>(0, 31) << 7 | RandInt<u32>(0, 3) << 5 | RandInt<u32>(0, 7);
}

// B or BL from instruction index to a random instruction of the program, or to the infinite loop after it.
u32 RandomBranch(size_t index, size_t length, bool link) {
    const u32 cond = RandInt<u32>(0, 3) == 0 ? 14 : RandInt<u32>(0, 13);
    const s64 target = RandInt<s64>(0, static_cast<s64>(length));
    const s64 offset = target - static_cast<s64>(index) - 2;
    return cond << 28 | (link ? 0b1011u : 0b1010u) << 24 | (static_cast<u32>(offset) & 0xFFFFFF);
}

std::vector<u32> RandomProgram(size_t length) {
    std::vector<u32> code;
    for (size_t i = 0; i < length; ++i) {
        const u32 kind = RandInt<u32>(0, 9);
        if (kind < 3) {
            code.push_back(RandomBranch(i, length, false));
        } else if (kind == 3) {
            code.push_back(RandomBranch(i, length, true));
        } else {
            code.push_back(RandomDataProcessing());
        }
    }
    return code;
}

}  // namespace

TEST_CASE("arm: conditional B after other instructions ends their block", "[arm][A32][condb]") {
    ArmTestEnv env;
    env.code_mem = {
        0xe3500000,  // cmp r0, #0
        0x1a000001,  // bne +#4 (to 0x10)
        0xe2811001,  // add r1, r1, #1
        0xeafffffe,  // b +#0
    };
    env.PadCodeMem();

    const IR::Block folded = TranslateArmBlock(env, true);
    const IR::Term::Terminal folded_term = folded.GetTerminal();
    const auto* if_term = boost::get<IR::Term::If>(&folded_term);
    REQUIRE(if_term != nullptr);
    CHECK(if_term->if_ == IR::Cond::NE);
    const auto* then_term = boost::get<IR::Term::LinkBlock>(&if_term->then_);
    const auto* else_term = boost::get<IR::Term::LinkBlock>(&if_term->else_);
    REQUIRE(then_term != nullptr);
    REQUIRE(else_term != nullptr);
    CHECK(PcOf(then_term->next) == 0x10);
    CHECK(PcOf(else_term->next) == 0x8);
    CHECK(PcOf(folded.EndLocation()) == 0x8);
    CHECK(folded.CycleCount() == 2);

    const IR::Block split = TranslateArmBlock(env, false);
    const IR::Term::Terminal split_term = split.GetTerminal();
    const auto* fast_term = boost::get<IR::Term::LinkBlockFast>(&split_term);
    REQUIRE(fast_term != nullptr);
    CHECK(PcOf(fast_term->next) == 0x4);
    CHECK(PcOf(split.EndLocation()) == 0x4);
    CHECK(split.CycleCount() == 1);
}

TEST_CASE("arm: conditional B at a block start keeps the block condition", "[arm][A32][condb]") {
    ArmTestEnv env;
    env.code_mem = {
        0x1a000001,  // bne +#4 (to 0xc)
        0xe2811001,  // add r1, r1, #1
        0xeafffffe,  // b +#0
    };
    env.PadCodeMem();

    for (const bool fold : {false, true}) {
        INFO("fold " << fold);
        const IR::Block block = TranslateArmBlock(env, fold);
        CHECK(block.GetCondition() == IR::Cond::NE);
        CHECK(PcOf(block.ConditionFailedLocation()) == 0x4);
        const IR::Term::Terminal term = block.GetTerminal();
        const auto* link_term = boost::get<IR::Term::LinkBlock>(&term);
        REQUIRE(link_term != nullptr);
        CHECK(PcOf(link_term->next) == 0xc);
    }
}

TEST_CASE("arm: folded conditional B runs like the split blocks", "[arm][A32][condb]") {
    ArmTestEnv env_fold;
    ArmTestEnv env_split;
    A32::UserConfig conf_fold;
    conf_fold.optimizations &= ~OptimizationFlag::FastDispatch;
    conf_fold.callbacks = &env_fold;
    A32::UserConfig conf_split = conf_fold;
    conf_split.optimizations &= ~OptimizationFlag::FoldConditionalBranches;
    conf_split.callbacks = &env_split;
    REQUIRE(conf_fold.HasOptimization(OptimizationFlag::FoldConditionalBranches));
    A32::Jit jit_fold{conf_fold};
    A32::Jit jit_split{conf_split};

    // Tick budgets that stop the run at different block ends, up to past the infinite loop.
    constexpr std::array<u64, 12> budgets{1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233};

    for (size_t iteration = 0; iteration < 2000; ++iteration) {
        const std::vector<u32> code = RandomProgram(24);
        env_fold.code_mem = code;
        env_fold.PadCodeMem();
        env_split.code_mem = code;
        env_split.PadCodeMem();
        jit_fold.ClearCache();
        jit_split.ClearCache();

        std::array<u32, 16> regs{};
        for (size_t i = 0; i < 15; ++i) {
            regs[i] = RandInt<u32>(0, 2) == 0 ? RandInt<u32>(0, 2) : RandInt<u32>(0, 0xFFFFFFFF);
        }
        const u32 cpsr = user_mode_arm | RandInt<u32>(0, 15) << 28;

        std::string listing;
        for (const u32 inst : code) {
            listing += fmt::format("{:08x} ", inst);
        }
        INFO("code " << listing);

        // A LinkBlockFast to a block not compiled yet returns to the dispatcher, which stops the
        // run when no ticks are left; the folded B runs first and stops at the LinkBlock after it.
        // Compile every block on the path first, so both runs stop at the same block ends.
        for (auto [jit, env] : {std::pair{&jit_fold, &env_fold}, std::pair{&jit_split, &env_split}}) {
            jit->Regs() = regs;
            jit->SetCpsr(cpsr);
            env->ticks_left = 1000;
            jit->Run();
        }

        for (const u64 budget : budgets) {
            INFO("budget " << budget);
            jit_fold.Regs() = regs;
            jit_fold.SetCpsr(cpsr);
            env_fold.ticks_left = budget;
            jit_fold.Run();

            jit_split.Regs() = regs;
            jit_split.SetCpsr(cpsr);
            env_split.ticks_left = budget;
            jit_split.Run();

            REQUIRE(jit_fold.Regs() == jit_split.Regs());
            REQUIRE(jit_fold.Cpsr() == jit_split.Cpsr());
            REQUIRE(env_fold.ticks_left == env_split.ticks_left);
        }
    }
}
