/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/emit_arm64_memory.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>

#include <mcl/bit_cast.hpp>
#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/emit_context.h"
#include "dynarmic/backend/arm64/fastmem.h"
#include "dynarmic/backend/arm64/fpsr_manager.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/ir/acc_type.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Arm64 {

using namespace oaknut::util;

namespace {

bool IsOrdered(IR::AccType acctype) {
    return acctype == IR::AccType::ORDERED || acctype == IR::AccType::ORDEREDRW || acctype == IR::AccType::LIMITEDORDERED;
}

LinkTarget ReadMemoryLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::ReadMemory8;
    case 16:
        return LinkTarget::ReadMemory16;
    case 32:
        return LinkTarget::ReadMemory32;
    case 64:
        return LinkTarget::ReadMemory64;
    case 128:
        return LinkTarget::ReadMemory128;
    }
    UNREACHABLE();
}

LinkTarget WriteMemoryLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::WriteMemory8;
    case 16:
        return LinkTarget::WriteMemory16;
    case 32:
        return LinkTarget::WriteMemory32;
    case 64:
        return LinkTarget::WriteMemory64;
    case 128:
        return LinkTarget::WriteMemory128;
    }
    UNREACHABLE();
}

LinkTarget WrappedReadMemoryLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::WrappedReadMemory8;
    case 16:
        return LinkTarget::WrappedReadMemory16;
    case 32:
        return LinkTarget::WrappedReadMemory32;
    case 64:
        return LinkTarget::WrappedReadMemory64;
    case 128:
        return LinkTarget::WrappedReadMemory128;
    }
    UNREACHABLE();
}

LinkTarget WrappedWriteMemoryLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::WrappedWriteMemory8;
    case 16:
        return LinkTarget::WrappedWriteMemory16;
    case 32:
        return LinkTarget::WrappedWriteMemory32;
    case 64:
        return LinkTarget::WrappedWriteMemory64;
    case 128:
        return LinkTarget::WrappedWriteMemory128;
    }
    UNREACHABLE();
}

LinkTarget ExclusiveReadMemoryLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::ExclusiveReadMemory8;
    case 16:
        return LinkTarget::ExclusiveReadMemory16;
    case 32:
        return LinkTarget::ExclusiveReadMemory32;
    case 64:
        return LinkTarget::ExclusiveReadMemory64;
    case 128:
        return LinkTarget::ExclusiveReadMemory128;
    }
    UNREACHABLE();
}

LinkTarget ExclusiveWriteMemoryLinkTarget(size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::ExclusiveWriteMemory8;
    case 16:
        return LinkTarget::ExclusiveWriteMemory16;
    case 32:
        return LinkTarget::ExclusiveWriteMemory32;
    case 64:
        return LinkTarget::ExclusiveWriteMemory64;
    case 128:
        return LinkTarget::ExclusiveWriteMemory128;
    }
    UNREACHABLE();
}

template<size_t bitsize>
void CallbackOnlyEmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1]);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());

    EmitRelocation(code, ctx, ReadMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }

    if constexpr (bitsize == 128) {
        code.MOV(Q8.B16(), Q0.B16());
        ctx.reg_alloc.DefineAsRegister(inst, Q8);
    } else {
        ctx.reg_alloc.DefineAsRegister(inst, X0);
    }
}

template<size_t bitsize>
void CallbackOnlyEmitExclusiveReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1]);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());

    code.MOV(Wscratch0, 1);
    code.STRB(Wscratch0, Xstate, ctx.conf.state_exclusive_state_offset);
    EmitRelocation(code, ctx, ExclusiveReadMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }

    if constexpr (bitsize == 128) {
        code.MOV(Q8.B16(), Q0.B16());
        ctx.reg_alloc.DefineAsRegister(inst, Q8);
    } else {
        ctx.reg_alloc.DefineAsRegister(inst, X0);
    }
}

template<size_t bitsize>
void CallbackOnlyEmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1], args[2]);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());

    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    EmitRelocation(code, ctx, WriteMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
}

