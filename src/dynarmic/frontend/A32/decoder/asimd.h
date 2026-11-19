/* This file is part of the dynarmic project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <set>
#include <vector>

#include <mcl/bit/bit_count.hpp>
#include <mcl/stdint.hpp>

#include "dynarmic/frontend/A32/decoder/arm.h"
#include "dynarmic/frontend/decoder/decoder_detail.h"
#include "dynarmic/frontend/decoder/matcher.h"

namespace Dynarmic::A32 {

template<typename Visitor>
using ASIMDMatcher = Decoder::Matcher<Visitor, u32>;

template<typename V>
std::vector<ASIMDMatcher<V>> GetASIMDDecodeTable() {
    std::vector<ASIMDMatcher<V>> table = {

#define INST(fn, name, bitstring) DYNARMIC_DECODER_GET_MATCHER(ASIMDMatcher, fn, name, Decoder::detail::StringToArray<32>(bitstring)),
#include "./asimd.inc"
#undef INST

    };

    // Exceptions to the rule of thumb.
    const std::set<std::string> comes_first{
        "VBIC, VMOV, VMVN, VORR (immediate)",
        "VEXT",
        "VTBL",
        "VTBX",
        "VDUP (scalar)",
    };
    const std::set<std::string> comes_last{
        "VMLA (scalar)",
        "VMLAL (scalar)",
        "VQDMLAL/VQDMLSL (scalar)",
        "VMUL (scalar)",
        "VMULL (scalar)",
        "VQDMULL (scalar)",
        "VQDMULH (scalar)",
        "VQRDMULH (scalar)",
    };
    const auto sort_begin = std::stable_partition(table.begin(), table.end(), [&](const auto& matcher) {
        return comes_first.count(matcher.GetName()) > 0;
    });
    const auto sort_end = std::stable_partition(table.begin(), table.end(), [&](const auto& matcher) {
        return comes_last.count(matcher.GetName()) == 0;
    });

    // If a matcher has more bits in its mask it is more specific, so it should come first.
    std::stable_sort(sort_begin, sort_end, [](const auto& matcher1, const auto& matcher2) {
        return mcl::bit::count_ones(matcher1.GetMask()) > mcl::bit::count_ones(matcher2.GetMask());
    });

    return table;
}

template<typename Visitor>
struct ASIMDDecodeBuckets {
    using Bucket = std::vector<const ASIMDMatcher<Visitor>*>;

    ASIMDDecodeBuckets() = default;
    // The buckets point into matchers, so a copy's would point into the original. A move keeps matchers' storage.
    ASIMDDecodeBuckets(const ASIMDDecodeBuckets&) = delete;
    ASIMDDecodeBuckets& operator=(const ASIMDDecodeBuckets&) = delete;
    ASIMDDecodeBuckets(ASIMDDecodeBuckets&&) = default;

    std::vector<ASIMDMatcher<Visitor>> matchers;
    std::array<Bucket, 0x1000> buckets;
};

template<typename V>
ASIMDDecodeBuckets<V> GetASIMDDecodeBuckets() {
    ASIMDDecodeBuckets<V> table;
    table.matchers = GetASIMDDecodeTable<V>();

    // Matchers stay in table order within each bucket.
    for (size_t i = 0; i < table.buckets.size(); ++i) {
        for (const auto& matcher : table.matchers) {
            const auto expect = detail::ToFastLookupIndexArm(matcher.GetExpected());
            const auto mask = detail::ToFastLookupIndexArm(matcher.GetMask());
            if ((i & mask) == expect) {
                table.buckets[i].push_back(&matcher);
            }
        }
    }
    return table;
}

template<typename V>
std::optional<std::reference_wrapper<const ASIMDMatcher<V>>> DecodeASIMD(u32 instruction) {
    static const auto table = GetASIMDDecodeBuckets<V>();

    const auto& bucket = table.buckets[detail::ToFastLookupIndexArm(instruction)];

    const auto iter = std::find_if(bucket.begin(), bucket.end(), [instruction](const auto* matcher) { return matcher->Matches(instruction); });
    return iter != bucket.end() ? std::optional<std::reference_wrapper<const ASIMDMatcher<V>>>(**iter) : std::nullopt;
}

}  // namespace Dynarmic::A32
