/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstddef>
#include <deque>
#include <new>
#include <type_traits>
#include <utility>
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

// A label that inline code and its deferred emit share. GenSharedLabel() makes it in EmitBuffers::labels,
// where it lives until EmitArm64() starts the next block.
using SharedLabel = oaknut::Label*;

// A deferred emit's closure, held inline so that queuing one doesn't allocate (a std::function would, for
// closures this size). EmitBuffers keeps the list, so its storage is reused between blocks.
class DeferredEmit {
public:
    template<typename F>
    requires(!std::is_same_v<std::decay_t<F>, DeferredEmit>)
    DeferredEmit(F&& f) {
        using T = std::decay_t<F>;
        static_assert(sizeof(T) <= sizeof(storage) && alignof(T) <= alignof(std::max_align_t), "Closure too large for DeferredEmit");
        new (storage) T(std::forward<F>(f));
        ops = &ops_for<T>;
    }
    DeferredEmit(DeferredEmit&& other) noexcept
            : ops{other.ops} {
        ops->move(storage, other.storage);
    }
    DeferredEmit(const DeferredEmit&) = delete;
    DeferredEmit& operator=(const DeferredEmit&) = delete;
    DeferredEmit& operator=(DeferredEmit&&) = delete;
    ~DeferredEmit() { ops->destroy(storage); }

    void operator()() { ops->call(storage); }

private:
    struct Ops {
        void (*call)(void* closure);
        void (*move)(void* to, void* from);
        void (*destroy)(void* closure);
    };

    template<typename T>
    static constexpr Ops ops_for{
        [](void* closure) { (*std::launder(static_cast<T*>(closure)))(); },
        [](void* to, void* from) { new (to) T(std::move(*std::launder(static_cast<T*>(from)))); },
        [](void* closure) { std::launder(static_cast<T*>(closure))->~T(); },
    };

    const Ops* ops;
    // Fits the largest closure, 80 bytes (the VFP pair fallbacks in emit_arm64_memory.cpp). The constructor
    // fails to compile for a larger one.
    alignas(std::max_align_t) std::byte storage[96];
};

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
    std::vector<DeferredEmit> deferred_emits;
    std::vector<VfpMemoryInfo> vfp_memory;  // by IR::Inst::GetName()
    std::deque<oaknut::Label> labels;       // a deque: SharedLabels point into it
    // EmitArm64 copies these into block_lists at the end, for the EmittedBlockInfo.
    std::vector<Relocation> relocations;
    std::vector<BlockRelocation> block_relocations;
    FastmemPatchTable fastmem_patches;
    // Not per block: it holds the lists of every block emitted since the last ClearCache().
    BlockListArena block_lists;
};

struct EmitContext {
    IR::Block& block;
    RegAlloc& reg_alloc;
    const EmitConfig& conf;
    EmittedBlockInfo& ebi;
    FpsrManager& fpsr;
    FastmemManager& fastmem;

    std::vector<DeferredEmit>& deferred_emits;
    std::vector<VfpMemoryInfo>& vfp_memory;
    std::deque<oaknut::Label>& labels;
    std::vector<Relocation>& relocations;             // become ebi.relocations
    std::vector<BlockRelocation>& block_relocations;  // become ebi.block_relocations
    FastmemPatchTable& fastmem_patches;               // becomes ebi.fastmem_patch_info

    VfpMemoryInfo VfpMemory(const IR::Inst* inst) const {
        const size_t name = inst->GetName();
        return name != 0 && name < vfp_memory.size() ? vfp_memory[name] : VfpMemoryInfo{};
    }

    FP::FPCR FPCR(bool fpcr_controlled = true) const {
        const FP::FPCR fpcr = conf.descriptor_to_fpcr(block.Location());
        return fpcr_controlled ? fpcr : fpcr.ASIMDStandardValue();
    }
};

inline SharedLabel GenSharedLabel(EmitContext& ctx) {
    return &ctx.labels.emplace_back();
}

}  // namespace Dynarmic::Backend::Arm64
