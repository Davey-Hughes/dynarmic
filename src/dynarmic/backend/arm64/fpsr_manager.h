/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <mcl/stdint.hpp>

namespace oaknut {
struct CodeGenerator;
struct WReg;
}  // namespace oaknut

namespace Dynarmic::Backend::Arm64 {

class FpsrManager {
public:
    explicit FpsrManager(oaknut::CodeGenerator& code, size_t state_fpsr_offset);

    void Spill();
    void Load();
    void Overwrite() { fpsr_loaded = false; }

    void GetFpsr(oaknut::WReg);

    // For a path off the main line that calls into C++ and rejoins it (a memory fallback): when IsLoaded(),
    // EmitSpillKeepState() before the call and EmitReZero() after it leave FPSR as the main line expects.
    bool IsLoaded() const { return fpsr_loaded; }
    void EmitSpillKeepState();
    void EmitReZero();

private:
    oaknut::CodeGenerator& code;
    size_t state_fpsr_offset;
    bool fpsr_loaded = false;
};

}  // namespace Dynarmic::Backend::Arm64
