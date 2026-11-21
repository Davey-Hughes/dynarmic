/* This file is part of the dynarmic project.
 * SPDX-License-Identifier: 0BSD
 */

#include <catch2/catch_test_macros.hpp>
#include <mcl/stdint.hpp>

#include "./testenv.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/optimization_flags.h"

using namespace Dynarmic;

namespace {

A32::UserConfig Config(ArmTestEnv* env, bool fast_dispatch) {
    A32::UserConfig config{env};
    if (!fast_dispatch) {
        config.optimizations &= ~OptimizationFlag::FastDispatch;
    }
    return config;
}

void RunFrom0(A32::Jit& jit, ArmTestEnv& env) {
    jit.Regs()[0] = 0;
    jit.Regs()[2] = 0;
    jit.Regs()[15] = 0;
    env.ticks_left = 30;
    jit.Run();
}

}  // namespace

// The BX is a FastDispatchHint terminal, so every iteration goes through the dispatcher. After the
// first iteration the target comes from the fast dispatch table; a stale entry would run old code.
TEST_CASE("arm: fast dispatch follows invalidation and ClearCache", "[arm][A32][fast_dispatch]") {
    for (const bool fast_dispatch : {true, false}) {
        ArmTestEnv test_env;
        A32::Jit jit{Config(&test_env, fast_dispatch)};
        test_env.code_mem = {
            0xe12fff11,  // 0x00: bx r1
            0xe320f000,  // 0x04: nop
            0xe320f000,  // 0x08: nop
            0xe320f000,  // 0x0c: nop
            0xe3a00001,  // 0x10: mov r0, #1
            0xe2822001,  // 0x14: add r2, r2, #1
            0xeafffff8,  // 0x18: b 0x00
        };
        jit.SetCpsr(0x000001d0);  // User-mode
        jit.Regs()[1] = 0x10;

        RunFrom0(jit, test_env);
        REQUIRE(jit.Regs()[0] == 1);
        REQUIRE(jit.Regs()[2] >= 3);

        test_env.code_mem[4] = 0xe3a00002;  // mov r0, #2
        jit.InvalidateCacheRange(0x10, 4);
        RunFrom0(jit, test_env);
        REQUIRE(jit.Regs()[0] == 2);
        REQUIRE(jit.Regs()[2] >= 3);

        test_env.code_mem[4] = 0xe3a00003;  // mov r0, #3
        jit.ClearCache();
        RunFrom0(jit, test_env);
        REQUIRE(jit.Regs()[0] == 3);
        REQUIRE(jit.Regs()[2] >= 3);

        // A different target through the same BX.
        test_env.code_mem[3] = 0xeafffffe;  // 0x0c: b 0x0c (spin until the ticks run out)
        jit.Regs()[1] = 0x0c;
        RunFrom0(jit, test_env);
        REQUIRE(jit.Regs()[0] == 0);
        REQUIRE(jit.Regs()[2] == 0);
        REQUIRE(jit.Regs()[15] == 0x0c);
    }
}