template<size_t bitsize>
void CallbackOnlyEmitExclusiveWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1], args[2]);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());

    oaknut::Label end;

    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    code.MOV(W0, 1);
    code.LDRB(Wscratch0, Xstate, ctx.conf.state_exclusive_state_offset);
    code.CBZ(Wscratch0, end);
    code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
    EmitRelocation(code, ctx, ExclusiveWriteMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    code.l(end);
    ctx.reg_alloc.DefineAsRegister(inst, X0);
}

constexpr size_t page_bits = 12;
constexpr size_t page_size = 1 << page_bits;
constexpr size_t page_mask = (1 << page_bits) - 1;

// This function may use Xscratch0 as a scratch register
// Trashes NZCV
template<size_t bitsize>
void EmitDetectMisalignedVAddr(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback) {
    static_assert(bitsize == 8 || bitsize == 16 || bitsize == 32 || bitsize == 64 || bitsize == 128);

    if (bitsize == 8 || (ctx.conf.detect_misaligned_access_via_page_table & bitsize) == 0) {
        return;
    }

    if (!ctx.conf.only_detect_misalignment_via_page_table_on_page_boundary) {
        const u64 align_mask = []() -> u64 {
            switch (bitsize) {
            case 16:
                return 0b1;
            case 32:
                return 0b11;
            case 64:
                return 0b111;
            case 128:
                return 0b1111;
            default:
                UNREACHABLE();
            }
        }();

        code.TST(Xaddr, align_mask);
        code.B(NE, *fallback);
    } else {
        // If (addr & page_mask) > page_size - byte_size, use fallback.
        code.AND(Xscratch0, Xaddr, page_mask);
        code.CMP(Xscratch0, page_size - bitsize / 8);
        code.B(HI, *fallback);
    }
}

// Outputs Xscratch0 = page_table[addr >> page_bits]
// May use Xscratch1 as scratch register
// Address to read/write = [ret0 + ret1], ret0 is always Xscratch0 and ret1 is either Xaddr or Xscratch1
// Trashes NZCV
template<size_t bitsize>
std::pair<oaknut::XReg, oaknut::XReg> InlinePageTableEmitVAddrLookup(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback) {
    const size_t valid_page_index_bits = ctx.conf.page_table_address_space_bits - page_bits;
    const size_t unused_top_bits = 64 - ctx.conf.page_table_address_space_bits;

    EmitDetectMisalignedVAddr<bitsize>(code, ctx, Xaddr, fallback);

    if (ctx.conf.silently_mirror_page_table || unused_top_bits == 0) {
        code.UBFX(Xscratch0, Xaddr, page_bits, valid_page_index_bits);
    } else {
        code.LSR(Xscratch0, Xaddr, page_bits);
        code.TST(Xscratch0, u64(~u64(0)) << valid_page_index_bits);
        code.B(NE, *fallback);
    }

    code.LDR(Xscratch0, Xpagetable, Xscratch0, LSL, 3);

    if (ctx.conf.page_table_pointer_mask_bits != 0) {
        const u64 mask = u64(~u64(0)) << ctx.conf.page_table_pointer_mask_bits;
        code.AND(Xscratch0, Xscratch0, mask);
    }

    code.CBZ(Xscratch0, *fallback);

    if (ctx.conf.absolute_offset_page_table) {
        return std::make_pair(Xscratch0, Xaddr);
    }
    code.AND(Xscratch1, Xaddr, page_mask);
    return std::make_pair(Xscratch0, Xscratch1);
}

