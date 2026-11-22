/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/interface/A32/config.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/opt/passes.h"

namespace Dynarmic::Optimization {

void A32ConstantMemoryReads(IR::Block& block, A32::UserCallbacks* cb) {
    // Reads the frontend did not record with AddConstantReadCandidate() cannot have an immediate
    // address yet, so only the instructions from the first recorded read to the last are visited.
    IR::Inst* const last = block.LastConstantReadCandidate();
    if (!last) {
        return;
    }

    for (auto iter = IR::Block::iterator{block.FirstConstantReadCandidate()};; ++iter) {
        IR::Inst& inst = *iter;
        switch (inst.GetOpcode()) {
        case IR::Opcode::A32ReadMemory8: {
            if (!inst.AreAllArgsImmediates()) {
                break;
            }

            const u32 vaddr = inst.GetArg(1).GetU32();
            if (cb->IsReadOnlyMemory(vaddr)) {
                const u8 value_from_memory = cb->MemoryRead8(vaddr);
                inst.ReplaceUsesWith(IR::Value{value_from_memory});
            }
            break;
        }
        case IR::Opcode::A32ReadMemory16: {
            if (!inst.AreAllArgsImmediates()) {
                break;
            }

            const u32 vaddr = inst.GetArg(1).GetU32();
            if (cb->IsReadOnlyMemory(vaddr)) {
                const u16 value_from_memory = cb->MemoryRead16(vaddr);
                inst.ReplaceUsesWith(IR::Value{value_from_memory});
            }
            break;
        }
        case IR::Opcode::A32ReadMemory32: {
            if (!inst.AreAllArgsImmediates()) {
                break;
            }

            const u32 vaddr = inst.GetArg(1).GetU32();
            if (cb->IsReadOnlyMemory(vaddr)) {
                const u32 value_from_memory = cb->MemoryRead32(vaddr);
                inst.ReplaceUsesWith(IR::Value{value_from_memory});
            }
            break;
        }
        case IR::Opcode::A32ReadMemory64: {
            if (!inst.AreAllArgsImmediates()) {
                break;
            }

            const u32 vaddr = inst.GetArg(1).GetU32();
            if (cb->IsReadOnlyMemory(vaddr)) {
                const u64 value_from_memory = cb->MemoryRead64(vaddr);
                inst.ReplaceUsesWith(IR::Value{value_from_memory});
            }
            break;
        }
        default:
            break;
        }

        if (&inst == last) {
            break;
        }
    }
}

}  // namespace Dynarmic::Optimization
