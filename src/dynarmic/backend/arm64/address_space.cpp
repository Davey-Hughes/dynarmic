/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <algorithm>
#include <cstdio>
#include <iterator>

#include <mcl/bit_cast.hpp>

#include "dynarmic/backend/arm64/a64_address_space.h"
#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/devirtualize.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace Dynarmic::Backend::Arm64 {

AddressSpace::AddressSpace(size_t code_cache_size)
        : code_cache_size(code_cache_size)
        , mem(code_cache_size)
        , code(mem.ptr(), mem.ptr())
        , fastmem_manager(exception_handler)
        , fast_dispatch_table(std::make_unique<FastDispatchEntry[]>(fast_dispatch_table_size)) {
    ASSERT_MSG(code_cache_size <= 128 * 1024 * 1024, "code_cache_size > 128 MiB not currently supported");

    exception_handler.Register(mem, code_cache_size);
    exception_handler.SetFastmemCallback([this](u64 host_pc) {
        return FastmemCallback(host_pc);
    });
}

AddressSpace::~AddressSpace() = default;

CodePtr AddressSpace::Get(IR::LocationDescriptor descriptor) {
    if (const auto iter = block_entries.find(descriptor); iter != block_entries.end()) {
        return iter->second;
    }
    return nullptr;
}

// The entry of the block that starts at or before host_pc, or end() if there is none.
static auto FindReverseEntry(const std::vector<std::pair<CodePtr, IR::LocationDescriptor>>& entries, CodePtr host_pc) {
    const auto iter = std::upper_bound(entries.begin(), entries.end(), host_pc,
                                       [](CodePtr pc, const auto& entry) { return pc < entry.first; });
    // upper_bound locates the first entry greater than host_pc, so the one before it is the block's
    return iter == entries.begin() ? entries.end() : std::prev(iter);
}

std::optional<IR::LocationDescriptor> AddressSpace::ReverseGetLocation(CodePtr host_pc) {
    if (const auto iter = FindReverseEntry(reverse_block_entries, host_pc); iter != reverse_block_entries.end()) {
        return iter->second;
    }
    return std::nullopt;
}

CodePtr AddressSpace::ReverseGetEntryPoint(CodePtr host_pc) {
    if (const auto iter = FindReverseEntry(reverse_block_entries, host_pc); iter != reverse_block_entries.end()) {
        return iter->first;
    }
    return nullptr;
}

CodePtr AddressSpace::GetOrEmit(IR::LocationDescriptor descriptor) {
    if (CodePtr block_entry = Get(descriptor)) {
        return block_entry;
    }

    IR::Block ir_block = GenerateIR(descriptor);
    return Emit(std::move(ir_block));
}

void AddressSpace::InvalidateBasicBlocks(const tsl::robin_set<IR::LocationDescriptor>& descriptors) {
    UnprotectCodeMemory();

    for (const auto& descriptor : descriptors) {
        const auto iter = block_entries.find(descriptor);
        if (iter == block_entries.end()) {
            continue;
        }

        // Unlink before removal because InvalidateBasicBlocks can be called within a fastmem callback,
        // and the currently executing block may have references to itself which need to be unlinked.
        RelinkForDescriptor(descriptor, nullptr);

        FastDispatchErase(descriptor);
        block_entries.erase(iter);
    }

    FlushCode();
    ProtectCodeMemory();
}

void AddressSpace::ClearCache() {
    std::fill_n(fast_dispatch_table.get(), fast_dispatch_table_size, FastDispatchEntry{});
    block_entries.clear();
    reverse_block_entries.clear();
    block_infos.clear();
    block_references.clear();
    emit_buffers.block_lists.Clear();
    code.set_offset(prelude_info.end_of_prelude);
}

void AddressSpace::DumpDisassembly() const {
    for (u32* ptr = mem.ptr(); ptr < code.xptr<u32*>(); ptr++) {
        std::printf("%s", Common::DisassembleAArch64(*ptr, mcl::bit_cast<u64>(ptr)).c_str());
    }
}