template<std::size_t bitsize>
CodePtr EmitMemoryLdr(oaknut::CodeGenerator& code, int value_idx, oaknut::XReg Xbase, oaknut::XReg Xoffset, bool ordered, bool extend32 = false) {
    const auto index_ext = extend32 ? oaknut::IndexExt::UXTW : oaknut::IndexExt::LSL;
    const auto add_ext = extend32 ? oaknut::AddSubExt::UXTW : oaknut::AddSubExt::LSL;
    const auto Roffset = extend32 ? oaknut::RReg{Xoffset.toW()} : oaknut::RReg{Xoffset};

    CodePtr fastmem_location = code.xptr<CodePtr>();

    if (ordered) {
        code.ADD(Xscratch0, Xbase, Roffset, add_ext);

        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.LDARB(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 16:
            code.LDARH(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 32:
            code.LDAR(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 64:
            code.LDAR(oaknut::XReg{value_idx}, Xscratch0);
            break;
        case 128:
            code.LDR(oaknut::QReg{value_idx}, Xscratch0);
            code.DMB(oaknut::BarrierOp::ISH);
            break;
        default:
            ASSERT_FALSE("Invalid bitsize");
        }
    } else {
        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.LDRB(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 16:
            code.LDRH(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 32:
            code.LDR(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 64:
            code.LDR(oaknut::XReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 128:
            code.LDR(oaknut::QReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        default:
            ASSERT_FALSE("Invalid bitsize");
        }
    }

    return fastmem_location;
}

template<std::size_t bitsize>
CodePtr EmitMemoryStr(oaknut::CodeGenerator& code, int value_idx, oaknut::XReg Xbase, oaknut::XReg Xoffset, bool ordered, bool extend32 = false) {
    const auto index_ext = extend32 ? oaknut::IndexExt::UXTW : oaknut::IndexExt::LSL;
    const auto add_ext = extend32 ? oaknut::AddSubExt::UXTW : oaknut::AddSubExt::LSL;
    const auto Roffset = extend32 ? oaknut::RReg{Xoffset.toW()} : oaknut::RReg{Xoffset};

    CodePtr fastmem_location;

    if (ordered) {
        code.ADD(Xscratch0, Xbase, Roffset, add_ext);

        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.STLRB(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 16:
            code.STLRH(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 32:
            code.STLR(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 64:
            code.STLR(oaknut::XReg{value_idx}, Xscratch0);
            break;
        case 128:
            code.DMB(oaknut::BarrierOp::ISH);
            code.STR(oaknut::QReg{value_idx}, Xscratch0);
            code.DMB(oaknut::BarrierOp::ISH);
            break;
        default:
            ASSERT_FALSE("Invalid bitsize");
        }
    } else {
        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.STRB(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 16:
            code.STRH(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 32:
            code.STR(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 64:
            code.STR(oaknut::XReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 128:
            code.STR(oaknut::QReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        default:
            ASSERT_FALSE("Invalid bitsize");
        }
    }

    return fastmem_location;
}

template<size_t bitsize>
void InlinePageTableEmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.WriteQ(inst);
        } else {
            return ctx.reg_alloc.WriteReg<std::max<std::size_t>(bitsize, 32)>(inst);
        }
    }();
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    // FPSR stays live on the inline path. Only the fallback calls into C++, so it saves FPSR around the call.
    const bool fpsr_loaded = ctx.fpsr.IsLoaded();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const auto [Xbase, Xoffset] = InlinePageTableEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
    EmitMemoryLdr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered);

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fpsr_loaded, fallback, end] {
        code.l(*fallback);
        if (fpsr_loaded) {
            ctx.fpsr.EmitSpillKeepState();
        }
        code.MOV(Xscratch0, Xaddr);
        EmitRelocation(code, ctx, WrappedReadMemoryLinkTarget(bitsize));
        if (fpsr_loaded) {
            ctx.fpsr.EmitReZero();
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        if constexpr (bitsize == 128) {
            code.MOV(Rvalue.B16(), Q0.B16());
        } else {
            code.MOV(Rvalue.toX(), Xscratch0);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

template<size_t bitsize>
void InlinePageTableEmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.ReadQ(args[2]);
        } else {
            return ctx.reg_alloc.ReadReg<std::max<std::size_t>(bitsize, 32)>(args[2]);
        }
    }();
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    const bool fpsr_loaded = ctx.fpsr.IsLoaded();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const auto [Xbase, Xoffset] = InlinePageTableEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
    EmitMemoryStr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered);

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fpsr_loaded, fallback, end] {
        code.l(*fallback);
        if (fpsr_loaded) {
            ctx.fpsr.EmitSpillKeepState();
        }
        if constexpr (bitsize == 128) {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Q0.B16(), Rvalue.B16());
        } else {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Xscratch1, Rvalue.toX());
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        EmitRelocation(code, ctx, WrappedWriteMemoryLinkTarget(bitsize));
        if (fpsr_loaded) {
            ctx.fpsr.EmitReZero();
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

std::optional<DoNotFastmemMarker> ShouldFastmem(EmitContext& ctx, IR::Inst* inst) {
    if (!ctx.conf.fastmem_pointer || !ctx.fastmem.SupportsFastmem()) {
        return std::nullopt;
    }

    const auto marker = std::make_tuple(ctx.block.Location(), inst->GetName());
    if (ctx.fastmem.ShouldFastmem(marker)) {
        return marker;
    }
    return std::nullopt;
}

inline bool ShouldExt32(EmitContext& ctx) {
    return ctx.conf.fastmem_address_space_bits == 32 && ctx.conf.silently_mirror_fastmem;
}

// May use Xscratch0 as scratch register
// Address to read/write = [ret0 + ret1], ret0 is always Xfastmem and ret1 is either Xaddr or Xscratch0
// Trashes NZCV
template<size_t bitsize>
std::pair<oaknut::XReg, oaknut::XReg> FastmemEmitVAddrLookup(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback) {
    if (ctx.conf.fastmem_address_space_bits == 64 || ShouldExt32(ctx)) {
        return std::make_pair(Xfastmem, Xaddr);
    }

    if (ctx.conf.silently_mirror_fastmem) {
        code.UBFX(Xscratch0, Xaddr, 0, ctx.conf.fastmem_address_space_bits);
        return std::make_pair(Xfastmem, Xscratch0);
    }

    code.LSR(Xscratch0, Xaddr, ctx.conf.fastmem_address_space_bits);
    code.CBNZ(Xscratch0, *fallback);
    return std::make_pair(Xfastmem, Xaddr);
}

template<size_t bitsize>
void FastmemEmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, DoNotFastmemMarker marker) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.WriteQ(inst);
        } else {
            return ctx.reg_alloc.WriteReg<std::max<std::size_t>(bitsize, 32)>(inst);
        }
    }();
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    // As the inline page-table path: a fault lands on the fallback, which saves FPSR around the call.
    const bool fpsr_loaded = ctx.fpsr.IsLoaded();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
    const auto fastmem_location = EmitMemoryLdr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered, ShouldExt32(ctx));

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, marker, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fpsr_loaded, fallback, end, fastmem_location] {
        ctx.ebi.fastmem_patch_info.emplace(
            fastmem_location - ctx.ebi.entry_point,
            FastmemPatchInfo{
                .marker = marker,
                .fc = FakeCall{
                    .call_pc = mcl::bit_cast<u64>(code.xptr<void*>()),
                },
                .recompile = ctx.conf.recompile_on_fastmem_failure,
            });

        code.l(*fallback);
        if (fpsr_loaded) {
            ctx.fpsr.EmitSpillKeepState();
        }
        code.MOV(Xscratch0, Xaddr);
        EmitRelocation(code, ctx, WrappedReadMemoryLinkTarget(bitsize));
        if (fpsr_loaded) {
            ctx.fpsr.EmitReZero();
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        if constexpr (bitsize == 128) {
            code.MOV(Rvalue.B16(), Q0.B16());
        } else {
            code.MOV(Rvalue.toX(), Xscratch0);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

template<size_t bitsize>
void FastmemEmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, DoNotFastmemMarker marker) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.ReadQ(args[2]);
        } else {
            return ctx.reg_alloc.ReadReg<std::max<std::size_t>(bitsize, 32)>(args[2]);
        }
    }();
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    const bool fpsr_loaded = ctx.fpsr.IsLoaded();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
    const auto fastmem_location = EmitMemoryStr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered, ShouldExt32(ctx));

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, marker, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fpsr_loaded, fallback, end, fastmem_location] {
        ctx.ebi.fastmem_patch_info.emplace(
            fastmem_location - ctx.ebi.entry_point,
            FastmemPatchInfo{
                .marker = marker,
                .fc = FakeCall{
                    .call_pc = mcl::bit_cast<u64>(code.xptr<void*>()),
                },
                .recompile = ctx.conf.recompile_on_fastmem_failure,
            });

        code.l(*fallback);
        if (fpsr_loaded) {
            ctx.fpsr.EmitSpillKeepState();
        }
        if constexpr (bitsize == 128) {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Q0.B16(), Rvalue.B16());
        } else {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Xscratch1, Rvalue.toX());
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        EmitRelocation(code, ctx, WrappedWriteMemoryLinkTarget(bitsize));
        if (fpsr_loaded) {
            ctx.fpsr.EmitReZero();
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

// A pair (VfpMemoryRole) is served by one 64-bit host access on the page-table path, or on the fastmem path
// with a 32-bit guest address space. Elsewhere its two accesses are emitted as usual.
bool CanPairOnPageTable(const EmitContext& ctx) {
    return ctx.conf.page_table_pointer != 0 && !ctx.conf.absolute_offset_page_table && (ctx.conf.detect_misaligned_access_via_page_table & (32 | 64)) == 0;
}

std::optional<DoNotFastmemMarker> PairFastmemMarker(EmitContext& ctx, IR::Inst* lo) {
    return ShouldExt32(ctx) ? ShouldFastmem(ctx, lo) : std::nullopt;
}

// Emits the 64-bit access for a pair at Xaddr, or a branch to fallback when the page table path can't
// serve both words (a page miss, or a pair that crosses into the next page) or the fastmem pair would
// run past the 4 GiB arena. Returns the access's location for fastmem patching.
template<typename T>
CodePtr EmitPairAccess(oaknut::CodeGenerator& code, EmitContext& ctx, bool is_read, bool fastmem, oaknut::XReg Xaddr, T Rvalue, const SharedLabel& fallback) {
    if (fastmem) {
        code.CMN(Xaddr.toW(), 7);
        code.B(CS, *fallback);
    } else {
        const auto [Xbase, Xoffset] = InlinePageTableEmitVAddrLookup<64>(code, ctx, Xaddr, fallback);
        ASSERT(Xbase.index() == Xscratch0.index() && Xoffset.index() == Xscratch1.index());
        code.CMP(Xscratch1, page_size - 8);
        code.B(HI, *fallback);
    }

    const CodePtr location = code.xptr<CodePtr>();
    const auto Xbase = fastmem ? Xfastmem : Xscratch0;
    const auto Roffset = fastmem ? oaknut::RReg{Xaddr.toW()} : oaknut::RReg{Xscratch1};
    const auto ext = fastmem ? oaknut::IndexExt::UXTW : oaknut::IndexExt::LSL;
    if (is_read) {
        code.LDR(Rvalue, Xbase, Roffset, ext);
    } else {
        code.STR(Rvalue, Xbase, Roffset, ext);
    }
    return location;
}

void AddFastmemPatch(oaknut::CodeGenerator& code, EmitContext& ctx, CodePtr location, DoNotFastmemMarker marker) {
    ctx.ebi.fastmem_patch_info.emplace(
        location - ctx.ebi.entry_point,
        FastmemPatchInfo{
            .marker = marker,
            .fc = FakeCall{
                .call_pc = mcl::bit_cast<u64>(code.xptr<void*>()),
            },
            .recompile = ctx.conf.recompile_on_fastmem_failure,
        });
}

// PairReadLo: both words into one FPR (the low word is lo's value). The fallback reads them one at a time
// through the 32-bit callbacks, in the order the two accesses had.
void EmitPairedReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* lo) {
    auto args = ctx.reg_alloc.GetArgumentInfo(lo);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Dvalue = ctx.reg_alloc.WriteD(lo);
    const bool fpsr_loaded = ctx.fpsr.IsLoaded();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Dvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();
    const auto marker = PairFastmemMarker(ctx, lo);
    const CodePtr location = EmitPairAccess(code, ctx, true, marker.has_value(), *Xaddr, *Dvalue, fallback);

    ctx.deferred_emits.emplace_back([&code, &ctx, lo, marker, location, Xaddr = *Xaddr, Dvalue = *Dvalue, fpsr_loaded, fallback, end] {
        if (marker) {
            AddFastmemPatch(code, ctx, location, *marker);
        }

        code.l(*fallback);
        if (fpsr_loaded) {
            ctx.fpsr.EmitSpillKeepState();
        }
        code.MOV(Xscratch0, Xaddr);
        EmitRelocation(code, ctx, LinkTarget::WrappedReadMemory32);
        if (fpsr_loaded) {
            ctx.fpsr.EmitReZero();
        }
        code.FMOV(Dvalue.toS(), Wscratch0);
        ctx.conf.emit_check_memory_abort(code, ctx, lo, *end);
        code.ADD(Wscratch0, Xaddr.toW(), 4);
        EmitRelocation(code, ctx, LinkTarget::WrappedReadMemory32);
        if (fpsr_loaded) {
            ctx.fpsr.EmitReZero();
        }
        code.MOV(Dvalue.Selem()[1], Wscratch0);
        ctx.conf.emit_check_memory_abort(code, ctx, lo, *end);
        code.B(*end);
    });

    code.l(*end);
}

// PairWriteLo: both words from the 64-bit value's register. The fallback writes them one at a time through
// the 32-bit callbacks, in the order the two accesses had.
void EmitPairedWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* lo) {
    auto args = ctx.reg_alloc.GetArgumentInfo(lo);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);

    const auto emit = [&](auto& Rvalue) {
        constexpr bool is_fpr = std::is_same_v<std::decay_t<decltype(*Rvalue)>, oaknut::DReg>;

        const bool fpsr_loaded = ctx.fpsr.IsLoaded();
        ctx.reg_alloc.SpillFlags();
        RegAlloc::Realize(Xaddr, Rvalue);

        SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();
        const auto marker = PairFastmemMarker(ctx, lo);
        const CodePtr location = EmitPairAccess(code, ctx, false, marker.has_value(), *Xaddr, *Rvalue, fallback);

        ctx.deferred_emits.emplace_back([&code, &ctx, lo, marker, location, Xaddr = *Xaddr, Rvalue = *Rvalue, fpsr_loaded, fallback, end] {
            if (marker) {
                AddFastmemPatch(code, ctx, location, *marker);
            }

            code.l(*fallback);
            if (fpsr_loaded) {
                ctx.fpsr.EmitSpillKeepState();
            }
            code.MOV(Xscratch0, Xaddr);
            if constexpr (is_fpr) {
                code.FMOV(Wscratch1, Rvalue.toS());
            } else {
                code.MOV(Wscratch1, Rvalue.toW());
            }
            EmitRelocation(code, ctx, LinkTarget::WrappedWriteMemory32);
            if (fpsr_loaded) {
                ctx.fpsr.EmitReZero();
            }
            ctx.conf.emit_check_memory_abort(code, ctx, lo, *end);
            code.ADD(Wscratch0, Xaddr.toW(), 4);
            if constexpr (is_fpr) {
                code.UMOV(Wscratch1, Rvalue.Selem()[1]);
            } else {
                code.LSR(Xscratch1, Rvalue, 32);
            }
            EmitRelocation(code, ctx, LinkTarget::WrappedWriteMemory32);
            if (fpsr_loaded) {
                ctx.fpsr.EmitReZero();
            }
            ctx.conf.emit_check_memory_abort(code, ctx, lo, *end);
            code.B(*end);
        });

        code.l(*end);
    };

    if (args[2].IsInGpr()) {
        auto Xvalue = ctx.reg_alloc.ReadX(args[2]);
        emit(Xvalue);
    } else {
        auto Dvalue = ctx.reg_alloc.ReadD(args[2]);
        emit(Dvalue);
    }
}

