/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/ir/basic_block.h"

namespace Dynarmic::IR {
class Block;
}  // namespace Dynarmic::IR

namespace Dynarmic::Backend::Arm64 {

struct EmitConfig;
class FastmemManager;
class FpsrManager;

using SharedLabel = std::shared_ptr<oaknut::Label>;

inline SharedLabel GenSharedLabel() {
    return std::make_shared<oaknut::Label>();
}

// How an instruction takes part in a VFP register transfer to or from memory, when its emitter departs from
// the usual code (AssignVfpMemoryRoles() in emit_arm64_memory.cpp). A pair is two 32-bit accesses to
// consecutive words, a D register's halves, served by one 64-bit host access.
enum class VfpMemoryRole : u8 {
    None,
    PairReadLo,       // reads both words into one FPR
    PairReadHi,       // no code: shares PairReadLo's register
    PairReadPack,     // no code: the packed value is PairReadLo's register
    PairWriteLsw,     // no code: the value's low word, in the value's register
    PairWriteMsw,     // no code: shares the value's register, read only by PairWriteHi
    PairWriteLo,      // writes both words
    PairWriteHi,      // no code
    PairHiAddress,    // no code: an Add32 read only by PairReadHi or PairWriteHi
    StoreValueToGpr,  // an S register read only by a 32-bit memory write: load it into a GPR
};

struct VfpMemoryInfo {
    VfpMemoryRole role = VfpMemoryRole::None;
    const IR::Inst* partner = nullptr;  // PairReadHi: its PairReadLo
};

// Containers EmitArm64 fills for each block. The caller keeps them between blocks so that their
// storage is reused instead of allocated again for every block.
struct EmitBuffers {
    RegAllocBuffers reg_alloc;
    std::vector<std::function<void()>> deferred_emits;
    std::vector<VfpMemoryInfo> vfp_memory;  // by IR::Inst::GetName()
};

struct EmitContext {
    IR::Block& block;
    RegAlloc& reg_alloc;
    const EmitConfig& conf;
    EmittedBlockInfo& ebi;
    FpsrManager& fpsr;
    FastmemManager& fastmem;

    std::vector<std::function<void()>>& deferred_emits;
    std::vector<VfpMemoryInfo>& vfp_memory;

    VfpMemoryInfo VfpMemory(const IR::Inst* inst) const {
        const size_t name = inst->GetName();
        return name != 0 && name < vfp_memory.size() ? vfp_memory[name] : VfpMemoryInfo{};
    }

    FP::FPCR FPCR(bool fpcr_controlled = true) const {
        const FP::FPCR fpcr = conf.descriptor_to_fpcr(block.Location());
        return fpcr_controlled ? fpcr : fpcr.ASIMDStandardValue();
    }
};

}  // namespace Dynarmic::Backend::Arm64
