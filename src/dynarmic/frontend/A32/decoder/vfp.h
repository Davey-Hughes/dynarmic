/* This file is part of the dynarmic project.
 * Copyright (c) 2032 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <vector>

#include <mcl/stdint.hpp>

#include "dynarmic/frontend/A32/decoder/arm.h"
#include "dynarmic/frontend/decoder/decoder_detail.h"
#include "dynarmic/frontend/decoder/matcher.h"

namespace Dynarmic::A32 {

template<typename Visitor>
using VFPMatcher = Decoder::Matcher<Visitor, u32>;

template<typename Visitor>
struct VFPDecodeTable {
    using Bucket = std::vector<const VFPMatcher<Visitor>*>;

    std::vector<VFPMatcher<Visitor>> matchers;
    std::array<Bucket, 0x1000> unconditional;
    std::array<Bucket, 0x1000> conditional;
};

template<typename V>
VFPDecodeTable<V> GetVFPDecodeTable() {
    VFPDecodeTable<V> table;
    table.matchers = {

#define INST(fn, name, bitstring) DYNARMIC_DECODER_GET_MATCHER(VFPMatcher, fn, name, Decoder::detail::StringToArray<32>(bitstring)),
#include "./vfp.inc"
#undef INST

    };

    const auto is_unconditional = [](const auto& matcher) {
        return (matcher.GetMask() & 0xF0000000) == 0xF0000000;
    };
    std::stable_partition(table.matchers.begin(), table.matchers.end(), is_unconditional);

    // Matchers stay in table order within each bucket.
    for (size_t i = 0; i < table.unconditional.size(); ++i) {
        for (const auto& matcher : table.matchers) {
            const auto expect = detail::ToFastLookupIndexArm(matcher.GetExpected());
            const auto mask = detail::ToFastLookupIndexArm(matcher.GetMask());
            if ((i & mask) == expect) {
                (is_unconditional(matcher) ? table.unconditional : table.conditional)[i].push_back(&matcher);
            }
        }
    }
    return table;
}

template<typename V>
std::optional<std::reference_wrapper<const VFPMatcher<V>>> DecodeVFP(u32 instruction) {
    static const auto table = GetVFPDecodeTable<V>();

    const bool is_unconditional = (instruction & 0xF0000000) == 0xF0000000;
    const auto& buckets = is_unconditional ? table.unconditional : table.conditional;
    const auto& bucket = buckets[detail::ToFastLookupIndexArm(instruction)];

    const auto iter = std::find_if(bucket.begin(), bucket.end(), [instruction](const auto* matcher) { return matcher->Matches(instruction); });
    return iter != bucket.end() ? std::optional<std::reference_wrapper<const VFPMatcher<V>>>(**iter) : std::nullopt;
}

}  // namespace Dynarmic::A32