IR::AccType AccTypeOf(const IR::Inst* access) {
    return access->GetArg(access->NumArgs() - 1).GetAccType();
}

// Same guest instruction, same unordered access type.
bool IsSameAccess(const IR::Inst* a, const IR::Inst* b) {
    return a->GetArg(0).GetU64() == b->GetArg(0).GetU64() && AccTypeOf(a) == AccTypeOf(b) && !IsOrdered(AccTypeOf(a));
}

// Whether hi is lo + 4 as the frontend computes it: Add32(lo, 4), or both an immediate or Add32(base, imm)
// offset from the same base.
bool IsNextWord(const IR::Value& lo, const IR::Value& hi) {
    struct Split {
        const IR::Inst* base;  // nullptr: an immediate address
        u32 offset;
    };
    const auto split = [](const IR::Value& address) -> Split {
        if (address.IsImmediate()) {
            return {nullptr, static_cast<u32>(address.GetImmediateAsU64())};
        }
        const IR::Inst* inst = address.GetInst();
        if (inst->GetOpcode() == IR::Opcode::Add32 && !inst->GetArg(0).IsImmediate() && inst->GetArg(1).IsImmediate() && inst->GetArg(2).IsImmediate() && !inst->GetArg(2).GetU1()) {
            return {inst->GetArg(0).GetInst(), inst->GetArg(1).GetU32()};
        }
        return {inst, 0};
    };

    const Split l = split(lo), h = split(hi);
    if (!lo.IsImmediate() && h.base == lo.GetInst() && h.offset == 4) {
        return true;
    }
    return l.base == h.base && h.offset == l.offset + 4;
}