size_t AddressSpace::GetRemainingSize() {
    return code_cache_size - static_cast<size_t>(code.offset());
}

CodePtr AddressSpace::Emit(IR::Block block) {
    if (GetRemainingSize() < 1024 * 1024) {
        ClearCache();
    }

    UnprotectCodeMemory();

    EmittedBlockInfo new_block_info = EmitArm64(code, std::move(block), GetEmitConfig(), fastmem_manager, emit_buffers);
    const CodePtr entry_point = new_block_info.entry_point;

    ASSERT(block_entries.insert({block.Location(), entry_point}).second);
    ASSERT(reverse_block_entries.empty() || reverse_block_entries.back().first < entry_point);
    reverse_block_entries.emplace_back(entry_point, block.Location());
    const EmittedBlockInfo& block_info = block_infos.emplace_back(std::move(new_block_info));

    Link(block_info);
    RelinkForDescriptor(block.Location(), entry_point);

    unflushed_code.emplace_back(entry_point, block_info.size);
    FlushCode();
    ProtectCodeMemory();

    RegisterNewBasicBlock(block, block_info);

    return entry_point;
}

void AddressSpace::Link(const EmittedBlockInfo& block_info) {
    using namespace oaknut;
    using namespace oaknut::util;

    for (auto [ptr_offset, target] : block_info.relocations) {
        CodeGenerator c{mem.ptr(), mem.ptr()};
        c.set_xptr(reinterpret_cast<u32*>(block_info.entry_point + ptr_offset));

        switch (target) {
        case LinkTarget::ReturnToDispatcher:
            c.B(prelude_info.return_to_dispatcher);
            break;
        case LinkTarget::ReturnFromRunCode:
            c.B(prelude_info.return_from_run_code);
            break;
        case LinkTarget::ReadMemory8:
            c.BL(prelude_info.read_memory_8);
            break;
        case LinkTarget::ReadMemory16:
            c.BL(prelude_info.read_memory_16);
            break;
        case LinkTarget::ReadMemory32:
            c.BL(prelude_info.read_memory_32);
            break;
        case LinkTarget::ReadMemory64:
            c.BL(prelude_info.read_memory_64);
            break;
        case LinkTarget::ReadMemory128:
            c.BL(prelude_info.read_memory_128);
            break;
        case LinkTarget::WrappedReadMemory8:
            c.BL(prelude_info.wrapped_read_memory_8);
            break;
        case LinkTarget::WrappedReadMemory16:
            c.BL(prelude_info.wrapped_read_memory_16);
            break;
        case LinkTarget::WrappedReadMemory32:
            c.BL(prelude_info.wrapped_read_memory_32);
            break;
        case LinkTarget::WrappedReadMemory64:
            c.BL(prelude_info.wrapped_read_memory_64);
            break;
        case LinkTarget::WrappedReadMemory128:
            c.BL(prelude_info.wrapped_read_memory_128);
            break;
        case LinkTarget::ExclusiveReadMemory8:
            c.BL(prelude_info.exclusive_read_memory_8);
            break;
        case LinkTarget::ExclusiveReadMemory16:
            c.BL(prelude_info.exclusive_read_memory_16);
            break;
        case LinkTarget::ExclusiveReadMemory32:
            c.BL(prelude_info.exclusive_read_memory_32);
            break;
        case LinkTarget::ExclusiveReadMemory64:
            c.BL(prelude_info.exclusive_read_memory_64);
            break;
        case LinkTarget::ExclusiveReadMemory128:
            c.BL(prelude_info.exclusive_read_memory_128);
            break;
        case LinkTarget::WriteMemory8:
            c.BL(prelude_info.write_memory_8);
            break;
        case LinkTarget::WriteMemory16:
            c.BL(prelude_info.write_memory_16);
            break;
        case LinkTarget::WriteMemory32:
            c.BL(prelude_info.write_memory_32);
            break;
        case LinkTarget::WriteMemory64:
            c.BL(prelude_info.write_memory_64);
            break;
        case LinkTarget::WriteMemory128:
            c.BL(prelude_info.write_memory_128);
            break;
        case LinkTarget::WrappedWriteMemory8:
            c.BL(prelude_info.wrapped_write_memory_8);
            break;
        case LinkTarget::WrappedWriteMemory16:
            c.BL(prelude_info.wrapped_write_memory_16);
            break;
        case LinkTarget::WrappedWriteMemory32:
            c.BL(prelude_info.wrapped_write_memory_32);
            break;
        case LinkTarget::WrappedWriteMemory64:
            c.BL(prelude_info.wrapped_write_memory_64);
            break;
        case LinkTarget::WrappedWriteMemory128:
            c.BL(prelude_info.wrapped_write_memory_128);
            break;
        case LinkTarget::ExclusiveWriteMemory8:
            c.BL(prelude_info.exclusive_write_memory_8);
            break;
        case LinkTarget::ExclusiveWriteMemory16:
            c.BL(prelude_info.exclusive_write_memory_16);
            break;
        case LinkTarget::ExclusiveWriteMemory32:
            c.BL(prelude_info.exclusive_write_memory_32);
            break;
        case LinkTarget::ExclusiveWriteMemory64:
            c.BL(prelude_info.exclusive_write_memory_64);
            break;
        case LinkTarget::ExclusiveWriteMemory128:
            c.BL(prelude_info.exclusive_write_memory_128);
            break;
        case LinkTarget::CallSVC:
            c.BL(prelude_info.call_svc);
            break;
        case LinkTarget::ExceptionRaised:
            c.BL(prelude_info.exception_raised);
            break;
        case LinkTarget::InstructionSynchronizationBarrierRaised:
            c.BL(prelude_info.isb_raised);
            break;
        case LinkTarget::InstructionCacheOperationRaised:
            c.BL(prelude_info.ic_raised);
            break;
        case LinkTarget::DataCacheOperationRaised:
            c.BL(prelude_info.dc_raised);
            break;
        case LinkTarget::GetCNTPCT:
            c.BL(prelude_info.get_cntpct);
            break;
        case LinkTarget::AddTicks:
            c.BL(prelude_info.add_ticks);
            break;
        case LinkTarget::GetTicksRemaining:
            c.BL(prelude_info.get_ticks_remaining);
            break;
        default:
            ASSERT_FALSE("Invalid relocation target");
        }
    }

    for (const auto& block_relocation : block_info.block_relocations) {
        // A block can link to the same target more than once; its references are added here, one after another.
        auto& references = block_references[block_relocation.target];
        if (references.empty() || references.back() != &block_info) {
            references.push_back(&block_info);
        }
        LinkBlockLink(block_info.entry_point, Get(block_relocation.target), block_relocation);
    }
}

