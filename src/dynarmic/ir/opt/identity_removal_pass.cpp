/* This file is part of the dynarmic project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <boost/container/small_vector.hpp>

#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/opt/passes.h"

namespace Dynarmic::Optimization {

namespace {

// Points each of inst's arguments past the identities it reads through.
void SkipIdentityArgs(IR::Inst& inst) {
    const size_t num_args = inst.NumArgs();
    for (size_t i = 0; i < num_args; i++) {
        while (true) {
            IR::Value arg = inst.GetArg(i);
            if (!arg.IsIdentity())
                break;
            inst.SetArg(i, arg.GetInst()->GetArg(0));
        }
    }
}

bool HasIdentityArg(const IR::Inst& inst) {
    const size_t num_args = inst.NumArgs();
    for (size_t i = 0; i < num_args; i++) {
        if (inst.GetArg(i).IsIdentity()) {
            return true;
        }
    }
    return false;
}

}  // namespace

void IdentityRemovalPass(IR::Block& block) {
    // On the stack for most blocks, rather than grown on the heap for each one.
    boost::container::small_vector<IR::Inst*, 64> to_invalidate;

    auto iter = block.begin();
    while (iter != block.end()) {
        IR::Inst& inst = *iter;

        SkipIdentityArgs(inst);

        if (inst.GetOpcode() == IR::Opcode::Identity) {
            iter = block.Instructions().erase(inst);
            to_invalidate.push_back(&inst);
        } else if (inst.GetOpcode() == IR::Opcode::Void) {
            // Invalidated by an earlier pass, so it has no arguments left to release.
            iter = block.Instructions().erase(inst);
        } else {
            ++iter;
        }
    }

    for (IR::Inst* inst : to_invalidate) {
        inst->Invalidate();
    }
}

void DeadCodeEliminationThenIdentityRemoval(IR::Block& block) {
    // DeadCodeElimination followed by IdentityRemovalPass with one walk of the block. The reverse walk does the
    // first and takes identities and invalidated instructions out of the list. Use counts are left as the
    // first pass leaves them until the walk ends; then the instructions that read an identity are pointed past
    // it in program order, and the identities are invalidated, as IdentityRemovalPass does.
    boost::container::small_vector<IR::Inst*, 32> reads_identity;
    boost::container::small_vector<IR::Inst*, 32> identities;

    auto iter = block.end();
    while (iter != block.begin()) {
        IR::Inst& inst = *--iter;

        if (!inst.HasUses() && !inst.MayHaveSideEffects()) {
            inst.Invalidate();
        }

        if (HasIdentityArg(inst)) {
            reads_identity.push_back(&inst);
        }

        if (inst.GetOpcode() == IR::Opcode::Identity) {
            identities.push_back(&inst);
            iter = block.Instructions().erase(inst);
        } else if (inst.GetOpcode() == IR::Opcode::Void) {
            iter = block.Instructions().erase(inst);
        }
    }

    for (auto inst = reads_identity.rbegin(); inst != reads_identity.rend(); ++inst) {
        SkipIdentityArgs(**inst);
    }
    for (auto inst = identities.rbegin(); inst != identities.rend(); ++inst) {
        (*inst)->Invalidate();
    }
}

}  // namespace Dynarmic::Optimization