// What may sit between a pair's two accesses: address arithmetic, which reading or writing the second word
// early can't affect.
bool IsAddressArithmetic(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::Add32 || inst.GetOpcode() == IR::Opcode::Sub32;
}

// The first instruction after from that isn't address arithmetic, or nullptr.
IR::Inst* NextAccess(IR::Block& block, IR::Inst* from) {
    for (auto iter = std::next(IR::Block::iterator{from}); iter != block.end(); ++iter) {
        if (!IsAddressArithmetic(*iter)) {
            return &*iter;
        }
    }
    return nullptr;
}

}  // namespace

void AssignVfpMemoryRoles(EmitContext& ctx) {
    auto& roles = ctx.vfp_memory;
    roles.clear();

    size_t max_name = 0;
    for (const auto& inst : ctx.block) {
        max_name = std::max<size_t>(max_name, inst.GetName());
    }
    roles.resize(max_name + 1);

    const auto set = [&](const IR::Inst* inst, VfpMemoryRole role, const IR::Inst* partner = nullptr) {
        roles[inst->GetName()] = {role, partner};
    };
    const auto is_unassigned = [&](const IR::Inst* inst) {
        return inst->GetName() != 0 && roles[inst->GetName()].role == VfpMemoryRole::None;
    };
    const auto only_use_is = [](const IR::Value& value, IR::Opcode opcode) {
        return !value.IsImmediate() && value.GetInst()->GetOpcode() == opcode && value.GetInst()->UseCount() == 1;
    };

    const bool page_table = CanPairOnPageTable(ctx);
    const auto can_pair = [&](IR::Inst* lo) {
        return page_table || PairFastmemMarker(ctx, lo).has_value();
    };
    // The paired access computes the second address itself.
    const auto drop_hi_address = [&](const IR::Inst* hi) {
        if (only_use_is(hi->GetArg(1), IR::Opcode::Add32) && is_unassigned(hi->GetArg(1).GetInst())) {
            set(hi->GetArg(1).GetInst(), VfpMemoryRole::PairHiAddress);
        }
    };

    for (auto& inst : ctx.block) {
        switch (inst.GetOpcode()) {
        case IR::Opcode::Pack2x32To1x64: {
            // %lo = A32ReadMemory32 a; [address arithmetic]; %hi = A32ReadMemory32 a+4; ...; Pack2x32To1x64 %lo, %hi
            if (!only_use_is(inst.GetArg(0), IR::Opcode::A32ReadMemory32) || !only_use_is(inst.GetArg(1), IR::Opcode::A32ReadMemory32)) {
                break;
            }
            IR::Inst* const lo = inst.GetArg(0).GetInst();
            IR::Inst* const hi = inst.GetArg(1).GetInst();
            if (!is_unassigned(lo) || !is_unassigned(hi) || !is_unassigned(&inst) || NextAccess(ctx.block, lo) != hi) {
                break;
            }
            if (!IsSameAccess(lo, hi) || !IsNextWord(lo->GetArg(1), hi->GetArg(1)) || !can_pair(lo)) {
                break;
            }
            set(lo, VfpMemoryRole::PairReadLo);
            set(hi, VfpMemoryRole::PairReadHi, lo);
            set(&inst, VfpMemoryRole::PairReadPack);
            drop_hi_address(hi);
            break;
        }
        case IR::Opcode::A32WriteMemory32: {
            const IR::Value value = inst.GetArg(2);
            if (only_use_is(value, IR::Opcode::A32GetExtendedRegister32) && is_unassigned(value.GetInst())) {
                set(value.GetInst(), VfpMemoryRole::StoreValueToGpr);
                break;
            }

            // A32WriteMemory32 a, LeastSignificantWord %d; [address arithmetic]; A32WriteMemory32 a+4, MostSignificantWord %d
            if (!only_use_is(value, IR::Opcode::LeastSignificantWord) || value.GetInst()->GetArg(0).IsImmediate()) {
                break;
            }
            IR::Inst* const lo = &inst;
            IR::Inst* const hi = NextAccess(ctx.block, lo);
            if (!hi || hi->GetOpcode() != IR::Opcode::A32WriteMemory32 || !only_use_is(hi->GetArg(2), IR::Opcode::MostSignificantWord)) {
                break;
            }
            IR::Inst* const lsw = value.GetInst();
            IR::Inst* const msw = hi->GetArg(2).GetInst();
            if (msw->GetArg(0).IsImmediate() || msw->GetArg(0).GetInst() != lsw->GetArg(0).GetInst()) {
                break;
            }
            if (!is_unassigned(lo) || !is_unassigned(hi) || !is_unassigned(lsw) || !is_unassigned(msw)) {
                break;
            }
            if (!IsSameAccess(lo, hi) || !IsNextWord(lo->GetArg(1), hi->GetArg(1)) || !can_pair(lo)) {
                break;
            }
            set(lsw, VfpMemoryRole::PairWriteLsw);
            set(msw, VfpMemoryRole::PairWriteMsw);
            set(lo, VfpMemoryRole::PairWriteLo);
            set(hi, VfpMemoryRole::PairWriteHi);
            drop_hi_address(hi);
            break;
        }
        default:
            break;
        }
    }
}