void AddressSpace::LinkBlockLink(const CodePtr entry_point, const CodePtr target_ptr, const BlockRelocation& block_relocation) {
    using namespace oaknut;
    using namespace oaknut::util;

    CodeGenerator c{mem.ptr(), mem.ptr()};
    c.set_xptr(reinterpret_cast<u32*>(entry_point + block_relocation.code_offset));

    switch (block_relocation.type) {
    case BlockRelocationType::Branch:
        if (target_ptr) {
            c.B((void*)target_ptr);
        } else {
            c.NOP();
        }
        break;
    case BlockRelocationType::MoveToScratch1:
        if (target_ptr) {
            c.ADRL(Xscratch1, (void*)target_ptr);
        } else {
            c.ADRL(Xscratch1, prelude_info.return_to_dispatcher);
        }
        break;
    default:
        ASSERT_FALSE("Invalid BlockRelocationType");
    }
}

void AddressSpace::RelinkForDescriptor(IR::LocationDescriptor target_descriptor, CodePtr target_ptr) {
    const auto references = block_references.find(target_descriptor);
    if (references == block_references.end()) {
        return;
    }

    for (const EmittedBlockInfo* block_info : references->second) {
        for (const auto& block_relocation : block_info->block_relocations) {
            if (block_relocation.target == target_descriptor) {
                LinkBlockLink(block_info->entry_point, target_ptr, block_relocation);
                // Only the patched instructions changed: one B or NOP, or the two of an ADRL.
                const size_t patch_size = block_relocation.type == BlockRelocationType::MoveToScratch1 ? 8 : 4;
                unflushed_code.emplace_back(block_info->entry_point + block_relocation.code_offset, patch_size);
            }
        }
    }
}

