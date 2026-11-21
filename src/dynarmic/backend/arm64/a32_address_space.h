/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include "dynarmic/backend/arm64/address_space.h"
#include "dynarmic/backend/block_range_information.h"
#include "dynarmic/interface/A32/config.h"

namespace Dynarmic::Backend::Arm64 {

struct EmittedBlockInfo;

class A32AddressSpace final : public AddressSpace {
public:
    explicit A32AddressSpace(const A32::UserConfig& conf);

    IR::Block GenerateIR(IR::LocationDescriptor) const override;

    void InvalidateCacheRanges(const boost::icl::interval_set<u32>& ranges);

protected:
    friend class A32Core;

    void EmitPrelude();
    EmitConfig MakeEmitConfig() const;
    const EmitConfig& GetEmitConfig() const override { return emit_conf; }
    void RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) override;

    const A32::UserConfig conf;
    // Made once: a copy per block would also copy and release the coprocessor pointers.
    const EmitConfig emit_conf;
    BlockRangeInformation<u32> block_ranges;
};

}  // namespace Dynarmic::Backend::Arm64