template<size_t bitsize>
void EmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if constexpr (bitsize == 32) {
        switch (const auto info = ctx.VfpMemory(inst); info.role) {
        case VfpMemoryRole::PairReadLo:
            EmitPairedReadMemory(code, ctx, inst);
            return;
        case VfpMemoryRole::PairReadHi:
            ctx.reg_alloc.GetArgumentInfo(inst);
            ctx.reg_alloc.DefineAsExisting(inst, info.partner);
            return;
        default:
            break;
        }
    }

    if (const auto marker = ShouldFastmem(ctx, inst)) {
        FastmemEmitReadMemory<bitsize>(code, ctx, inst, *marker);
    } else if (ctx.conf.page_table_pointer != 0) {
        InlinePageTableEmitReadMemory<bitsize>(code, ctx, inst);
    } else {
        CallbackOnlyEmitReadMemory<bitsize>(code, ctx, inst);
    }
}

template<size_t bitsize>
void EmitExclusiveReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    CallbackOnlyEmitExclusiveReadMemory<bitsize>(code, ctx, inst);
}

template<size_t bitsize>
void EmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if constexpr (bitsize == 32) {
        switch (ctx.VfpMemory(inst).role) {
        case VfpMemoryRole::PairWriteLo:
            EmitPairedWriteMemory(code, ctx, inst);
            return;
        case VfpMemoryRole::PairWriteHi:
            ctx.reg_alloc.GetArgumentInfo(inst);
            return;
        default:
            break;
        }
    }

    if (const auto marker = ShouldFastmem(ctx, inst)) {
        FastmemEmitWriteMemory<bitsize>(code, ctx, inst, *marker);
    } else if (ctx.conf.page_table_pointer != 0) {
        InlinePageTableEmitWriteMemory<bitsize>(code, ctx, inst);
    } else {
        CallbackOnlyEmitWriteMemory<bitsize>(code, ctx, inst);
    }
}

template<size_t bitsize>
void EmitExclusiveWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    CallbackOnlyEmitExclusiveWriteMemory<bitsize>(code, ctx, inst);
}

template void EmitReadMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);

}  // namespace Dynarmic::Backend::Arm64