void AddressSpace::FlushCode() {
    if (unflushed_code.empty()) {
        return;
    }

#if defined(__APPLE__) || defined(_WIN32)
    for (const auto& [ptr, size] : unflushed_code) {
        mem.invalidate(reinterpret_cast<u32*>(ptr), size);
    }
#else
    // The sequence of oaknut::CodeBlock::invalidate(), with every range in each step. The DSB and ISB that end
    // it cost far more than cleaning and invalidating a block's lines, so a block and the blocks relinked to
    // it share them.
    static u64 icache_line_size = 0x10000, dcache_line_size = 0x10000;

    u64 ctr;
    __asm__ volatile("mrs %0, ctr_el0"
                     : "=r"(ctr));

    const u64 isize = icache_line_size = std::min<u64>(icache_line_size, 4 << ((ctr >> 0) & 0xf));
    const u64 dsize = dcache_line_size = std::min<u64>(dcache_line_size, 4 << ((ctr >> 16) & 0xf));

    for (const auto& [ptr, size] : unflushed_code) {
        const u64 end = mcl::bit_cast<u64>(ptr) + size;
        for (u64 addr = mcl::bit_cast<u64>(ptr) & ~(dsize - 1); addr < end; addr += dsize) {
            __asm__ volatile("dc cvau, %0"
                             :
                             : "r"(addr)
                             : "memory");
        }
    }
    __asm__ volatile("dsb ish"
                     :
                     :
                     : "memory");

    for (const auto& [ptr, size] : unflushed_code) {
        const u64 end = mcl::bit_cast<u64>(ptr) + size;
        for (u64 addr = mcl::bit_cast<u64>(ptr) & ~(isize - 1); addr < end; addr += isize) {
            __asm__ volatile("ic ivau, %0"
                             :
                             : "r"(addr)
                             : "memory");
        }
    }
    __asm__ volatile("dsb ish\nisb"
                     :
                     :
                     : "memory");
#endif

    unflushed_code.clear();
}

FakeCall AddressSpace::FastmemCallback(u64 host_pc) {
    {
        const auto host_ptr = mcl::bit_cast<CodePtr>(host_pc);

        const auto entry = FindReverseEntry(reverse_block_entries, host_ptr);
        if (entry == reverse_block_entries.end()) {
            goto fail;
        }

        const CodePtr entry_point = entry->first;
        const EmittedBlockInfo& block_info = block_infos[static_cast<size_t>(entry - reverse_block_entries.begin())];

        const FastmemPatchInfo* const patch_entry = block_info.fastmem_patch_info.Find(host_ptr - entry_point);
        if (!patch_entry) {
            goto fail;
        }

        const auto fc = patch_entry->fc;

        if (patch_entry->recompile) {
            const auto marker = patch_entry->marker;
            fastmem_manager.MarkDoNotFastmem(marker);
            InvalidateBasicBlocks({std::get<0>(marker)});
        }

        return fc;
    }

fail:
    fmt::print("dynarmic: Segfault happened within JITted code at host_pc = {:016x}\n", host_pc);
    fmt::print("Segfault wasn't at a fastmem patch location!\n");
    ASSERT_FALSE("segfault");
}

}  // namespace Dynarmic::Backend::Arm64
