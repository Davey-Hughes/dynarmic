/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <mcl/stdint.hpp>
#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/emit_context.h"
#include "dynarmic/backend/arm64/fastmem.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Arm64 {

class AddressSpace {
public:
    explicit AddressSpace(size_t code_cache_size);
    virtual ~AddressSpace();

    virtual IR::Block GenerateIR(IR::LocationDescriptor) const = 0;

    CodePtr Get(IR::LocationDescriptor descriptor);

    // Returns "most likely" LocationDescriptor assocated with the emitted code at that location
    std::optional<IR::LocationDescriptor> ReverseGetLocation(CodePtr host_pc);

    // Returns "most likely" entry_point associated with the emitted code at that location
    CodePtr ReverseGetEntryPoint(CodePtr host_pc);

    CodePtr GetOrEmit(IR::LocationDescriptor descriptor);

    void InvalidateBasicBlocks(const tsl::robin_set<IR::LocationDescriptor>& descriptors);

    void ClearCache();

    void DumpDisassembly() const;

protected:
    virtual const EmitConfig& GetEmitConfig() const = 0;
    virtual void RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) = 0;

    void ProtectCodeMemory() {
#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)
        mem.protect();
#endif
    }

    void UnprotectCodeMemory() {
#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)
        mem.unprotect();
#endif
    }

    size_t GetRemainingSize();
    CodePtr Emit(IR::Block&& block);
    // The info has to be the one in block_infos: block_references keeps its address.
    void Link(const EmittedBlockInfo& block_info);
    void LinkBlockLink(const CodePtr entry_point, const CodePtr target_ptr, const BlockRelocation& block_relocation);
    // Patches the blocks that link to the target. The patched instructions wait in unflushed_code for FlushCode().
    void RelinkForDescriptor(IR::LocationDescriptor target_descriptor, CodePtr target_ptr);
    // Cache maintenance for every range in unflushed_code, with one synchronization for all of them.
    void FlushCode();

    FakeCall FastmemCallback(u64 host_pc);

    // Fast dispatch (OptimizationFlag::FastDispatch): a direct-mapped cache of descriptor -> entry point that
    // the dispatcher probes in generated code before it calls GetOrEmit(). A32AddressSpace::EmitPrelude()
    // in a32_address_space.cpp emits the probe; its index must match FastDispatchIndex().
    struct FastDispatchEntry {
        u64 descriptor = ~u64{0};
        CodePtr code_ptr = nullptr;
    };
    static constexpr size_t fast_dispatch_table_bits = 16;
    static constexpr size_t fast_dispatch_table_size = size_t{1} << fast_dispatch_table_bits;
    static constexpr size_t FastDispatchIndex(u64 descriptor) {
        return ((descriptor ^ (descriptor >> 32)) >> 1) & (fast_dispatch_table_size - 1);
    }
    void FastDispatchInsert(IR::LocationDescriptor descriptor, CodePtr code_ptr) {
        fast_dispatch_table[FastDispatchIndex(descriptor.Value())] = {descriptor.Value(), code_ptr};
    }
    void FastDispatchErase(IR::LocationDescriptor descriptor) {
        FastDispatchEntry& entry = fast_dispatch_table[FastDispatchIndex(descriptor.Value())];
        if (entry.descriptor == descriptor.Value()) {
            entry = {};
        }
    }

    const size_t code_cache_size;
    oaknut::CodeBlock mem;
    oaknut::CodeGenerator code;

    // A IR::LocationDescriptor will have one current CodePtr.
    // However, there can be multiple other CodePtrs which are older, previously invalidated blocks.
    tsl::robin_map<IR::LocationDescriptor, CodePtr> block_entries;
    // Sorted by entry point: Emit() only appends code until ClearCache() resets it.
    std::vector<std::pair<CodePtr, IR::LocationDescriptor>> reverse_block_entries;
    // block_infos[i] belongs to reverse_block_entries[i]. Infos are only added until ClearCache(), so their
    // addresses are stable.
    std::deque<EmittedBlockInfo> block_infos;
    // A block that links to a target. The references to one target form a chain, newest first; the nodes
    // live in emit_buffers.block_lists.
    struct BlockReference {
        const EmittedBlockInfo* block_info;
        const BlockReference* next;
    };
    // The blocks that link to each target, each listed once (Link() adds a block's references together).
    tsl::robin_map<IR::LocationDescriptor, const BlockReference*> block_references;

    std::vector<std::pair<CodePtr, size_t>> unflushed_code;

    ExceptionHandler exception_handler;
    FastmemManager fastmem_manager;
    EmitBuffers emit_buffers;
    std::unique_ptr<FastDispatchEntry[]> fast_dispatch_table;

    struct PreludeInfo {
        std::ptrdiff_t end_of_prelude;

        using RunCodeFuncType = HaltReason (*)(CodePtr entry_point, void* jit_state, volatile u32* halt_reason);
        RunCodeFuncType run_code;
        RunCodeFuncType step_code;
        void* return_to_dispatcher;
        void* return_from_run_code;

        void* read_memory_8;
        void* read_memory_16;
        void* read_memory_32;
        void* read_memory_64;
        void* read_memory_128;
        void* wrapped_read_memory_8;
        void* wrapped_read_memory_16;
        void* wrapped_read_memory_32;
        void* wrapped_read_memory_64;
        void* wrapped_read_memory_128;
        void* exclusive_read_memory_8;
        void* exclusive_read_memory_16;
        void* exclusive_read_memory_32;
        void* exclusive_read_memory_64;
        void* exclusive_read_memory_128;
        void* write_memory_8;
        void* write_memory_16;
        void* write_memory_32;
        void* write_memory_64;
        void* write_memory_128;
        void* wrapped_write_memory_8;
        void* wrapped_write_memory_16;
        void* wrapped_write_memory_32;
        void* wrapped_write_memory_64;
        void* wrapped_write_memory_128;
        void* exclusive_write_memory_8;
        void* exclusive_write_memory_16;
        void* exclusive_write_memory_32;
        void* exclusive_write_memory_64;
        void* exclusive_write_memory_128;

        void* call_svc;
        void* exception_raised;
        void* dc_raised;
        void* ic_raised;
        void* isb_raised;

        void* get_cntpct;
        void* add_ticks;
        void* get_ticks_remaining;
    } prelude_info;
};

}  // namespace Dynarmic::Backend::Arm64
